---
id: D7
title: The scripts and the build agree with themselves
arc: D
size: S
verification: golden-12, scripts-fail
---

# D7 — The scripts and the build agree with themselves

The row that makes every other row's verification trustworthy. All four items paid: the test suite is held to `${SUBSTRATE_WARNINGS}` rather than a retyped subset, all five root scripts refuse an unknown configuration instead of silently building debug, the Sponza guard moved inside the no-game branch, and [`scripts/common.sh`](../../../scripts/common.sh) holds the one copy of `usage`, `list_games` and the sanitizer environment — see below

## D7, and why it goes first

Three of the four items here were ways a check can pass without having checked. All four
are paid:

- ~~`CMakeLists.txt` retypes four warning flags for `substrate_tests` instead of using
  `${SUBSTRATE_WARNINGS}`, so the suite silently misses the two flags the list appends under
  `if(WIN32)`.~~ [`CMakeLists.txt:689`](../../../CMakeLists.txt#L689) applies the list itself. The
  file's own comment at [`:557`](../../../CMakeLists.txt#L557) had argued for one list on the
  grounds that "a game compiled to a laxer standard than the engine is a game that finds the
  engine's bugs later than the engine does" — the test suite was the target that argument did
  not name and needed most, in a suite built on every Windows build and, since wine was
  removed, never run.
- ~~`./run.sh releas` runs **debug**, silently, and passes `releas` to the game as a scene
  path. Neither `run.sh` nor `test.sh` has the `*)` branch `build.sh` has.~~ All four scripts
  refuse it now: [`run.sh:69`](../../../run.sh#L69) grew a catch-all when it learned to take a game
  name, [`test.sh:36`](../../../test.sh#L36) got the one it never had, and
  [`build.sh:58`](../../../build.sh#L58) and [`build_release.sh:99`](../../../build_release.sh#L99) had it
  all along — the second explaining why: "a build that quietly dropped the flag would hand
  back something that looks like what was asked for".
- ~~`run.sh` aborts unless the *engine's* Sponza is present, whatever game the build directory
  holds and whatever scene its config names.~~ The guard now sits inside the no-game branch
  ([`run.sh:181`](../../../run.sh#L181)), so the smallest game
  [`guides/making-a-game.md`](../../guides/making-a-game.md) documents runs without fetching an
  asset it never loads. Worth keeping as a pair with the item above: the same commit that
  taught `run.sh` about games had left the guard that assumes there are none, three lines
  under a comment that already said the scene is "used when no game was named".
- ~~The fourth is ordinary duplication: `usage()` copy-pasted across all five root scripts,
  `list_games()` across two, and the sanitizer-environment block across `run.sh` and
  `test.sh`.~~ [`scripts/common.sh`](../../../scripts/common.sh) holds one of each, sourced by all
  five. `usage()` keys off `BASH_SOURCE[-1]` so the shared copy still names the script the
  user actually ran.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- By its own failure modes: the scripts must exit non-zero where they
  silently succeeded before.

## Reference

[architecture/tooling.md](../../architecture/tooling.md).

## Outcome

Recorded above, under *D7, and why it goes first*.
