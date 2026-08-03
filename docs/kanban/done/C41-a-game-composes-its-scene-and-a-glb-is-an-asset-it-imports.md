---
id: C41
title: A game composes its scene and a glb is an asset it imports
arc: C
size: L
verification: golden, tests-hosted, validation, scaffold
---

# C41 — A game composes its scene and a glb is an asset it imports

Afterwards a game builds its world by making nodes and giving them components, and
`GameSetup::scene` is gone. A `.glb` stops being *the scene* and becomes what it is: one
asset, imported onto a node, alongside however many others.

```cpp
const NodeId arena = e.scene().create("arena");
e.scene().add<StaticGeometry>(arena, "res:/arena.glb");
```

**Importing is a component, not a verb on `Scene`.** That is what makes this row a consumer of
[C42](C42-a-node-carries-components-including-the-games-own.md) rather than a sibling of it:
once a node can carry components, "this node is that asset's geometry" is one of them, and a
free `addStaticGeometry` would be a second way to say the same thing. C42 lands first.

**The engine already has this verb and calls it the second-class one.** There are two loaders
for the same file type. `GltfScene::load` builds the scene, is called once from
`Engine::loadScene` ([Engine.h:587](../../../engine/Engine.h#L587)), and its output is
`sceneData` -- the thing every other subsystem is sized from. `GltfScene::appendModel` imports
the identical format into the live world, carries its geometry, lights, emitters, sounds,
colliders and rig ([Engine.cpp:663-712](../../../engine/Engine.cpp#L663)), and is reached
through `Engine::addModel`. The second does strictly more than the first. What separates them
is only that one runs before the subsystems are sized and the other after.

**And `scene::Scene` -- the actual scene -- cannot import anything.** It is a node tree with
attachments ([Scene.h:171-338](../../../engine/scene/Scene.h#L171)): `create`, `setParent`,
`attachInstance`, `attachBody`, `attachLight`. Every one of those is a verb about a node that
already exists. Nothing on it says "put this asset in the world", so the type named `Scene`
is the one type that cannot be given a scene.

The consequence a game feels is that its world is a command-line argument. `--scene` replaces
what `GameSetup::scene` named, so a game cannot know whether the world it is standing in is
its own -- which is why `game/battle_arena` opens with `arenaWorldApplies()`, a gate its
roadmap calls mandatory from the first line of game code, comparing `config().scene.path`
against `gameSetup().scene`. That gate is not a pattern worth teaching. It is a game asking
"did somebody swap my world out" because the engine's model allows the question.

## What this changes

- `GameSetup::scene` and `sceneScale` go. A game imports what it wants, onto the node it
  wants, in `init()`; the node's transform is the placement, so `addModel`'s transform
  argument goes with it.
- The import verbs become component types -- static geometry, a model, a rig -- so composition
  is `create` plus `add<T>` and there is one way to put something in the world.
- One loader. `load` becomes `appendModel` into an empty world, which is what it is.
- `--scene` becomes what it always meant in practice: a debugging override that imports one
  asset into an empty world and runs the game's `init` with nothing else in it. `scripts/golden.sh`'s
  eleven cases are exactly that, so they keep working and stop needing every game to defend
  against them.

## Colliders, which is where importing gets its teeth

An asset does not only bring geometry: `arena.glb` authors its collision as `.collider` nodes,
and importing it has to put bodies in the world and triangles in the navmesh. Three things are
wrong with how that happens today, and all three are load-order assumptions this row breaks.

- **Binding is written twice.** `initPhysics` walks the loaded scene's colliders, and
  `Engine::addModel` walks an import's with a second copy of the same arithmetic -- its own
  comment says so, and calls it the Rule of Threes taken at its word
  ([Engine.cpp:702-795](../../../engine/Engine.cpp#L702)). With one loader there is one walk,
  and the coincidence stops being one.
- **The navmesh does not know about imports.** `initNavigation` bakes from static mesh
  colliders and runs at load and from `applyPendingScene`
  ([Engine.cpp:1133](../../../engine/Engine.cpp#L1133)) -- never from `addModel`. So geometry
  imported at runtime is solid to the solver and absent from every path query, silently. A game
  composing its arena in `init()` walks straight into that, and `game/battle_arena`'s Phase 3
  is the caller that would find it.
- **Placement is the node's, and static bodies do not move.** A collider component has to be
  built at its node's world transform rather than at an import transform, and a static body
  whose node is moved afterwards is the same hazard `movedSinceBake` warns about on the render
  side. Either the node is frozen once it carries static collision, or the component refuses to
  be static -- and saying which is part of this row.

The `.collider` name convention itself survives unchanged and gets clearer: a node whose name
ends in `.collider` gets a collider component instead of a geometry one, which is what the
convention already means and could not previously say.

## The hard part, and why this is L

Subsystem sizing runs between "the scene is loaded" and "the game is initialised", and reads
the loaded document: physics capacity from the collider count, particles from the emitters,
the acceleration structure and the navmesh from the geometry. A game composing its world in
`init` inverts that order, so every one of those has to become incremental or deferred --
which is the same shortfall [C40](C40-the-engine-sizes-its-own-pools-and-a-game-states-no-budgets.md)
is about, reached from the other side. C40 should land first; this card is its consumer, and
the two together are what let a game state no budgets *and* build its world in code.

`Engine::applyPendingScene` also rebuilds the instance table, scene buffers, spatial index and
navigation while touching neither physics, audio, particles nor the animator -- a divergence
this row either fixes or inherits.

## Verification

- `scripts/golden.sh check` -- every case byte-identical. Eleven of the thirteen are
  `--scene` overrides, so they are the direct test of the new override path.
- `./test.sh debug`, then `./test.sh asan`.
- `scaffold`: a scaffolded game builds and runs without touching `engine/`. A game that can no
  longer say `setup.scene = ...` and cannot yet say anything else is the failure this catches.
- Zero validation errors with layers on.
- Both games in the tree run, and `./build.sh` alone still builds with `game/` absent.

## Expected to be wrong about

Whether `--scene` should run the game's `init` at all, or should be a pure asset viewer with no
game code in it. The golden cases would be happy either way; a game debugging its own content
would not.

## Reference update

[architecture/systems.md](../../architecture/systems.md) on scene loading and the two-stage
placement walk, [architecture/limitations.md](../../architecture/limitations.md) on
`applyPendingScene`, and [guides/making-a-game.md](../../guides/making-a-game.md), which teaches
`setup.scene` as the first thing a game writes.

## Outcome

`GameSetup::scene` and `sceneScale` are gone and both games in the tree compose their worlds in
`init`. `game/battle_arena` makes an `arena` node and imports `arena.glb` onto it; `game/demo`
states `setWorldScale(2)`, makes a `sponza` node and imports Sponza onto it, then builds the
rest as it already did. `Engine::addModel(NodeId, path)` is the verb: it places the import at
the node's world transform, hangs every instance it produced under that node, records a
`scene::ImportedModel` component, and rebakes the navmesh when the import brought colliders.

**Three things had to be undone before either game could run, and all three were the load-order
assumption the card predicted.**

- `initPhysics` early-returned when the loaded document declared no collider and no cloth, so a
  game composing its world got `createCharacter` refused by an uninitialised world -- silently,
  with `'player' was refused a controller` as the only symptom. The world is now created
  whenever physics is enabled. C40 made capacity elastic, so there was nothing left to save by
  guessing.
- The bindless texture array was sized once, at 64 free slots past whatever loaded. An empty
  world gets 65, and Sponza alone needs 64 -- `out of texture slots at image 64`, then a
  building with no textures. `GltfScene::reserveTextureSlots` now doubles the array, appending
  slots past the end so every index a material already names keeps its meaning, and re-creates
  the pool, layout and set. `Engine::addModel` already calls `Renderer::setScene` after every
  import, which is what rebuilds the pipelines holding the old layout.
- `GltfScene::appendModel` never extended `boundsMin`/`boundsMax`, because before this row the
  bounds came from `load` and an append was a prop inside them. A composed world's bounds are
  *only* its imports, so a `boundsSet` flag now decides whether to seed or to union, over all
  eight transformed corners.

**A scale is not a placement, and the API says so now.** The first attempt threaded a `scale`
argument through `addModel` and `appendModel`; that was wrong twice over -- it needed passing at
five call sites in `game/demo` alone, and it invited writing the scale into the node's transform
instead. `scaleSceneData` holds rigs and dynamic colliders at their authored size and carries
light ranges, intensities and audio falloff with the factor; `placeSceneData` does none of that,
so a node scaled by two imports a stretched character. `Engine::setWorldScale` is the one place
that number goes, and `GltfScene::sceneScale` -- which `--scene-scale` still writes -- is what
it sets.

### The defect this shipped with, found by looking at the screen

**`addModel(node, path)` attached every imported instance with an identity offset, and that
erased the document's own node hierarchy.** The reasoning on the line was that the import's
transform is already baked into each instance, so a second copy would place it twice -- and it
is wrong, because `Scene`'s sweep writes `node.worldTransform * offset` back *over* the
instance every frame. With identity, that overwrite is the node's transform alone.

`arena.glb` is one floor at the origin and a `columns` node carrying a -36.9 z translation. The
floor survived and the columns did not: they landed 37 metres out, half of them off the arena
entirely. **Every count read correct** -- same 2 placements, same 2 draws, same 3508 triangles
as the `load` path, same navmesh, same collider binding -- because nothing was missing. It was
in the wrong place, and no number this row checks is a position.

Fixed by making the attachment offset what it is everywhere else in the engine:
`inverse(placement) * instanceTable.transform(instance)`, which is the same arithmetic
`bindDrivenNodes` uses for a driven mesh.

The lesson is the cheap one and it cost the most: **a scene-composition row needs somebody to
look at the frame.** The golden suite covers `--scene`, which does not go through this call at
all, and `scripts/locomotion.sh` passed nine of nine because the character walks on colliders
that were placed correctly. Both gates were green over a visibly broken arena.

### What the card asked for and did not get

- **`add<StaticGeometry>(node, "res:/arena.glb")` is not the shape.** `Scene::add<T>` is a
  component store: it holds a value, and it cannot perform a load without `Scene` knowing about
  `Engine`, an upload queue and a device wait. `addModel(node, path)` is the verb and
  `ImportedModel` is the component recording what it produced, which is the same information
  reached the honest way.
- **There is still one loader too many.** `load` was not folded into `appendModel`, and the
  collider binding walk is still written twice -- once in `initPhysics` for a `--scene`
  document and once in `addModel` for every import. C41 made the second the only one a game
  exercises, which turns a symmetric duplication into an asymmetric one. That is
  [its own card](../backlog/chore-one-collider-walk-instead-of-two.md).
- **`--scene` still runs the game's `init`.** The card listed "whether `--scene` should run the
  game's `init` at all, or should be a pure asset viewer" under what it expected to be wrong
  about, and the answer came back during the row: it should be a viewer. `game/battle_arena`
  now composes unconditionally and has no `arenaWorldApplies`. `game/demo` still gates on
  `Engine::sceneOverridden()`, because `scripts/golden.sh` runs `./run.sh <config>` with no game
  name and gets whichever game the build directory holds -- so the demo's world would land in
  thirteen baselines. The fix is a viewer the harness runs instead of a game, and that is
  [its own card](../backlog/chore-the-golden-suite-runs-a-viewer-not-whichever-game-the-build-holds.md).

### Verification

| Check | Result |
|---|---|
| `scripts/golden.sh check release` | **13 of 13, byte-identical** |
| `./test.sh debug` | **1070 tests, 108 suites** |
| `./test.sh asan` | **1070 tests, 108 suites** |
| `scripts/locomotion.sh release` | **9 of 9 arms**, and every figure unchanged -- 8.21 m travelled, 0.93 m peak rise, 1.00 along the camera, 0.00 m of pose drift |
| `validation`, `./run.sh demo debug` | 0 errors over 30 frames, across two texture-array growths |
| `validation`, `./run.sh battle_arena debug` | 0 errors over 30 frames |
| `scaffold` | `./new_game.sh` builds and runs; the template no longer writes `setup.scene` |
| `./build.sh release` with no game | links `libsubstrate.a` |

`game/battle_arena` reports `2 colliders bound`, `Nav: 850 triangles, 1812 vertices, 30 regions`
and both fighters holding controllers, and walks 5.68 m over 240 steps of held input at
`along 1.00, facing 1.00`. `game/demo` grows its texture array `65 -> 130` on the Sponza import.
