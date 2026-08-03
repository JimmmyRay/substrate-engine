---
id: C14
title: One document scan, and a package that bakes
arc: C
size: M
verification: golden-12, tests-hosted, trace
---

# C14 — One document scan, and a package that bakes

[`core::json::GltfDocument`](../../../engine/core/Json.h) parses once and hands its `nodes` array to all three extras readers. **Measured on the same 8000-node scene C13 used: `extras` fell from ~57 ms to ~19 ms and the whole load from ~80 ms to ~44 ms** -- a 45% cut, from an M row. The live defect it predicted is fixed with it: an emitter on an unnamed node reported an empty string where a collider on the same node reported `node 7`. The package half is done too: `build_release.sh` resolves the manifest once to learn which scenes are staged, bakes each with `scripts/ktx2.py`, then takes the real manifest with `--require-cache`, which fails naming every image that came back cold. **Which scenes to bake is decided by the same resolver that decides what to package**, so the bake list cannot drift from the staged list -- the failure mode a hand-kept list would have

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- Per-pass GPU cost from `scripts/baseline.py` trace medians, several runs
  per arm -- never the `GPU @` log line.

## Reference

[architecture/tooling.md](../../architecture/tooling.md), [architecture/systems.md](../../architecture/systems.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
