---
id: chore-the-golden-suite-runs-a-viewer-not-whichever-game-the-build-holds
title: The golden suite runs a viewer not whichever game the build holds
arc: chore
size: S
verification: golden, readback, scaffold
---

# chore-the-golden-suite-runs-a-viewer-not-whichever-game-the-build-holds — The golden suite runs a viewer not whichever game the build holds

Afterwards `game/viewer/` exists: a game whose `init` composes nothing and whose `configure`
states the lighting the golden baselines were captured against. `run.sh` builds *it* for the
no-game path, so `scripts/golden.sh` stops running whichever game the build directory happens
to hold. `Engine::sceneOverridden()` and `demoWorldApplies()` go with it, and every game in
the tree composes its world unconditionally.

## Why

`scripts/golden.sh` invokes `./run.sh "$CONFIG"` with no game name, and `run.sh` falls back to
the game the CMake cache holds ([run.sh:114](../../../run.sh#L114)). So the thirteen cases run
through `game/demo` — or through `game/battle_arena` if that is what was built last, which is
a different frame for the same command.

The cost lands in game code. `game/demo` gates its entire world on
`demoWorldApplies()` = `!e.sceneOverridden()` ([DemoWorld.cpp:294](../../../game/demo/DemoWorld.cpp#L294)),
which is a game asking whether somebody swapped its world out. C41 deleted the string
comparison that used to answer the question and left the question. `game/battle_arena` dropped
its copy and now composes unconditionally, which is right and is also why running `golden.sh`
against a battle_arena build directory would put an arena in thirteen baselines.

## What it takes

- `game/viewer/`: `configure` naming the sun, ambient, exposure and tonemap the baselines were
  captured with; `init` declaring `App.Quit` and installing the camera the demo installs.
  Nothing else — no world, no materials, no settings module.
- `run.sh`: the no-game path builds `viewer` rather than `cached_game()`. Naming a game still
  runs that game.
- `game/demo`: `demoWorldApplies` deleted, `DemoGame::init` imports Sponza unconditionally.
- `engine/Engine.h`: `sceneOverridden()` deleted.

**The risk is entirely in the camera and the lighting**, and it is a byte-identical check, so
it is self-answering. No golden case passes `--camera`, so the view comes from whatever
controller the game installed plus `Camera::frameBounds`; the viewer has to install the same
one. If a case moves, the viewer is not yet what the demo was, and the diff says which.

## Verification

- `scripts/golden.sh check release` — thirteen cases, byte-identical, run against a build
  directory holding `battle_arena` as well as one holding `demo`. The second is the point.
- `scripts/readback.sh` — nine cases, unchanged; it goes through the same `run.sh` path.
- `scaffold`: `./new_game.sh` still produces something that builds and runs.

## Outcome

`game/viewer/` is 80 lines: a `configure` naming the demo's sun, ambient, exposure and mix
graph, and an `init` that declares Escape, installs a `FlyCamera` and places the fallback point
lights a file with none of its own gets. `run.sh` builds it whenever no game is named --
`cached_game()` no longer decides -- and the three checks that go through that path (golden,
readback, the resize soak) stopped depending on what somebody built last.

`Engine::sceneOverridden` and `demoWorldApplies` are deleted. `game/demo` imports Sponza and
builds its world unconditionally, as `game/battle_arena` already did.

**The failure the card predicted happened, and it was not the camera.** The first run came back
8 of 13 differing at a mean delta of 73.6 -- and the diff image read as a wrong camera pose,
because the frame was black except where the sun came through the arcade. It was the *lights*:
`placeLights` was a static in `DemoGame.cpp` behind `gltfScene().lights().empty() &&
!demoWorldApplies(e)`, which is to say it ran on golden cases and nowhere else. Four lights a
game shipped for the harness's benefit, in the harness's own binary now. The camera was never
the risk -- `Engine::applyCameraConfig` frames whatever camera `Game::init` installed, and a
`FlyCamera` is a `FlyCamera`.

### Verification

| Check | Result |
|---|---|
| `scripts/golden.sh check release`, cache holding `demo` | **13 of 13, byte-identical** |
| `scripts/golden.sh check release`, cache holding `battle_arena` | **13 of 13, byte-identical** -- the card's actual claim |
| `scripts/readback.sh release` | **9 of 9 bit-identical**, plus the lit silhouette and the 12-swapchain resize soak |
| `scripts/locomotion.sh release` | **9 of 9 arms**, unchanged -- the demo composes unconditionally now |
| `./test.sh debug` | **1070 tests, 108 suites** |
| `scaffold` | `./new_game.sh` builds and runs |
| `./run.sh release` with no game | runs `viewer` |
| `scripts/check_ascii.sh` | clean |

## Reference update

[architecture/tooling.md](../../architecture/tooling.md) on what the golden suite runs and on
what naming no game means, [architecture/limitations.md](../../architecture/limitations.md) --
where the entry this card was opened from is now struck through --
[architecture/systems.md](../../architecture/systems.md), `CLAUDE.md` and
[guides/making-a-game.md](../../guides/making-a-game.md).
