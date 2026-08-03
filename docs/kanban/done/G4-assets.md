---
id: G4
title: Assets
arc: G
size: L
verification: golden-12, tests-4, leak
---

# G4 — Assets

L

## `Assets` — where the real work is

`GltfScene` is already the asset store: it owns the shared vertex and index buffers, the
material buffer, the textures, the bindless descriptor set and the primitive list. What it
is not is *multi-model* or *growable* — `load()` sizes every buffer to one file, once.

- **`loadModel(path)` appends into shared buffers.** This needs growable device-local
  geometry storage: allocate with headroom, reallocate-and-recopy on overflow, log the
  reallocation. **This is the single largest item in the arc**, and everything procedural
  waits on it.
- **`createMesh(MeshData)` then falls out of that path for free**, which is the argument for
  doing the growth work properly rather than special-casing a second buffer for procedural
  geometry.
- **`createMaterial` / `setMaterialParam`** make the material buffer mutable and
  revision-counted, mirroring `InstanceTable::revision()`: a static scene uploads once, a
  mutated one costs a memcpy rather than a diff. `GpuMaterial` gains a `shader` index and a
  `params` vec4.

**`loadModel` is additive, and `GameSetup::scene` survives it.** The sketch in Part 1 has a
game loading its own models in `init`, which reads as the engine no longer loading a scene at
all. It cannot stop: `scripts/golden.sh` names a scene on the command line for every one of
its cases, precisely so that no baseline depends on a configured default, and `run.sh -- <path>`
is the same override by hand. So the engine keeps loading `setup.scene`, a command-line path
keeps winning over it, and `loadModel` adds to what is already there.

The consequence lands on the game rather than on the engine, and G9 is where it is felt: **a
game that authors content in code has to condition it on the scene it was actually handed.**
`DemoGame::init` already does exactly this for lights -- it auto-places a set only when
`gltfScene().lights()` is empty -- so the pattern exists and the row inherits it rather than
inventing it.

The texture half needs nothing new. The bindless array, the stable slots with a free list,
the transfer queue and the reserved fallback slot are all already there — they were built
as the four checkable properties of the residency delegation, and a multi-model `Assets` is
simply their first real consumer.

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- The unit suite in four configurations, each its own invocation:
  `./test.sh debug`, `release`, `asan`, `tsan`.
- A thousand create/destroy cycles under ASan, high-water mark unchanged.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

Three pieces, and the row was right that the first is the whole of it. `reserveGeometry`
doubles the vertex and index buffers and copies the old contents forward, so `appendModel`
no longer refuses what does not fit; `createMesh(MeshData)` then really did fall out of
that path, adding a struct and a forwarding call rather than a second buffer;
`createMaterial`/`setMaterial` with a `materialRevision()` counter give the material table
the same upload rule the instance table already had.

**Growth broke a descriptor set, silently.** A grown buffer is a new `VkBuffer`, and the
skinning set still named the freed one — `VUID-vkCmdDispatch-None-02699`, "using buffer
sceneVertices that is invalid or has been destroyed", found only because the growth path
was exercised with validation on. Fixed by splitting `writeSkinSet(slot, allocate)` out of
the allocate-and-write path so `setScene` can rewrite every frame's copy. Without the
layers this reads as a shader sampling garbage.

**`GpuMaterial` did not gain `shader` and `params`.** The row asked for them; nothing reads
them until G5 defines what a shader index selects. Adding two fields the GPU ignores means
getting the std430 layout right twice, so they wait for their consumer.

Verification. `scripts/golden.sh check release` — twelve cases, byte-identical, which is
the claim that a scene loading one file renders exactly what it did before any of this.
`./test.sh` in four configurations, each its own invocation: debug, release, asan, tsan,
641 tests passing in every one.

The thousand create/destroy cycles could **not** be run under ASan on this machine: the
NVIDIA driver fails `vkCreateDevice` with `VK_ERROR_INITIALIZATION_FAILED` under
AddressSanitizer, the same way it segfaults there under ThreadSanitizer, so no frame is
ever drawn. The cycle ran in debug with validation instead, which is where the claim
actually lives — the allocator's largest free run before and after a thousand
create-then-destroy round trips: 58618 free vertices and 248382 free indices, both
sides, unchanged. Host-side leaks stay covered by the unit suite under ASan.
