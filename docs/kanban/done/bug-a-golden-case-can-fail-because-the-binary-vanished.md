---
id: bug-a-golden-case-can-fail-because-the-binary-vanished
title: A golden case can fail because the binary vanished
arc: bug
size: S
verification: golden-11, scripts-fail
---

# bug-a-golden-case-can-fail-because-the-binary-vanished — A golden case can fail because the binary vanished

`scripts/golden.sh` reported `FAIL lit` and `1 of 11 cases differ` during G17's verification.
Nothing rendered. The tail of `debug_frames/golden/lit.log` was:

```
[41/43] Linking CXX executable demo
...
==> done: build/release/demo
error: build/release/demo still does not exist after building.
```

That last line is `run.sh`'s guard at [`run.sh:152`](../../../run.sh#L152) firing immediately
after `build_game.sh` returned successfully. An immediate re-run gave `all 11 cases match`.

**Both halves of this are wrong, and the second is what makes the first expensive.**

`build.sh` runs under `set -euo pipefail`, so a failed `cmake --build` cannot reach the
`==> done:` line — ninja exited zero. Yet `[ ! -x "$BIN" ]` was true one statement later. So
either the link genuinely had not landed when the test ran, or something removed it; neither
is explained, and `==> done: $BUILD_DIR/$SUBSTRATE_GAME` at
[`build.sh:91`](../../../build.sh#L91) is printed unconditionally rather than derived from the
file existing, so the message actively asserts something nobody checked.

The second half: **re-running the suite overwrote `lit.log`**, so the evidence was gone before
it could be read. `debug_frames/golden/failed/3/` was kept, but it holds the *stale* artifacts
from a previous run — an `actual.png` from a case that never drew. A harness that discards the
log of a failure and keeps a misleading image in its place is worse than one that keeps
nothing, because the kept image invites a pixel diff of two irrelevant files.

A golden FAIL is stop-the-line, and this one cost an investigation to classify as "not a
rendering change". That is the cost worth removing: a flake that looks exactly like a
regression until the log is read, and whose log does not survive.

## Verification

- Reproduce or bound it: run `scripts/golden.sh` in a loop overnight, or under a concurrent
  build against the same `build/release`, and count the failures. A cause nobody can trigger
  is a cause nobody can fix, so this may end as a detection row rather than a fix row.
- `scripts/golden.sh` still 11 of 11 with the harness change in place.
- A deliberately broken build (delete `build/release/demo` mid-run, or point `$BIN` at
  nothing) reports as a **harness** failure and not as a differing case, and keeps its log.

## Reference update

[architecture/tooling.md](../../architecture/tooling.md) — the golden suite section, which
states what a FAIL means and does not distinguish "the image changed" from "the run never
happened".

## Outcome

**It was not a flake and it did not end as a detection row.** The card allowed for "a cause
nobody can trigger", and the cause turned out to be one `strace` away: **the linker unlinks
its output before it writes it** — `unlink("out")` then `openat(..., O_RDWR|O_CREAT|O_TRUNC)`
— so for the length of a link of a 45 MB executable, `build/release/demo` does not exist for
anybody. Ninja takes no lock, two sessions share this checkout, and `run.sh` builds
unconditionally on every launch. So a second session's `./run.sh demo release` starting its
link in the window between `build_game.sh` returning and `[ ! -x "$BIN" ]` running is the
whole of it, and it explains every part of the report: a `cmake --build` that genuinely
exited zero, a file gone one statement later, and an immediate re-run passing.

The hypothesis checked first was wrong and is worth recording so nobody re-checks it: an
engine-only `./build.sh release` reconfigures with `SUBSTRATE_GAME` empty, which drops `demo`
from the build graph — but ninja does **not** clean dead outputs on an ordinary build, and the
binary was still there afterwards, byte for byte and timestamp for timestamp.

What landed, in four scripts and no C++:

- **`build_lock` in `scripts/common.sh`** — `flock` on `build/<config>/.build.lock`, held on
  fd 9 for the life of the calling shell. `build.sh` takes it around configure-and-build;
  `run.sh` and `test.sh` take it *earlier* and hold it across the check for the binary,
  because the check is inside the window the lock exists to close. `SUBSTRATE_BUILD_LOCK`
  carries the held directory to children so `build.sh` does not queue behind its own caller.
  Both holders `exec 9>&-` before their `exec`: a shell-opened descriptor survives it, and a
  120-second capture or a TSan suite holding the lock would block every build in the tree.
  Verified by racing two `./build.sh release` — one printed `waiting for another build in
  build/release/` and both completed.
- **`build.sh` asserts what it announces.** `==> done: <artifact>` is now derived from the
  file existing, and a `cmake --build` that reports success without producing the artifact
  exits non-zero naming it. Only for a default build: `./build.sh debug --target shaders` is
  asked to produce something else and prints `==> done: build/debug (--target shaders)`.
- **`golden.sh` has three outcomes, not two.** `HARNESS` is a run that never reached a
  comparison; `FAIL` is one that compared an image and found it changed. The discriminator is
  the log rather than the exit code — everything upstream of the comparison also exits 1,
  while a genuine comparison always logs a `Compare:` verdict. A harness failure stops the
  suite and exits **2**, because whatever stopped one case stops the other ten identically
  and eleven of them read as a total regression.
- **Stale artifacts can no longer be kept.** Each case now `rm -f`s its own `actual.png`,
  `diff.png` and `.log` before running, so a case that never draws leaves nothing behind and
  the keep step files an absence rather than the previous run's correct image. The `.log` is
  copied on its own terms instead of inside a brace expansion whose failure was swallowed
  with the images'.
- **`test.sh` gained the check `run.sh` already had.** It exec'd `substrate_tests` without
  ever asking whether it existed — the same window, one script over.

`test.sh` was not in the card's scope and is in the fix because the defect is not about the
golden suite: it is that *any* script can exec a binary a concurrent link has just removed.
Fixing the one reported instance would have left the same trap under `./test.sh`.

## Verification results

- `scripts/golden.sh check release` — **all 11 cases match**, byte-identical, exit 0.
- `scripts-fail`, exactly as the card specified it: with `$BIN` pointed at nothing, the suite
  reports `HARNESS lit -- the run did not reach a comparison`, quotes the three tail lines of
  the log inline (including `error: build/release/demo-deliberately-missing still does not
  exist after building.`), stops after one case, exits **2**, and keeps
  `debug_frames/golden/failed/4/` holding `lit.log` and `lit.expected.png` and **no**
  `actual.png` — which is the half of the defect that made the first investigation expensive.
- `./test.sh debug` — **1007 tests, 103 suites, all passed**, run because `test.sh` changed.
- `bash -n` on all five modified scripts.
