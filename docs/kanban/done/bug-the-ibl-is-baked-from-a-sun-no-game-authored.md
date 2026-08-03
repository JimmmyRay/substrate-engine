---
id: bug-the-ibl-is-baked-from-a-sun-no-game-authored
title: The IBL is baked from a sun no game authored
arc: bug
size: S
verification: golden, validation
---

# bug-the-ibl-is-baked-from-a-sun-no-game-authored — The IBL is baked from a sun no game authored

The environment cube, the irradiance map and the prefiltered chain are all baked from
`Renderer::sunDirection` inside `createIblResources`, which runs from `Renderer::init`
([Renderer.cpp:424](../../../engine/gfx/Renderer.cpp#L424)) — **before any sun the game or the
scene authored has been applied**. So every scene in this tree is image-lit by the renderer's
own default sun rather than by the one lighting it.

## The ordering, which is the whole bug

`Engine::init` runs `initWindow()`, then `initRenderer()`, then `loadScene()`, then
`initLights()` ([Engine.cpp:286-304](../../../engine/Engine.cpp#L286)). `initRenderer` calls
`render.init`, which bakes the IBL from whatever `sunDirection` holds at that moment — the
member's own initialiser, `{-0.35f, 0.85f, 0.4f}`. `initLights` is where a sun actually
arrives, and it runs afterwards.

It has been invisible because `GameSetup`'s old `sunDirection` default was the *same*
`{-0.35f, 0.85f, 0.4f}`, so a game that did not override it baked with the right value by
coincidence. `game/battle_arena` authors `{-0.3f, 0.9f, 0.25f}` and its ambient cube is lit
from somewhere its sun is not; `engine/assets/mirror.gltf` ships its own directional light and
is in the same position.

D20 did not cause this and does not change it — it found it, because moving the sun into
`GameSetup::lights` made the question "when is the sun known?" one somebody had to answer.

## What the fix has to decide

Baking later is the obvious move and it is not free: the IBL bake is a blocking submit of a
512² cube plus a five-mip prefiltered chain, and `createIblResources` is also called from the
shader hot-reload path, which has its own reasons for the current ordering. The candidates are
to bake on the first frame instead of at init, or to re-bake from `initLights` when the sun it
resolved differs from the one baked with — the second is cheaper to reason about and pays the
cost twice on exactly the scenes that need it.

**Expect the golden set to move**, and that is the finding rather than a problem: every case
whose scene ships a directional light is currently lit by the wrong environment. A moved
baseline here is a corrected image, which is the one circumstance a re-snap is the right answer
— stated in advance, on this card, rather than discovered while running the suite.

## Verification

- `scripts/golden.sh check` — expected to move. Each moved case has to be looked at and the
  move argued: a scene whose sun matches the old default must **not** move, and one that ships
  its own directional light must.
- Zero validation errors with layers on, since the fix moves a blocking submit.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) on the IBL bake and when it
happens, and [architecture/tooling.md](../../architecture/tooling.md), which now records that
the validation layers want `--no-ray-query` on a scene that ray-traces.

## Outcome

`Renderer::createIblResources` records the sun it baked with, and `rebakeIblIfSunMoved` --
called from `Engine::initLights` the moment the real sun is resolved -- re-bakes when they
disagree and does nothing when they agree. It replaces the four images and the one descriptor
set that names them, and deliberately **not** `iblSetLayout`: `init` creates that and every
pipeline baked it into its own layout, so a re-bake must not rebuild pipelines the way the
hot-reload path does.

**One golden case moved, and it was the predicted one.** Every other case is lit by a sun of
`{-0.35, 0.85, 0.4}`, which is the value `Renderer::sunDirection` is initialised to, so no
re-bake happens and nothing moves. `mirror.gltf` ships its own directional light, re-bakes, and
logs two `IBL ready` lines.

`mirror-no-rt` did **not** move, and that is the corroboration: without ray tracing the
reflection never samples the environment cube, so the same scene with the same corrected cube
is unchanged. The difference was 23796 of 1440000 pixels at a mean delta of 0.26, entirely the
specular sun-spot on the four metallic spheres -- strongest on the smoothest (roughness 0.05),
absent on the roughest (0.65). That is what a moved reflected sun looks like and not what
corruption looks like.

### The re-snap

`debug_frames/golden/mirror.png` was replaced with the corrected capture, and **only** that
file: all forty-four PNGs under `debug_frames/golden/` were hashed before and after and the
diff is one line. `scripts/golden.sh snap` was not used, because it re-captures all thirteen
and the claim being made is about one.

This is the circumstance the project allows a re-snap in: the old image is wrong, and the
argument was written on this card before the suite was run.

### Verification

| Check | Result |
|---|---|
| `scripts/golden.sh check release` | **13 of 13**, with `mirror.png` corrected and the other twelve byte-identical and untouched |
| `./test.sh debug` | **1070 tests, 108 suites** |
| `validation`, `mirror.gltf` in debug | **0 errors**, and two `IBL ready` lines, so the check covered the new path |

**Validation on this scene needs `--no-ray-query`.** With the ray-query extensions on, thirty
headless frames do not finish inside 90 s; without them the same run takes a couple of seconds.
That is not this change -- the scene completes either way without validation, and the re-bake
is independent of ray query -- it is the layers' cost on the ray-query path, the same shape as
the rule ASan already needs on this driver. Recorded in tooling.md.

### What this cost, which is the part worth reading

The bake happens once more than it used to on exactly the scenes that need it, and the second
one is a blocking submit of a 512^2 cube plus a five-mip chain during startup. The card offered
two candidates -- bake on the first frame, or re-bake when the sun turns out to differ -- and
the second is what landed, because it pays nothing at all on a scene whose sun matches and is a
straight equality test rather than a new ordering for anything else to depend on.
