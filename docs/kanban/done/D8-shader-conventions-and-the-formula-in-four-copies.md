---
id: D8
title: Shader conventions, and the formula in four copies
arc: D
size: S
verification: golden-12, inspection
---

# D8 — Shader conventions, and the formula in four copies

`worldFromDepth` and `FAR_DEPTH` now live once, in [`frame.glsl`](../../../engine/shaders/frame.glsl), which owns `invViewProj` and is the narrowest scope every consumer already reaches. Four copies under three names deleted, plus the two open-coded ones. The spelling half went with D1's and D2's sweeps: all 23 push-constant blocks are `pc`, `local_size` spells only the dimensions a shader indexes, and a write-only storage image says so. **The third item was the one worth doing by hand** -- three bloom shaders shared a destination declaration and only two of them are write-only; `bloom_up.comp` reads what it overwrites, so a blanket sweep would have written a lie the compiler accepts. It now carries the comment saying why it is the exception. **The golden set is byte-identical across all twelve cases** both times, which is the proof the row promised in advance

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- By inspection alone -- no test covers this path, and that gap is part
  of the finding.

## Reference

[architecture/rendering.md](../../architecture/rendering.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
