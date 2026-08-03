---
id: C42
title: A node carries components including the game's own
arc: C
size: L-XL
verification: golden, tests-hosted, validation, scaffold, leak
---

# C42 — A node carries components including the game's own

Afterwards a scene node holds *components* rather than six named attachment slots: the
engine's own -- instance, body, character, sound, light, emitter -- become components like
any other, and a game registers its own types and attaches them to the same node. A fighter
stops being a struct in a vector beside the tree and becomes a node with a mesh, a controller
and a `Health` on it.

```cpp
const NodeId fighter = e.scene().create("player");
e.scene().add<Model>(fighter, "res:/Mana/Mana.gltf");
e.scene().add<CharacterController>(fighter, capsule);
e.scene().add<Health>(fighter, 100);          // the game's own, same call
```

**Importing an asset is one of these**, which is why
[C41](C41-a-game-composes-its-scene-and-a-glb-is-an-asset-it-imports.md) is this row's consumer:
a `.glb` on a node is a component, not a special verb, and that is the whole of what "a glb is
an asset rather than a scene" means once nodes carry components.

**The tree already predicted this and declined to build it early.** `InstanceTable`'s header
says an entity component system adopted later "sits *beside* this, holding an `InstanceId` as
a component, rather than trying to own it"
([InstanceTable.h:288-295](../../../engine/scene/InstanceTable.h#L288)). That is the shape this
row builds, and the note is the reason it is buildable without disturbing the dense storage
every pass reads: no subsystem's storage moves, only what a node knows about it.

## What is wrong with `Attachments`

`Attachments` is a fixed struct of six typed fields plus an offset matrix
([Scene.h:117-146](../../../engine/scene/Scene.h#L117)). Two consequences follow from its being
a struct rather than a set:

- **One of each, forever.** A node has exactly one instance, one body, one sound. Two sounds on
  a character -- footsteps and a voice -- is not expressible, and neither is a fighter whose rig
  is two skinned meshes, which is why `game/battle_arena` creates a child node per mesh and
  keeps a `std::vector<NodeId>` of them.
- **Nothing a game defines.** Every field is an engine handle. There is no room on a node for
  health, an action queue, a team, or a state machine cursor, so a game's own data lives
  somewhere else and is joined back to the tree by hand.

`game/battle_arena` is that join written out. `struct Fighter` holds a `PhysicsCharacterId`, a
`NodeId`, an `AnimatorId`, a vector of mesh `NodeId`s and five animation parameter indices, and
lives in `std::vector<Fighter> fighters` addressed by the constants `kPlayer` and `kEnemy`
([ArenaWorld.h](../../../game/battle_arena/ArenaWorld.h)). Every phase of that game's roadmap
adds fields to it -- a path follower in Phase 3, health and an action queue in Phase 4, a combat
state machine in Phase 6. It is a component table with one row type, hand-rolled, because the
tree offers nowhere to put one.

## What this is not

Not an ECS in the storage sense. There is no archetype table, no system scheduler and no
iteration order this row promises -- the engine's dense arrays stay exactly where they are and
keep being walked exactly as they are. What changes is that a *node* can be asked what it
carries, and a game can add to that list. The refusals in
[CLAUDE.md](../../../CLAUDE.md) are about indirection written over Vulkan; this is a gameplay
data model on the scene tree, which is the layer that has none.

## Verification

- `scripts/golden.sh check` -- every case byte-identical. The engine's six attachments becoming
  components must not change a transform, a draw or an order.
- `./test.sh debug`, then `./test.sh asan`. Hosted cases for attach, detach, query, the
  more-than-one case `Attachments` cannot express, and a game type round-tripping.
- `scaffold`: a scaffolded game builds and runs without touching `engine/`.
- `leak`: create and destroy nodes with components at two different cycle counts and reach the
  same steady state. A component store that does not release with its node is the failure mode.
- Zero validation errors with layers on.

## Expected to be wrong about

Whether the engine's own six should become components at all, or whether the honest answer is
a node with its existing typed slots *plus* an open component set for everything else. The
first is uniform and touches every consumer of `attachments()`; the second is smaller and
leaves two ways to ask what a node holds. Worth deciding before either is started, and
`game/battle_arena` is the caller to decide it against.

## Reference update

[architecture/systems.md](../../architecture/systems.md) on the scene tree and its sweep, and
[guides/making-a-game.md](../../guides/making-a-game.md), which teaches the attach verbs.

## Outcome

`Scene` gained `add<T>`, `get<T>`, `has<T>`, `remove<T>` and `each<T>` over a per-type
`unordered_map<slot, T>`, held type-erased in a vector indexed by a per-type id.
`game/battle_arena`'s `Fighter` is the first caller: it was a struct in
`std::vector<Fighter>` with a `NodeId` field and is now a component on the node it describes,
with `ArenaWorld` reduced to two node handles.

**The card's open question resolved against keeping the engine's six where they are**, and the
deciding fact was not the one the card expected. There are only fourteen sites touching
`attachments()` — far fewer than the "touches every consumer" the card assumed — so the
conversion was affordable. What ruled it out is that two of those fourteen are the per-frame
sweep, which reads `attached` for every node in the scene: a hash lookup per node per kind is a
real cost on the hottest walk in `scene/`, bought for API symmetry alone. So the store is for
everything the engine does not already walk densely, which is every type a game defines and
every type [C41](C41-a-game-composes-its-scene-and-a-glb-is-an-asset-it-imports.md) adds.

The type erasure has no base class. `engine/` defines exactly three and CLAUDE.md is explicit
that a fourth is the thing to stop, so `shared_ptr<void>` carries the deleter — which is all a
virtual destructor would have bought — and one function pointer carries the single operation
that must run without knowing `T`.

**Verification.** `./test.sh debug` and `./test.sh asan`: 1069 of 1069, 108 suites — seven new
`SceneComponents` cases covering game-defined types, type and node independence, replacement on
re-add, death with the node *and its subtree*, stale handles reading as nothing, `each` skipping
dead carriers, `clear`, and a two-count cycling arm as the leak check. `scripts/golden.sh check
release`: 13 of 13 byte-identical. `scripts/locomotion.sh release`: 9 of 9 arms. Validation:
zero errors. `battle_arena` reproduces its Phase 2 trace exactly through the component path —
`8.21 m travelled, along 1.00, facing 1.00`, the same figures as before the conversion.

## Deferred

- **More than one of a kind is still not expressible**, which was one of the two complaints the
  card opened with. One value per type per node is the standard shape and a node wanting two
  sounds holds a component with a container in it, or two child nodes — but the card claimed
  this row would fix it and it does not. Recorded here rather than left implied.
- Nothing walks components in a stable order. `each` is a hash map walk and says so; a system
  that needs determinism sorts what it collects. If a future row wants ordered iteration that is
  a dense store with a sparse index, which is a different data structure and a different card.
