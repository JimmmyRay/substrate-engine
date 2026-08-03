---
id: G1
title: Engine, Game, entry point
arc: G
size: L
verification: golden-12, scaffold
---

# G1 — Engine, Game, entry point

L

## G1 as landed

**Byte-identical, and checked against HEAD rather than against the baseline.** All eleven
golden cases match. Five of them differ byte-wise from the *committed* baseline, which
turned out to be stale -- snapped before the commit that closed the previous roadmap -- so
the check that actually answers "did G1 move a pixel" was made by building the
pre-G1 commit and comparing its captures to G1's directly. All eleven are byte-identical
that way. The unit suite passes in all four configurations, TSan included, which is what
proves `SUBSTRATE_HOSTED_SOURCES` survived the CMake change; ASan reports no leak in
Substrate (three allocations inside `libdbus-1.so`, which predate this work); validation
reports zero errors.

Four things departed from the design above, and each is recorded rather than absorbed.

**The loop has five phases, not four.** `beginFrame()` also runs the camera, the audio
listener and the accumulator, which the sketch left implicit, and `drawUi` sits *before*
the step loop rather than after it. The ordering is not a preference: the inspector reads
instance transforms, so drawing it after the movers would show post-step values and change
pixels. The one ordering that *did* change is `camera().update()`, which now runs before
the game's `frameUpdate` -- deliberately, so a game resolving "forward" against
`camera().yaw` gets this frame's yaw rather than last frame's. Nothing observes the
difference: action state was resolved and cached before either, and the UI does not read
the camera.

**`spawnExtraCharacters` stayed in the engine.** The row assigned it to the game. It is
selected by `scene.characters` in the config, which puts it under the *"the engine acts on
its own config"* carve-out, and it has to run before `setInstances` because the renderer
sizes its buffers from the character count it is handed. `locomotionMachine` and
`driveCharacters` did move.

**`placeLights` moved, and the branch above it did not.** A file that ships its own lights
still wins, and that is engine policy because it is a property of the *file*. What a scene
*without* lights gets is a game decision, so both fallbacks -- the auto-placed set and the
config's point-light list -- are in `DemoGame::init`, which reads `e.gltfScene().lights()`
to decide.

**`Ui.Click` became the engine's.** The row left it in `AppActions`. It is the UI's own
click and the UI is an engine subsystem, so it is declared beside the camera's and the
binding menu's actions by the rule those two already follow: the thing that consumes an
action is what names it. It is also the one action exempted from pointer mode, and that
exemption belongs with the declaration.

**One defect the split introduced, found and fixed.** `./test.sh` delegates to
`./build.sh`, which clears the configured game -- so running the unit suite silently
un-configured the build directory and the next `./run.sh` failed. `test.sh` now reads the
cache and carries the value through: it builds one target and changes nothing else about
the directory.

## Notes the G1 row carries

**The split of `main.cpp`.** To the engine: the `--config` pre-scan and
`Config::loadFromFile` / `applyCommandLine`; Logger and Profiler init; `volkInitialize`,
GLFW init, window creation, the UI-scale read; `VulkanContext::init`, `renderDocAttach`,
`Uploader`; `Renderer::init` and the 34 config-to-renderer assignments (temporarily — G2
deletes them); the `--capture-target list` early exit; scene load, `addSceneInstances`,
`SceneAnimator`, `ParticleSystem`, `AudioEngine`, `PhysicsWorld`; all seven GLFW callbacks
and `AppState`; the frame loop; the shutdown ordering. To `game/demo/`: `placeLights`,
`spawnExtraCharacters`, `locomotionMachine`, `driveCharacters`, `PlayerActions`,
`AppActions` and `applyActions`, `drawSettingsPanel`, `PanelState`, `debugViewName`,
`nextCapturePath`.

**One piece goes to neither, and the row says why.** The `DrivenInstance` / `DrivenSource`
wiring and the collider-to-instance binding walk exist *because* there is no scene tree.
They go to the engine and are marked for deletion by G3, because putting them in the game
would move them twice.

**The binary moves, deliberately, and four scripts must move with it in the same stage.**
`build/<cfg>/substrate` stops existing: `build.sh` produces a library, and
`build_game.sh demo` produces `build/<cfg>/demo`. The changes are small and were counted
rather than estimated — `run.sh` names the old path on **one line** (`BIN=`), and
`scripts/baseline.py` on one more; both resolve the configured game's name instead.
`scripts/golden.sh` goes through `./run.sh` and needs no edit, but its prerequisite becomes
`./build_game.sh demo`. **Doing this in a later stage is not an option**: between G1 and
that stage there would be no runnable binary, and the golden set is what proves G1 moved
no pixel.

**`SUBSTRATE_HOSTED_SOURCES` must survive the CMake change.** The test target links only
those translation units — the ones pulling in neither Vulkan nor a window — and that is
what lets `./test.sh tsan` run at all where `./run.sh tsan` cannot. Nothing under `game/`
ever joins that list.

**One naming decision taken now to avoid a rename later.** `Engine::scene()` returns
`Scene&` from G3 onward, so G1's accessor for the loaded glTF is `gltfScene()` — G3 then
introduces a name rather than changing one.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- A scaffolded game builds and runs without touching anything under `engine/`.

## Reference

[architecture/README.md](../../architecture/README.md), [architecture/principles.md](../../architecture/principles.md).

## Outcome

Recorded above, under *G1 as landed*.
