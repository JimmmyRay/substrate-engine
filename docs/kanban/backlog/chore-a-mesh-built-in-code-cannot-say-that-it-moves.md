---
id: chore-a-mesh-built-in-code-cannot-say-that-it-moves
title: A mesh built in code cannot say that it moves
arc: chore
size: S
verification:
---

# chore-a-mesh-built-in-code-cannot-say-that-it-moves — A mesh built in code cannot say that it moves

`Engine::createMesh` takes geometry and nothing else
([`Engine.h:330`](../../../engine/Engine.h#L330)), so the instance it creates is static and a
game whose mesh moves has to reach past the call and set the flag by hand:

```cpp
// game/battle_arena/ArenaWorld.cpp:609 and :682, twice, with the same comment on both
e.instances().setFlags(instance, scene::kInstanceDynamic, 0);
```

**`addInstance` two lines further down the header takes the argument and refuses to default
it** — "Not defaulted, and not a bool — see `scene::InstanceMotion`"
([`Engine.h:350`](../../../engine/Engine.h#L350)) — which is the right call and is the whole
of the inconsistency: the neighbouring verb makes the caller decide, and this one decides for
them and gets it wrong for anything that moves.

What it costs when nobody notices is not a wrong picture. A static instance that moves lands
in the baked tier of the acceleration structure and the renderer rebuilds the whole structure
every frame the sweep carries it off the transform it was baked at
([`Renderer.cpp:932`](../../../engine/gfx/Renderer.cpp#L932)) — with a warning, once, which is
the only thing that made this findable at all. `battle_arena` has two such meshes, the shield
sphere and the queue marks, and both are written every frame.

Found while establishing whether the roadmap's `addModel` row was still outstanding. It is
not the same row: a rig imported by `addModel` is skinned, so it lands in the deformed tier
and never touches the static one. This is the caller that is left.

## Verification

- `game/battle_arena` builds and runs with both `setFlags` calls deleted and the motion
  passed to `createMesh` instead, and no acceleration-structure warning appears over a run
  in which the player walks.
- `./test.sh debug`, then `./test.sh asan`.
- `scripts/golden.sh` — byte-identical. No golden case builds a mesh in code.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — wherever the instance motion
convention is described.

## Outcome

Filled in on close.
