---
id: D5
title: Reuse what the engine already has
arc: D
size: S
verification: golden-12
---

# D5 — Reuse what the engine already has

[`Logger::vformat`](../../../engine/core/Logger.h#L99) is a public static that sizes a `va_list` format correctly in two passes. Three sites re-implement it with a fixed buffer that silently truncates: [`Inspector.cpp:15`](../../../engine/ui/Inspector.cpp#L15) at 160 bytes, [`Profiler.cpp:611`](../../../engine/core/Profiler.cpp#L611) at 256, and [`Physics.cpp:88`](../../../engine/scene/Physics.cpp#L88) at 1024 — which truncates and then hands the result to `Logger::debug`, the very thing that would have sized it. The Rule of Threes here is not unmet — the extraction **already exists and is simply not called**, which is the cheapest kind of finding there is, and the clearest evidence in the arc that an uncleaned tree teaches the next author the wrong thing. All three now call it; the citations above are where each fixed site says so

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.

## Reference

[architecture/principles.md](../../architecture/principles.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
