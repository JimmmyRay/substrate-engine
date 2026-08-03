---
id: chore-what-the-frame-pays-outside-record
title: What the frame pays outside record
arc: chore
size: S
verification: trace, tests-4, golden-11, validation
---

# chore-what-the-frame-pays-outside-record — What the frame pays outside record

Afterwards every step `Renderer::drawFrame` takes before and after `Renderer::record` has a
name, so the gap between the `Frame` zone and the sum of its children is small enough to be
evidence rather than noise.

[chore-one-profiler-zone-covers-every-pass-the-cpu-records](../done/chore-one-profiler-zone-covers-every-pass-the-cpu-records.md)
instrumented what `record` calls, and it accounted for 96-98% of `record`. It did not touch
what `drawFrame` does *around* it, and that is where the frame's spikes live. `waitFence`,
`acquire`, `submit` and `present` have zones; everything between them does not:

| Work | Where | Why it is worth a name |
|---|---|---|
| `syncImages` | [Renderer.cpp:540](../../../engine/gfx/Renderer.cpp#L540) | `vkDeviceWaitIdle`, then `stbi_load` decode and upload, in the middle of a frame |
| `ensureSpriteCapacity` | [Renderer.cpp:616](../../../engine/gfx/Renderer.cpp#L616) | device wait plus buffer realloc |
| `ensureInstanceCapacity` | [Renderer.cpp:3648](../../../engine/gfx/Renderer.cpp#L3648) | device wait plus buffer realloc |
| `pollShaderReload` | [Renderer.cpp:2577](../../../engine/gfx/Renderer.cpp#L2577) | a filesystem stat every frame, and on a hit `glslc` and a full `createPipelines` |
| the pipeline and render-target rebuild | [Renderer.cpp:6779](../../../engine/gfx/Renderer.cpp#L6779) | `vkDeviceWaitIdle` and rebuild everything |
| `updateUniforms` and `updateLights` | [Renderer.cpp:3435](../../../engine/gfx/Renderer.cpp#L3435) | a CPU light cull and **two sorts**, per frame |

These differ from a pass in a way that decides how they read. A pass costs roughly the same
every frame; **each of these costs nothing on almost every frame and milliseconds on one**,
so the number to look at is the max rather than the median — which `scripts/baseline.py
--zones` already prints and which is currently reporting the max of a zone that does not
exist.

`updateLights` is the exception and the reason this is worth doing before a large scene
rather than after one. It is not a spike; it is a per-frame cull and two `std::sort` calls
over the light list, so its cost is a function of light count. On Sponza that is invisible.
On a level with a thousand lights it is a frame budget, and nothing in the trace would say so.

`applyPendingScene` ([Engine.cpp:813](../../../engine/Engine.cpp#L813)) belongs to the same
list and sits in `Engine` rather than `Renderer` — a full device idle and a scene upload on
the frame thread, at whatever moment an async load happens to complete.

**Scopes above the early-outs**, as with every other zone in the tree: `pollShaderReload`
returning without recompiling and `syncImages` finding nothing dirty are the *common* cases
here, and a zone that vanishes on the cheap path is one whose max cannot be trusted.

## Verification

- `scripts/baseline.py --config debug --zones`, and a run with `--frames` large enough that
  a capacity growth and an image sync both occur — the new rows appear, and the **max**
  column on `syncImages` and the capacity zones is far above the median, which is the shape
  this card exists to make visible.
- The gap between `Frame` and the sum of its children narrows. Quote it before and after;
  what remains is work still unnamed, which is a question the next card can ask.
- A run with `--no-rt` and a shader touched mid-run, so `pollShaderReload` records a hit as
  well as its per-frame miss.
- `scripts/golden.sh check release` — eleven cases, byte-identical.
- `./test.sh` in all four configurations, and zero validation errors with layers on.

## Reference update

[guides/profiling.md](../../guides/profiling.md) — "What is instrumented" says the frame's
spine and every pass; it gains the steps between them, and the note that these are read by
max rather than by median.

## Outcome

Eight scopes: the six on the table plus `updateLights` under `updateUniforms`, and
`applyPendingScene` in `Engine`. The gap between `Frame` and the sum of its children on the
demo in debug, 239 frames:

| | ms/frame | share of the frame |
|---|---|---|
| before | 0.1620 | 3.7% |
| after | 0.1050 | 2.4% |

The new depth-1 rows sum to 0.0559 and the gap closed by 0.0570, which is the accounting
identity holding. **What is left is 0.105 ms a frame that nothing names**, and that is the
question for the next card.

**`updateUniforms` was the whole of the difference at the median** — 0.0542 ms a frame, of
which `updateLights` is 0.0356. It was the largest single unattributed item in the frame and
it is not a spike; it is the CPU light cull and its two sorts, which is the thing the card
said was worth naming before a large scene rather than after one.

**The maxes are the finding, and they are larger than the card guessed.** Driven with
`--input-script` (C16) and a shader touched mid-run:

| Zone | median | max |
|---|---|---|
| `pollShaderReload` | 0.0003 | **3266.13** |
| `pipelineRebuild` | 0.0002 | **23.51** |
| `syncImages` | 0.0001 | **0.76** |
| `ensureSpriteCapacity` | 0.0001 | **0.31** |
| `ensureInstanceCapacity` | 0.0004 | 0.0006 |

`pollShaderReload`'s 3.27 *seconds* is one `glslc` sweep and a full `createPipelines`, and the
frame it lands on is 3271 ms. `pipelineRebuild`'s 23.5 ms is what an added model costs when it
changes the feature key — a whole frame budget, on a keypress, and nothing in the trace named
it before.

**The card put `ensureInstanceCapacity` on `drawFrame`'s list and it is not on it.** The three
callers are `Engine::setInstances`, `createFrameResources` and `setAnimator`, reached from
`addModel`, `createMesh` and `removeModel` — which a *game* calls, so the zone appears as
`Game::frameUpdate/ensureInstanceCapacity`. It is still a mid-frame device wait and still
worth naming; it is attributed to the game that caused it rather than to the renderer, which
is more useful than the card expected. The scope went inside the function, so it lands
correctly wherever it is called from.

**Two call sites had to change so a zone could not vanish**, which is the same call-site
lesson the sibling card learned. `ensureSpriteCapacity` was guarded by `sprites != nullptr`
and is now called with `0` instead, which its first test returns on. The pipeline rebuild was
a bare `if` and is now a braced block with the scope above the dirty test.

**One golden run failed and it was not a pixel.** `particles` came back FAIL with
`vkWaitForFences(upload) failed: VK_ERROR_DEVICE_LOST` in its log — after the capture, so the
image it wrote is byte-identical to its baseline at max channel delta 0, and what failed was
the process exit code. The device loss followed a run this card's own verification had
`SIGTERM`ed part-way through a `glslc` recompile. The re-run is clean at 11 of 11. Recorded
rather than passed over, because a golden failure whose diff is empty is a *harness* failure
and reads exactly like a real one.

`scripts/golden.sh check release`, eleven of eleven byte-identical. 929 tests in each of
debug, release, asan and tsan. Zero validation errors with layers on.
