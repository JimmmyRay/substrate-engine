---
id: C2
title: Spatial queries
arc: C
size: M
verification: golden-12, tests-hosted, validation
---

# C2 — Spatial queries

`raycast`, `sphereCast` and `overlapSphere` on `PhysicsWorld`, returning a `RayHit`; `segmentBlocked` reimplemented over `raycast`. Two deviations from the row as written, both argued in the header: the third query is a **sphere** cast rather than a general `shapeCast`, because a general one needs a shape and `ColliderDesc` carries mesh data a query should not build per call; and `RayHit::operator bool` tests `distance` rather than the body handle, so a hit on geometry this class did not index — a character's internal body — reads as the hit it is

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- Zero validation errors with layers on, in every capture.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
