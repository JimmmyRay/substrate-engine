---
id: chore-startup-is-profiled-for-the-scene-load-and-nothing-else
title: Startup is profiled for the scene load and nothing else
arc: chore
size: M
verification: trace, tests-hosted, tests-4, golden-11
---

# chore-startup-is-profiled-for-the-scene-load-and-nothing-else — Startup is profiled for the scene load and nothing else

Afterwards the startup frame covers the whole of `Engine::init` rather than one call inside
it, `createPipelines`, the acceleration-structure build, the navmesh bake and the Jolt
collider cook each have a name, and `scripts/baseline.py` can report frame 0 — which today
it discards.

The glTF load path is instrumented well: twelve zones across
[`SceneParse.cpp`](../../../engine/scene/SceneParse.cpp) and
[`GltfScene.cpp`](../../../engine/scene/GltfScene.cpp), down to the `extras` scans that
[C13](../done/C13-a-measured-load-time-baseline.md) found were 73% of a load. Everything
*else* startup does is dark:

- `createPipelines` ([Renderer.cpp:2944](../../../engine/gfx/Renderer.cpp#L2944)) — shader
  module reads, SPIR-V reflection, every graphics and compute pipeline. `Pipeline.cpp` has no
  zones.
- `buildSceneAccelStruct` ([AccelStruct.cpp:142](../../../engine/gfx/AccelStruct.cpp#L142)) —
  a BLAS per primitive and a TLAS, and it is built twice during load.
- `NavMesh::bake` ([NavMesh.cpp:48](../../../engine/scene/NavMesh.cpp#L48)) — voxelize,
  region-label, BVH.
- Jolt world init and the collider cook ([Physics.cpp:352](../../../engine/scene/Physics.cpp#L352)).
- The `Uploader` batch submits ([Resources.cpp:355](../../../engine/gfx/Resources.cpp#L355)).

Load time is a thing this engine has already decided to care about — C13 exists, and C10
cannot be honest unless a level's load cost is bounded and known. Half of that cost is
currently measured to three decimal places and the other half is not measured at all.

## Two traps, and they are why this is M rather than S

**The startup frame is opened in the wrong place.** `Profiler::beginFrame` is called from
`Engine::loadScene` ([Engine.cpp:496](../../../engine/Engine.cpp#L496)), but `initWindow` and
`initRenderer` run *before* that call and `initAudio`, `initPhysics`, `initNavigation` and
`initCloth` run after it. A scope opened before the first `beginFrame` records with an empty
thread stack, so it lands at **depth 0 as a sibling of `Frame`** rather than inside it, and
its path has no `Frame/` prefix for anything to key on. So adding scopes to `initRenderer`
without moving the frame boundary produces zones that are technically in the trace and
attributable to nothing. The frame opens at the top of `Engine::init`.

**`scripts/baseline.py` drops frame 0 outright** — [baseline.py:121](../../../scripts/baseline.py#L121),
`if frame == 0: continue`. It is a deliberate exclusion, because frame 0 is startup and
pooling it with steady-state frames would poison every median. But it means the tool that
reads the trace cannot see startup **at all**, so the whole of this card's instrumentation
would be invisible to it.

That is the same discovery
[chore-one-profiler-zone-covers-every-pass-the-cpu-records](../done/chore-one-profiler-zone-covers-every-pass-the-cpu-records.md)
made about `--zones` and CPU scopes, and it has the same consequence: **the tooling half goes
first**, or the verification below cannot be performed. A `--startup` mode that reports frame
0 alone — one column, no medians, because there is one sample by construction — is the
smallest thing that works. Frame 0 is already pinned against eviction in both of the
profiler's trim loops, so the data survives a long run; nothing reads it.

## Verification

- `scripts/baseline.py --config release --startup` — the new zones appear with numbers, and
  their sum accounts for the wall-clock startup time the engine already logs. A gap is work
  no zone names, and naming it is the point.
- The same at `--config debug`. Debug is where pipeline compilation and the collider cook are
  worth attributing, and the two configurations should disagree by a lot on exactly those.
- Frame 0 zones sit **under** `Frame` in the trace, at depth 1 or deeper, not beside it. That
  is the check on the moved frame boundary and it is visible in Perfetto directly.
- A steady-state `--zones` run is **unchanged** — moving the frame boundary must not shift
  which frame is which, and the per-pass numbers are the control.
- `./test.sh` in all four configurations, and `scripts/golden.sh check release`.

## Reference update

[architecture/tooling.md](../../architecture/tooling.md) — `scripts/baseline.py` gains a mode,
and the description of what the trace's frame 0 holds is currently "startup and asset
loading", which becomes true rather than aspirational.

[guides/profiling.md](../../guides/profiling.md) — the note that the window is FIFO so a long
run evicts frame 0, and that load timings are "therefore also logged directly", is the
paragraph this card changes.

## Outcome

`scripts/baseline.py --startup`, the frame boundary moved to the top of `Engine::init`, and
twenty-three zones. Startup is 683 ms in release on Sponza with **0.031 ms unnamed** at depth
1 — 0.005%.

| | release | debug |
|---|---|---|
| `Frame` (all of startup) | 683.1 | 830.7 |
| `VulkanContext::init` | 274.1 | 278.1 |
| `Swapchain::create` | 145.8 | 141.4 |
| `Engine::initWindow` | 93.2 | 100.9 |
| `GltfScene::textures` | 78.0 | 82.9 |
| `GltfScene::geometry` | 22.1 | **121.5** |
| `createIblResources` | 19.4 | 22.0 |
| `createPipelines` | 3.3 | **20.4** |

**The two configurations disagree by a lot on exactly the things they should**, which is the
check the card asked for: 5.5x on `GltfScene::geometry` and 6x on `createPipelines`, both our
code, against 1.4% on `VulkanContext::init` and 3% on `Swapchain::create`, both the driver's.
**Two thirds of startup is the driver in both.** That is a useful thing to know before
anybody optimises a loader.

**The tooling half went first and it had to.** `baseline.py` dropped frame 0 outright, so
every zone this card adds would have been invisible to the tool meant to report them. It now
returns frame 0 separately and `--startup` prints it.

**The frame boundary was wrong in a second place the card did not name, and it was the
bigger one.** Moving `beginFrame` to the top of `init` fixed `initWindow` and `initRenderer`
as predicted — and left **fifty-four depth-0 zones**, because `Engine::run` calls
`Game::init` *after* `init` returns and before the loop's first `beginFrame`. Everything a
game builds at startup was landing at depth 0 as a sibling of `Frame`. The startup scope is
therefore a member of `Engine` rather than a local, opened in `init` and closed in `run`
after `Game::init` and `applyBindings`. Depth-0 zones on the main thread: **54 before, 1
after** — the `Frame` zone itself.

*The remaining depth-0 zones are `GltfScene::decode` on the texture-decode workers, 31 of
them, and they are not a boundary bug.* A worker thread has its own scope stack and `Frame`
was never on it, so a zone recorded there has no frame path by construction. Its parent
`GltfScene::decodeAll` is on the main thread and does carry one. Recorded rather than fixed:
the alternative is giving every worker a synthetic parent, which would make the path a
fiction.

**`--startup` needed a count column, and finding that out cost one wrong table.** The card
argued for "one column, no medians, because there is one sample by construction" — and there
is one *startup*, not one of each zone in it. Last-value-wins reported `Game::init` at
0.095 ms when it is 63.9, because `buildSceneAccelStruct` runs **five times** inside it and
only the last was shown. Summed and counted now.

**That is the finding, and it has its own card.** Eighty-two percent of the demo's
`Game::init` is five acceleration-structure rebuilds — 52 ms — one per mesh the game creates,
plus three `createPipelines` at 8.4 ms for the same reason. The count scales with how much a
game builds in code, which is the case this engine is for. Opened as
[chore-the-acceleration-structure-is-rebuilt-once-per-mesh-a-game-adds](chore-the-acceleration-structure-is-rebuilt-once-per-mesh-a-game-adds.md)
rather than left in this paragraph.

**The zones agree with the numbers the engine already logs**, measured in one run rather than
across two: the loader's own `timing:` line said `parse=19.7ms textures=96.0ms` and the trace
said 19.7 and 96.0 for the same zones. `Engine::loadScene` is larger than the loader's
`total` — 190.3 against 98.9 — because it also does instances, the animator,
`ensureInstanceCapacity`, `buildSceneAccelStruct` and `createPipelines`, all of which now
have names.

**The steady-state control held.** `--zones` at release before and after: `Lighting` 1.054
both, `GBuffer` 0.270 both, `Bloom` 0.087 both, `Tonemap` 0.038 both, `Frame` 2.098 against
2.101. Moving the frame boundary did not shift which frame is which.

**One thing to know before reading a startup table**: `baseline.py` invokes `./run.sh <config>`
with no game name, so it opens Sponza and a game's own `init` builds nothing —
`Game::init` reads 0.096 ms there and 63.9 ms from a `./run.sh demo` trace. Same property the
game-zones card found, now biting a second mode.

934 tests in each of debug, release, asan and tsan. `scripts/golden.sh check release`, eleven
of eleven byte-identical.
