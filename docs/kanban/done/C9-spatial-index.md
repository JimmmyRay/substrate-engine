---
id: C9
title: Spatial index
arc: C
size: M-L
verification: golden-12, tests-hosted, trace
---

# C9 — Spatial index

**Over instances rather than nodes.** [`scene/SpatialIndex.h`](../../../engine/scene/SpatialIndex.h): a BVH with `raycast`, `overlap` and `visible`, maintained by `Engine` -- rebuilt on a structural change, refitted when anything moved, and free when nothing did. **The gate was on the wrong half.** `InstanceTable` already keeps a world box per slot and already refreshes it in `setTransform`, so the index needs no scene tree; what G3 was really gating is *incremental update on reparenting*, and a flat table has no reparenting to be incremental about -- a moved instance wants a refit, which is what it gets. **Measured on `instances.gltf`'s 4096 slots: 240 us to build, 0.06 us a frame thereafter**, and 0.25 us median refitting a settling physics stack. Fourteen hosted tests, and the ones that carry it compare every query against a brute-force scan on random scenes -- an accelerator has exactly one correctness property and it is *returns what O(n) would*

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- Per-pass GPU cost from `scripts/baseline.py` trace medians, several runs
  per arm -- never the `GPU @` log line.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
