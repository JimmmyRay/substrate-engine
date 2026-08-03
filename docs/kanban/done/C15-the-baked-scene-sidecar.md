---
id: C15
title: The baked scene sidecar
arc: C
size: L
verification: golden-12, tests-hosted, trace
---

# C15 — The baked scene sidecar

[`scene/SceneData.h`](../../../engine/scene/SceneData.h) names the CPU half of a load and [`SceneData.cpp`](../../../engine/scene/SceneData.cpp) writes it beside the source as `<name>.scene`. **Measured on C13's 8000-node scaling scene: 45 ms to 9 ms**, and on Sponza 123 ms to 111 ms -- the difference between the two is the point, and it is the one C13 predicted: Sponza's load is 90 ms of texture upload the sidecar does not touch. Invalidated three ways, each checked before a byte of payload is read: a format version, a layout digest folded from `sizeof` of every POD written, and the source's size and mtime. **The proof is the golden suite**: all twelve cases render byte-identically from the sidecar and from the document, which is the strongest check this row could have. Eleven hosted tests cover the round trip and every refusal

## What landed, and the three things the estimate did not predict

The split is [`parseSceneData`](../../../engine/scene/GltfScene.cpp#L201), and it is exactly the
two ranges above with `materials` moved up to make them contiguous. `GltfScene::load` is now
"fill a `SceneData` from a cache or a document, then decode, upload and describe" -- and the
half below the fill cannot tell which way it was filled, which is the property that makes
the golden suite a proof rather than a smoke test.

1. **A header split came with it.** `SceneData.cpp` is in `SUBSTRATE_HOSTED_SOURCES` and so
   may not include a header that reaches Vulkan -- but `Vertex`, `Primitive`, `Placement`
   and `SceneStats` lived in `GltfScene.h`, which owns `VkBuffer`s. They are now in
   [`scene/SceneTypes.h`](../../../engine/scene/SceneTypes.h), which is the same move
   `ui/FontMetrics.h` and `gfx/Decal.h` already made. The split was right on its own merits;
   C15 is only what made it a link error rather than a preference.
2. **The obvious way to read the file lost to the parser.** `istreambuf_iterator` into a
   vector took **34 ms** on Sponza's 12 MB sidecar against 28 ms to parse the glTF outright
   -- a cache that was slower than the thing it replaced. Sized-then-`read` takes 17 ms. The
   lesson is worth more than the fix: a cache is the one kind of code where a plausible
   implementation can be *worse than absent*, and the only way to know is to measure it
   against the path it replaces.
3. **The saving is where C13 said it was, not where it looks biggest.** Sponza goes 123 ms
   to 111 ms and the 8000-node scene goes **45 ms to 9 ms**, because Sponza's load is 90 ms
   of texture upload that no scene format touches. Quoting the Sponza number alone would
   have made this row look marginal; quoting the scaling number alone would have oversold
   it. Both are in the row.

The refusals are the other half, and they are what the eleven tests are mostly about: a
truncated file, a foreign file, a bumped version, a changed layout, an edited source and a
deleted source all produce *no cache* rather than a bad load. A cache that is wrong looks
exactly like a cache that is fast.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- Per-pass GPU cost from `scripts/baseline.py` trace medians, several runs
  per arm -- never the `GPU @` log line.

## Reference

[architecture/tooling.md](../../architecture/tooling.md), [architecture/systems.md](../../architecture/systems.md).

## Outcome

Recorded above, under *What landed, and the three things the estimate did not predict*.
