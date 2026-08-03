---
id: C3
title: Runtime effects
arc: C
size: M
verification: golden-12, validation, leak
---

# C3 — Runtime effects

`ParticleSystem::spawnEffect(effect, position, normal)` aims the emitter's local +Y along the normal and forces a burst, so a fire-and-forget call cannot leave a jet running. `ParticleEmitter::burst` makes an emitter a one-shot, and `update()` releases its slot once the last particle dies -- 200 impacts recycle one slot rather than appending 200. `setEmitters` stays as the bulk load-time path that sizes the pool. **The decal half is `gfx::decalAt`**, a free function rather than a handle table: a decal list is a `std::vector` the game already owns and can erase from, so what was missing was the arithmetic. It moved to [`gfx/Decal.h`](../../../engine/gfx/Decal.h) -- no Vulkan, therefore hosted, therefore tested -- the same split `DebugView.h` made for the same reason

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- Zero validation errors with layers on, in every capture.
- A thousand create/destroy cycles under ASan, high-water mark unchanged.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
