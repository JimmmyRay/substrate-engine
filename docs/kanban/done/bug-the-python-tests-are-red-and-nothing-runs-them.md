---
id: bug-the-python-tests-are-red-and-nothing-runs-them
title: The python tests are red and nothing runs them
arc: bug
size: S
verification: scripts-fail
---

# bug-the-python-tests-are-red-and-nothing-runs-them — The python tests are red and nothing runs them

`tests/manifest_test.py` fails fifteen of twenty-four cases:

```
E  ValueError: too many values to unpack (expected 3)
```

`manifest.build` returns four values and the tests unpack three. Neither file is modified in
the working tree, so this is not a break in flight — it landed whenever the fourth value was
added, and has been red ever since.

**Nothing local runs them.** `./test.sh` builds and runs the gtest binary; `CMakeLists.txt`
has no python target; `scripts/fetch_assets.sh` *invokes* `make_composite_scene.py` but never
its test. `.github/workflows/ci.yml` does run all three — `manifest_test.py`,
`make_composite_scene_test.py` and `check_pins_test.py` — so the title overstates it and the
substance survives: a green `./test.sh` has never had anything to say about them, and a green
`./test.sh` is what anybody working here reads.

That is the same shape `closing-a-card` already warns about for `locomotion.sh` and
`readback.sh`: **a suite outside the gate goes red in silence.** The difference is that those
two are named by a `verification:` token, so a card can pull them in. These have no token and
no runner at all.

Two halves, and the second is the one that matters:

- Fix the unpack. Whatever the fourth value is, the tests should assert about it rather than
  drop it — a test updated by widening its tuple and ignoring the new element is how the next
  one goes quiet.
- **Give them a local runner.** `./test.sh` grows a python step, so the command a card's
  verification already names covers them, or this recurs the next time CI is not what somebody
  is looking at.

Found while retiring `showcase.gltf`: the row touched `manifest.py`'s inputs, ran the suite to
check, and found it had been failing for reasons that predate the row entirely.

## Verification

`scripts-fail`, which is the honest token: the check is that the runner **exits non-zero**
where it silently succeeded before.

- The python suites pass, run by whatever the second half settles on.
- Breaking one of them on purpose makes the gate red. A runner that cannot fail is what this
  card is about.

## Outcome

Both halves, and the second one is what stops it happening again.

The unpack is fixed at all twenty-four call sites, and `cold` is no longer dropped: a
`ColdCache` class now pins what the value means — a `.ktx2` sidecar that was never built is
reported under `--require-cache`, silent without it, and **not** a missing file either way,
which is the distinction the field exists for. Three cases, because a value re-added to a
tuple and then ignored is exactly how this became invisible the first time.

`./test.sh` runs every `tests/*_test.py` before the C++ binary, and only when no gtest
argument was given — a `--gtest_filter` says the caller wants one C++ case and not another
suite's output in front of it. Absent python is a warning and a skip rather than a failure,
for the reason `fetch_assets.sh` skips its generators: python is not a build dependency of the
engine and refusing to run the C++ tests without it would make it one.

The title is left as it was written and corrected in the body instead. CI *did* run these —
the claim that nothing did was wrong, and rewriting the title to match what was found would
hide that a card can be opened on a half-checked premise. What was true is the part that
mattered: the only runner was one nobody looks at while working.

## Verification results

`scripts-fail`, and it is the arm that matters here rather than a formality:

- `./test.sh debug` — `check_pins_test` 13, `make_composite_scene_test` 3 (24 subtests),
  `manifest_test` **27** (24 that were failing, plus the three new ones), then **970 tests from
  100 test suites**. All green.
- **The gate goes red.** A deliberately failing `tests/zz_gate_probe_test.py` made `./test.sh`
  exit **1**, and the C++ suite did not run at all — the failure stops the script rather than
  scrolling past. Probe deleted.
- Before this, the same command was green with fifteen python cases erroring underneath it.
