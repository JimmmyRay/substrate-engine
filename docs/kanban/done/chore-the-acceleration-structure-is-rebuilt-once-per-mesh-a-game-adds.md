---
id: chore-the-acceleration-structure-is-rebuilt-once-per-mesh-a-game-adds
title: The acceleration structure is rebuilt once per mesh a game adds
arc: chore
size: S-M
verification: trace, golden-11, tests-4
---

# chore-the-acceleration-structure-is-rebuilt-once-per-mesh-a-game-adds — The acceleration structure is rebuilt once per mesh a game adds

Afterwards a game that creates twenty meshes during `Game::init` pays for **one** BLAS/TLAS
build rather than twenty, and the trace says so.

Found by
[chore-startup-is-profiled-for-the-scene-load-and-nothing-else](../done/chore-startup-is-profiled-for-the-scene-load-and-nothing-else.md),
which is the first card that could see it: `--startup` on `./run.sh demo` reports

```
Frame/Game::init                                     63.9 ms  x1
Frame/Game::init/buildSceneAccelStruct               52.1 ms  x5
Frame/Game::init/buildSceneAccelStruct/Uploader::submitAndWait  19.0 ms  x20
Frame/Game::init/createPipelines                      8.4 ms  x3
Frame/Game::init/ensureInstanceCapacity               1.1 ms  x5
```

**Eighty-two percent of `Game::init` is five acceleration-structure builds**, and the demo
creates a handful of props. The count is a function of how many meshes a game makes, so a
game that builds a level in code pays it once per mesh — which is the case this engine is
supposed to be for. `createPipelines` three times has the same shape: a material with a new
shader variant changes the feature key, and the rebuild is unconditional.

The mechanism is already understood and is not a defect in itself: `Engine::createMesh` and
`addModel` call `render.setScene`/`setInstances`, and `rebuildAccelIfStale` then rebuilds
because the instance table moved. What is missing is a way to say *"I am about to make
twenty of these"* — a batch, a deferral to the end of the frame, or a dirty flag consumed
once per frame rather than once per call. The frame already has a natural point for it:
`endFrame` calls `rebuildAccelIfStale` exactly once, and the startup path does not go
through `endFrame` at all.

Worth being careful about the one thing that makes deferral wrong: an accel structure that
is stale when a ray is traced draws a reflection of geometry that is not there. Whatever the
batch looks like, the invariant is that nothing traces between a mesh being added and the
rebuild.

Expected to be wrong about: whether the fix belongs in `Engine` (defer to the next
`endFrame`) or in the game-facing API (an explicit begin/end batch). The first needs no new
verb and is what `endFrame` already does; the second is honest about the cost but is API
surface a game has to remember.

## Verification

- `scripts/baseline.py --config release --startup` on a run that loads the demo's own scene
  — `Game::init/buildSceneAccelStruct` reports **x1**, and `Game::init` falls by roughly the
  42 ms the other four cost.
- `scripts/golden.sh check release` — eleven cases, byte-identical. `lit` traces rays against
  this structure, so a rebuild that is skipped when it should not be moves that image.
- A run with `--input-script` spawning several meshes mid-run, with `--rt` on: the reflection
  of a newly spawned cube appears on the frame it is spawned, not the frame after. That is
  the property a deferral is most likely to break and no golden case covers it.
- `./test.sh` in all four configurations.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — when the acceleration
structure is rebuilt, and what a game pays for adding geometry.

## Outcome

The deferral, not a batch verb: `setInstances` marks `accelDirty` instead of building, and
`rebuildAccelIfStale` consumes it once a frame in `endFrame` — which already runs before that
frame's `drawFrame`, so no new API and no invariant to remember. The demo, debug:

| | before | after |
|---|---|---|
| `Game::init` | 142.1 ms | **19.5 ms** |
| `Game::init/buildSceneAccelStruct` | x5 | **x1** |
| `Game::init/createPipelines` | x3, 66.5 ms | **x0** |
| whole of startup | 1151.9 ms | **973.6 ms** |

**`createPipelines` was half of it and the card did not mention it.** `setInstances` sets
`pipelinesDirty` and `setScene` calls `createPipelines()` outright — and `Engine::createMesh`
calls `setScene` per mesh, so a game making three props rebuilt every graphics and compute
pipeline three times, 22 ms each in debug, for an answer no frame had asked for. Only the
first `setScene` builds eagerly now, because until a scene exists there is no descriptor
layout to build a pipeline layout from; every later one marks the flag that `drawFrame`
already honours.

**The card's own "expected to be wrong about" was answered the third way.** It offered
`Engine`-side deferral or an explicit begin/end batch. The answer is neither: the *renderer*
defers, so `Engine` needed no change at all and a game needs no verb. The one thing this
rests on is an ordering that already existed and is now load-bearing — `Engine::endFrame`
calls `rebuildAccelIfStale` at line 2030 and `render.drawFrame` at 2147, in that order.

**The mid-run check found a defect that had nothing to do with batching and everything to do
with this area.** Spawning three cubes with `--input-script` gave one rebuild per spawn *on
the spawn frame*, which is the property the card asked for — and then **a full rebuild on
every frame from 61 to 200**. Pre-existing, and the engine's own warning says exactly what it
is: *"every frame is an instance that should have been created dynamic."* `createMesh` builds
every instance static, the demo attached a dynamic body to one and never flagged it, so it
was baked into the static tier and fell out of it. One line in `DemoGame::spawnCube`. After:
builds on frames 0, 1, 61, 62, 63 and nowhere else, and zero warnings.

That is worth stating plainly, because it is the same defect the golden set cannot see and
934 tests cannot see: **a rebuild of the whole acceleration structure, every frame, for the
rest of the run, on a keypress**, and the only thing that ever reported it was one warning
line and a trace nobody had reason to read until this week.

`scripts/golden.sh check release`, eleven of eleven byte-identical — the deferral changes
*when* the structure is built and not what it holds, and `lit` traces both shadows and
reflections against it. 934 tests in each of debug, release, asan and tsan. Zero validation
errors with layers on, in a run that spawns a mesh mid-frame. Startup in release: 665 ms,
0.030 ms unnamed.
