---
id: C10
title: Streaming and unloading
arc: C
size: L
verification: golden-12, tests-4, validation, leak
---

# C10 — Streaming and unloading

All three. `core::RangeAllocator` is *geometry range reclamation* (18 hosted tests). `scene::SceneLoader` is *async load off the frame thread* -- `GltfScene::load` splits into `loadCpu` and `upload`, the CPU half runs on a worker, the result is applied at a frame boundary; 9 hosted tests, hosted precisely so `./test.sh tsan` can see it. `GltfScene::appendModel`/`unloadModel` are *N models*: a second glTF sub-allocates out of the buffers the renderer already binds, and gives them back. ~~**Needs G4**~~ **It did not** -- see below; the premise was mine and it was wrong. `Engine::addModel`/`removeModel`/`beginLoadScene`; the demo binds `Z`, `H` and `X`

## What landed of C10, and what the rest actually waits on

The row names three things. **One of them was gated on G4 and is no longer.**

`core::RangeAllocator` is the reclamation half: first fit over a free list, coalescing on
free, growth that merges onto a free tail. It is integers and no Vulkan, so the part that
is easy to get wrong is testable without a device -- which matters more here than usual,
because **a free list that never coalesces still works.** It works for a few hundred
load/unload cycles and then stops, and by then the symptom is "streaming breaks in long
sessions" rather than anything pointing at the allocator. So the tests assert `holeCount`
rather than success, one of them runs 500 load/unload cycles asserting the buffer stays in
*one* piece, and the last one runs 20,000 random operations asserting the only property
that always has to hold: **two live ranges never overlap.**

Two decisions inside it are worth naming:

- **Free list, not compaction.** Compaction keeps the buffer dense by sliding survivors
  down, and it is wrong here because the offsets are live on the GPU -- baked into every
  `VkDrawIndexedIndirectCommand`, every BLAS, every primitive record. Moving one range
  means rewriting all of them and re-cooking the acceleration structures: a stall
  proportional to the whole scene, at the exact moment a game is streaming to avoid one.
  The price is fragmentation, which is why `largestFree` is public beside `used` -- a
  caller that cannot fit a model has to tell "full" from "shredded", because the answers
  are *grow* and *compact offline*.
- **`kNoRange` is `0xFFFFFFFF`, not zero.** Zero is a perfectly good offset. An allocator
  whose failure value is a valid result is one whose callers quietly stop checking.

**Async load landed, and the split it needed was already half done.** C15 had separated
parsing from uploading for its own reasons; C10 only had to name the seam.
`GltfScene::loadCpu` reads the sidecar or parses the document and touches no device;
`GltfScene::upload` is everything that needs a queue. `SceneLoader` runs the first on a
worker and `Engine::applyPendingScene` runs the second at the top of a frame -- never
inside one, because swapping a scene out from under a recorded command buffer is not
something a barrier can fix.

**`SceneLoader` takes its work as a `std::function`, and that is a testing decision rather
than an architectural one.** `GltfScene.h` reaches Vulkan, so a loader naming it directly
could not be linked into the unit suite -- and the unit suite is the only place
`./test.sh tsan` runs. A threaded class with no ThreadSanitizer coverage is the kind that
works for months and then corrupts a scene on someone else's machine. So the caller
supplies the work: `Engine` passes a lambda calling `loadCpu`, the tests pass one that can
be made to fail, block, or publish a megabyte for the release/acquire pair to carry. There
is no interface and no second implementation -- only a seam where the device half would
otherwise force this file out of the suite.

**What is honest about it:** the *parse* comes off the frame thread; the upload does not.
`applyPendingScene` calls `vkDeviceWaitIdle` and rebuilds. Streaming without any hitch at
all needs a transfer queue and a scene that can be built beside the live one, and the
second of those is G4 again.

## "Needs G4" was wrong, and the correction is the useful part

This document claimed for a long time -- and I repeated it -- that `unloadModel` was blocked
on G4 supplying N models. **It was not, and the mistake is worth keeping written down
because of its shape: a dependency was recorded once and then reasoned from instead of
checked.**

What the renderer actually requires is narrow. It reads seven things off the scene:
`vertexBuffer`, `indexBuffer`, `descriptorSet`, `descriptorSetLayout`, `vertexCount`,
`indexData`, `emissiveMaterials`. **Not one of them requires the geometry to have come from
a single file.** They require *one set of buffers and one descriptor set*, which is the
one-bind, draw-everything-indirect design and is worth keeping.

So "load another model" was never N `GltfScene`s -- that reading is what made it look like
an architectural row. It is sub-allocation out of the buffers that already exist, which is
exactly what `RangeAllocator` was built for. `appendModel` rebases every index value onto
its allocated vertex range, offsets each primitive's `firstIndex`, `baseVertex` and
`materialIndex`, and remaps the appended materials' texture indices onto the descriptor
slots their images landed in -- reusing the streaming headroom `buildDescriptors` was
already leaving. `unloadModel` returns the ranges and the slots.

Measured on the demo: appending `physics.gltf` into Sponza takes 349 vertices and 1554
indices; unloading returns exactly 349 and 1554; re-appending reuses the same range. The
second model renders inside the first.

**Two limits, stated rather than discovered.** Deforming models are refused -- `skinOffset`
and `morphOffset` index scene-wide arrays this does not yet extend, and appending a skinned
mesh whose offsets point into the base scene's influences would deform it by someone else's
skeleton. And the buffers do not grow yet: they are made with a quarter over plus 1024
elements of headroom, and an append that does not fit is refused with what it needed.
`RangeAllocator::grow` is written and tested; wiring it means reallocating, copying, and
rewriting the descriptor set, which is the next increment rather than this one.

**The bug this found, which is the reason to distrust "it built and it looked right".**
Appending grew the instance table, and nothing told the renderer -- `addModel` called
`setScene` but not `setInstances`, which is what resizes the indirect command buffer. The
symptom was a draw overrunning `instanceData` by sixteen bytes, reported against a buffer
size rather than against the missing call. `applyPendingScene` had the identical omission,
written the same day for the same reason.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- The unit suite in four configurations, each its own invocation:
  `./test.sh debug`, `release`, `asan`, `tsan`.
- Zero validation errors with layers on, in every capture.
- A thousand create/destroy cycles under ASan, high-water mark unchanged.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

Recorded above, under *What landed of C10, and what the rest actually waits on*.
