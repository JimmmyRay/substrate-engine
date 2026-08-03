---
id: C1
title: Uniform lifetimes and handles
arc: C
size: L
verification: golden-12, tests-4, leak
---

# C1 — Uniform lifetimes and handles

[`core/Handle.h`](../../../engine/core/Handle.h), and `InstanceId` is an alias of it. `PhysicsWorld` has `BodyId` and `PhysicsCharacterId` with free lists, generations, `createBody`/`createCharacter`/`destroy` and deferred reclaim at the top of `step()`. `AudioEngine` has `SoundId` with the same shape, reclaiming inside `update()`. `SceneAnimator` has `AnimatorId` with joint blocks pinned to slots, and `ParticleSystem` has `EmitterId`. Every subsystem now has `create`/`destroy`, a free list, a generation and a `valid()`; the golden set is byte-identical across all twelve cases

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- The unit suite in four configurations, each its own invocation:
  `./test.sh debug`, `release`, `asan`, `tsan`.
- A thousand create/destroy cycles under ASan, high-water mark unchanged.

## Reference

[architecture/principles.md](../../architecture/principles.md), [architecture/README.md](../../architecture/README.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
