---
id: D2
title: A namespace that means something
arc: D
size: S-M
verification: golden-12
---

# D2 — A namespace that means something

Four namespaces, one per directory under `engine/`: `core`, `gfx`, `scene`, `ui`. Twenty-two headers and nineteen sources that were at global scope are now in the one that names their directory, and `input`/`json`/`settings`/`options` became `core::input` and friends rather than four peers of the module they live in. `GpuProfiler` moved to `gfx/` instead — it takes a `VulkanContext` and fills a `VkQueryPool`, so it *is* graphics, and the corollary is that a misfiled header gets moved, not renamed. The row was written as S on a misreading (`GltfScene.h` and `GpuProfiler.h` were said to be *in* `namespace gfx`; both were at global scope with a `gfx` forward-declaration block on top, which is what a first-match grep sees) — the decision really was the small half, but the sweep is ~1,400 call sites across 90 files and is an M. Verified 488 tests in debug, 488 under ASan, and all twelve golden cases byte-identical

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.

## Reference

[architecture/principles.md](../../architecture/principles.md), [architecture/README.md](../../architecture/README.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
