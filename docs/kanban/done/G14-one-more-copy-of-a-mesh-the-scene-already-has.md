---
id: G14
title: One more copy of a mesh the scene already has
arc: G
size: S
verification: golden-11, tests-4
---

# G14 — One more copy of a mesh the scene already has

Afterwards the engine carries the verb `DemoWorld::placeCopy` had to write by hand: one more
instance of a primitive the scene already holds, at a transform, without a second model and
without a second copy of the geometry. `game/demo/DemoWorld.cpp` loses its private version.

This is a G row rather than a C row because the capability is already here and a game cannot
reach it. `addPlacementInstances` does exactly this for a *file's* placements and is public,
but it walks a placement range rather than taking a transform. The demo's own comment is the
finding:

> **The engine has no verb for this**, which is the single most concrete thing this row
> found. [...] so a game wanting a hundred urns cannot reach it. See the card.

This is that card. It is **S** because the implementation already exists and is known to
work — `placeCopy` builds seven fields out of a `Primitive` the engine holds — so the work is
choosing the scope it belongs at and moving it, not designing it.

Worth being careful about the one field the demo names as the trap: `dynamic` is not
cosmetic. It decides whether the instance writes a velocity for TAA and which tier of the
acceleration structure it lands in, and getting it wrong is a shadow that stays behind after
the thing casting it has been knocked over. Whatever the promoted signature looks like, that
argument should be hard to leave wrong.

Expected to be wrong about: whether the right scope is a public method on `Engine` or a free
function beside `addPlacementInstances`. Both current callers are games, so the narrowest rung
that reaches them may be the free function.

## Verification

- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan`, each its own
  invocation.
- `scripts/golden.sh` — eleven cases, byte-identical. This moves code without changing what
  it computes, so a moved pixel is a defect and a re-snap is not an available answer.

## Reference update

[architecture/scene.md](../../architecture/scene.md) — the instance table's public surface.

## Outcome

`Engine::addInstance(model, material, transform, motion)`, `Renderer::instancesGrew()`,
`scene::InstanceMotion`, and `DemoWorld.cpp`'s `placeCopy` deleted. Three call sites moved.

**The scope question the card flagged was decided by what the verb has to do afterwards,
not by who calls it.** A free function beside `addPlacementInstances` reaches both callers
and was the narrower rung — and it is the wrong answer, because the seven field copies were
never the hard part. `InstanceTable::create` cannot tell the renderer anything, and two
things then go wrong silently:

- A slot past the renderer's instance capacity is a `memcpy` past the end of a mapped
  staging range. `Renderer::updateInstances` copies `instances->shadingBytes()` with no
  bound, and nothing on the frame path calls `ensureInstanceCapacity` — the three callers
  are `setInstances`, `createFrameResources` and `setAnimator`. The demo's private helper
  survived on the doubling headroom the last `createMesh` happened to leave.
- A *new* slot is invisible to `staticTierStale`, which walks the slots the acceleration
  structure **baked**. It fires on a static instance having moved, not on one having
  appeared — so the instance draws in every raster pass and appears in no ray.

Neither is a thing a game can be expected to know, which is what puts the verb on `Engine`.
`Renderer.h` also claimed the renderer "grows its buffers when the slot count outruns them",
which was not true of any code path; the sentence is corrected.

**The obvious implementation is a five-fold regression, and measuring it is what caught
that.** `addInstance` first called `render.setInstances(&instanceTable)` — the existing verb
for "the table changed" — which also rebuilds the acceleration structure. On the demo, in
debug:

| | `Game::init` | `buildSceneAccelStruct` |
|---|---|---|
| before the card | 63.9 ms (release) | x5 |
| via `setInstances` | **316.6 ms** | **x16** |
| via `instancesGrew` | 142.1 ms (debug) | x5 |

`instancesGrew` is the cheap half: size the buffers, force the frame slots to re-upload, set
a flag. `rebuildAccelIfStale` consumes the flag once a frame, so a loop of any length costs
one rebuild — and it lands in frame 1's `endFrame`, which runs before that frame's
`drawFrame`, so no drawn frame is missing the geometry. The x5 that remain are
`Engine::createMesh`'s, and they are
[their own card](../backlog/chore-the-acceleration-structure-is-rebuilt-once-per-mesh-a-game-adds.md).

*This is a card that would have passed its stated verification while shipping a defect.* The
golden set is byte-identical either way — no case uses `addInstance` — and 934 tests say
nothing about it. What caught it was the startup trace the previous card had just built,
which is the second time this week the instrument arrived before the thing it measured.

**`dynamic` is now `scene::InstanceMotion` with no default.** The card asked for the
argument to be hard to leave wrong; an enum makes the call site say which it means, and no
default makes the caller decide. `InstanceDesc::dynamic` stays a bool, because a field among
thirty in a struct a caller fills deliberately is not the same risk as the fourth positional
argument to a verb.

934 tests in each of debug, release, asan and tsan; `scripts/golden.sh check release`,
eleven of eleven byte-identical. The demo under ASan, 90 frames: no memory error — the only
report is 584 bytes across nine allocations, every one of them inside `libvulkan`, the
NVIDIA driver and four ICDs, none in ours.
