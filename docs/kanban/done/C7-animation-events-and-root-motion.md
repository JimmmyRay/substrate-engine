---
id: C7
title: Animation events and root motion
arc: C
size: M
verification: golden-12, tests-hosted
---

# C7 — Animation events and root motion

`AnimationEvent` on `AnimationClip`, crossings found by `crossedEvents` and reported as `SceneAnimator::firedEvents()` -- a list read after the update, not a callback, because a callback is the engine calling into a game mid-update. `setRootNode` turns root motion on, and does both halves in one call: it reports the per-step delta *and* holds the node, because doing only the first moves the character twice. Fourteen test cases. Footsteps are C7 plus G7, which is why they are the milestone

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
