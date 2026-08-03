---
id: chore-the-demo-loads-a-scene-a-script-baked
title: The demo loads a scene a script baked
arc: chore
size: M
verification: golden-11, validation
---

# chore-the-demo-loads-a-scene-a-script-baked — The demo loads a scene a script baked

Afterwards `game/demo/assets/showcase.gltf` is gone, `DemoGame::configure` names Sponza, and
`DemoWorld` imports the character, the mirror and the orb through C21's verb. The demo
composes its world in code, which is what every other prop in it already does.

**Corrected while opening: the script does not go with it, only its `build_showcase` half.**
`scripts/make_composite_scene.py` writes *three* files — `character.gltf`, `reflect.gltf` and
`showcase.gltf` (`main`, lines 635-654) — and this row retires the last of them. The other two
are live demo scenes: `reflect.gltf` is the mirror-and-roughness-row scene the README lists and
`limitations.md:216` quotes a measurement from, and `character.gltf` is the skinned stage that
`tooling.md:1168` benchmarks at 256 and 1024 characters. Worse, `DemoGame.cpp:957` — the
`Scene.AddRig` verb C22 added, which is the very mechanism this card places the character
with — imports `res:/character.gltf`. Deleting the generator would delete the file the new
demo loads at runtime.

So the script survives, minus `build_showcase`, `SPONZA_FLOOR_Y` and whatever the composite
alone used. That is a smaller change than the card assumed and it moves the row's weight
entirely into `DemoWorld`.

The composite is a build-time workaround for a runtime gap, and the script says so: Sponza
and a Mixamo rig are third-party files that cannot be committed, so it grafts them into a
sibling document that references the originals' buffers where they lie. Once C21 and C22
land there is nothing left for it to do, and leaving it costs the demo a generated artefact
nobody edits and a scene shape nobody can see from the source.

**The 240 cylinders go with it.** They are `unitCylinder(material, 20)` with a flat brown
material — the code calls them urns and nothing makes them urn-shaped — and they exist only
to give frustum culling something to reject and the draw builder's run-merging something to
merge, because Khronos Sponza is one flattened mesh with no repeats. Real imported content
placed at real transforms is a better version of that test, and a synthetic pile of 240
primitives is not what the demo should be showing.

If a genuine instancing case is still wanted afterwards, Tethered's Sponza is one: 28 nodes
over 10 meshes, arcade sections reused eight, four, four and three times, 208k unique
triangles. It is a different asset with different provenance, so it is a decision to take on
its own and not a side effect of this row.

The blast radius is smaller than it looks, and that is worth stating because it is the reason
this is a chore rather than an arc row: **no golden case loads the composite.** The suite
pins `engine/assets/Sponza/glTF/Sponza.gltf` directly and the skinned case deliberately uses
`skin.gltf` rather than `character.gltf`. Nothing outside `game/demo/` has to move.

Blocked on C21, and on C22 for the character.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. The demo's own image changes and no
  golden case renders it, so any movement here is a defect in the engine paths the demo
  shares rather than the intended change.
- Zero validation errors with layers on, in a capture of the composed demo.
- `scripts/manifest.py` — the asset manifest no longer names a generated scene, and
  `scripts/fetch_assets.sh` no longer invokes a generator that is gone.

## Reference update

[architecture/tooling.md](../../architecture/tooling.md) — "The demo's scene is Sponza, and
the rest of it is code", replacing "The showcase scene". The card named
`architecture/assets.md`; there is no such file and the demo's scenes have always been
documented in `tooling.md`.

Three more had to move with it, because all three asserted things that stopped being true:
`limitations.md`'s "`Engine::addModel` brings geometry and nothing else" (it brings colliders
since C21 and a rig since C22, which is what this row stands on), the same file's light-budget
paragraph, and the README's table of generated scenes.

## Grounding, before any code

Three things the card assumed that the tree does not agree with. The first two are cheap; the
third is the row.

**1. The 240 cylinders are already gone.** `DemoWorld.cpp:683` says so and explains why —
`chore-the-demo-burns-static-instead-of-fire` retired them when the fire moved into the
vessels Sponza already hangs. Nothing to do.

**2. The generator survives.** See the corrected opening: three outputs, one of them retired
here. `build_showcase` and `SPONZA_FLOOR_Y` go; `build_character` and `build_reflect` stay,
and `fetch_assets.sh` keeps invoking the script.

**3. Nothing makes the imported character the player, and that is the actual work.**

`Engine::addModel` already creates bodies from an imported file's `substrate_collider` extras,
attaches them to nodes, and — `Engine.cpp:717` — takes the first character collider it sees as
`playerCharacterIndex`. So a runtime import *would* give a fully wired player, if the file
carried the collider. `character.gltf` does not: `substrate_collider` appears exactly twice in
the generator (lines 543 and 611) and both are in `build_showcase`. The composite is where the
character's capsule, `moveSpeed` 3.2 and `jumpSpeed` 4.2 have always lived.

Without one, `e.playerCharacter()` stays invalid and the demo loses WASD, the follow camera,
`setCharacterInput`, the navmesh follower and the gait-from-speed path all at once — every one
of them guarded on `playerCharacter().valid()`. `PhysicsWorld::createCharacter` is public, so
the game can *make* a controller; what it cannot do is tell the engine that controller is the
player. There is no setter, and `playerCharacterIndex` is written in two places, both inside
the engine's own collider walk.

Three ways out, and they are not equivalent:

- **Author the collider onto `character.gltf`** in `build_character`, so the import carries it.
  Cheapest, and it makes the stage scene idle properly rather than march on the spot — but the
  stage has no ground collider either, so the character falls out of it, and `tooling.md:1168`
  quotes that scene's cost at 256 and 1024 characters with no physics in it.
- **An engine door**, `setPlayerCharacter(character, node)`. Two lines, and squarely the shape
  G8 exists for — a capability a game cannot reach. But it is an engine change on a chore, and
  it wants its own card rather than being smuggled in here.
- **A demo-owned glTF** carrying nothing but the capsule and its extras, imported beside the
  rig. Keeps the engine untouched at the cost of the artefact this row is trying to delete.

**Deliberately not chosen here.** The first two are decisions about where a capability lives,
which is not a call a chore should make on its way past. The row is otherwise ready: the
mirror, orb, fills, ground box and both sounds all have public constructors reachable from
`DemoWorld` — `renderer().lights.push_back(makePointLight(...))` with `Scene::attachLight` is
already how the braziers do it (`DemoWorld.cpp:504`), `audio().create(AudioSourceDesc)` takes
the bus, distances and `spatial` flag the extras carry, and `physics().createBody(ColliderDesc)`
takes the ground box's half extents and friction directly.

## Outcome

The composite is gone. `game/demo/assets/showcase.gltf` and its baked `.scene` sidecar are
deleted, `build_showcase`, `SPONZA_FLOOR_Y` and the now-unused `merge_gltf` are out of
`scripts/make_composite_scene.py` (which still writes `character.gltf` and `reflect.gltf`),
`fetch_assets.sh` says what it generates and what it no longer does, and
`DemoGame::configure` names `res:/Sponza/glTF/Sponza.gltf`.

Everything the composite held is now built in `buildDemoWorld`: a mirror and an emissive orb
off one new `unitSphere`, the orb's point light, a static ground box, a non-spatial cricket
bed and a spatial hum on the orb, and the character -- `Mana/Mana.gltf` imported through
C21's verb, given a controller the game makes, handed over with G16's setter and paired to
its rig through `Engine::locomotion()`.

Four things this turned up that the card could not have known:

- **The physics world was never built.** Sponza declares no colliders, and `initPhysics`
  returned early on exactly that test. See G16 -- it is that card's second half, and without
  it every body in this file is refused in silence.
- **The state machine was built before the rig arrived.** `locomotionMachine` resolves six
  states by name against whatever clips the animator holds, and `DemoGame::init` ran it forty
  lines before `buildDemoWorld` imported the character. The composite hid this completely: its
  rig was in the file, so the clips existed before `init` began. Moved after the world.
- **`characterAt(0)` was never "the player".** It was right only because the composite's rig
  was the scene's one skinned thing; Sponza's own morphed props now take the low indices.
  `DemoWorld::playerRig` holds the appended character and the two readers use it.
- **The shadow atlas overflowed.** A file that declares any light replaces the demo's
  auto-placed set entirely, so the composite's two fills cost 12 layers where the heuristic's
  three cost 18. With the heuristic back, the orb's six did not fit. The orb now *overwrites*
  `lights[0]` -- the centre fill, which is where it stands -- and never erases it, because the
  braziers already hold indices into that vector.

**Scaling is the trap this row leaves behind.** `configure` doubles the building precisely so
the 1.8 m character does not have to change, so a position is scaled and a size is not. Scaling
the rig too produced a 3.6 m character walking at 6.4 m/s and jumping 3.7 m, which
`scripts/locomotion.sh` read back as eleven failures in one run -- the suite earning its keep,
since no golden image would have shown any of it.

## Verification results

- `scripts/golden.sh check release` — **all 11 cases match**, byte-identical. Which is the
  claim that matters here: the demo's own image changes completely and no golden case renders
  it, so any movement would have been a defect in the engine paths the demo shares.
- `--validation on`, 200 frames with the player driven by script: **zero**
  `VUID-`/validation errors, no criticals, no atlas overflow,
  `Demo world: player wired, 3 animator characters, 1 locomotion pairs`, and the machine
  visibly stepping `idle -> walk -> run` off the controller.
- `scripts/manifest.py demo` — no `showcase` anywhere in the package, `Mana/Mana.gltf` picked
  up from the new import, and both `atmosphere_*.wav` present. They were **not**, until this
  found it: the composite's extras carried bare relative paths, resolved by the loader against
  the document, and the same literal handed to `AudioEngine::create` from code resolves against
  the working directory instead. `res:/audio/...` is both the fix and what the packager scans
  for, so the bare form shipped a package with two sounds missing and nothing to say so.
  `--strict` still refuses the package, unchanged and correctly: Sponza is Crytek's and the
  composite referenced its images too.
- `scripts/locomotion.sh`: **four arms, zero assertions failed** — `still`, `modifier`,
  `walk-run-jump`, `jump-buffered`. The suite then failed to finish, twice, on a different arm
  each time: `vkCreateDevice` hung twenty-three seconds and returned `VK_ERROR_DEVICE_LOST`.
  Card: `bug-the-device-is-lost-part-way-through-a-suite`. **Three camera-relative arms are
  unrun.**

  This suite is not on this card's `verification:` line and should be. It caught every one of
  the four defects above and the scaling trap besides, none of which any golden image would
  have shown.
- `./test.sh debug` — **970 tests from 100 test suites** pass. Not on the card either, and run
  because this row changed `Engine::initPhysics`.
- `tests/make_composite_scene_test.py` — 3 cases, 24 subtests, pass. `SHOWCASE_SUN` is now
  `DEMO_SUN`: the generator no longer writes that light and `DemoGame::configure` states the
  same direction, but it is the vector `aim_quat`'s regression was found on and it stays.

## Deferred

- **`tests/manifest_test.py` is red and was red before this row** — `manifest.build` returns
  four values and the tests unpack three, in files neither of which this row touched. Nothing
  runs the python suites at all: `./test.sh` is the gtest binary, CMake has no python target,
  and `fetch_assets.sh` invokes `make_composite_scene.py` without ever running its test. Card:
  `bug-the-python-tests-are-red-and-nothing-runs-them`.
- **`scripts/locomotion.sh` belongs on this card's `verification:` line**, and on any row that
  changes how the demo is composed. Left off rather than edited in after the fact, because a
  verification contract rewritten to match what was run is not a contract. The next row to
  touch this should name `scripted-input`.

## Found after closing

**The character walked without turning, and the suite this card left off its verification line
is what caught it.** All three camera arms of `scripts/locomotion.sh` reported `faced 0.00 of
the way it walked` — every other number in every other arm correct.

`DemoGame` collects the parts it rotates into `facingNodes`: every child of `playerNode()`
**named `mesh`**, which is what `Engine::addModel` calls those nodes when the engine wires a
file's collider itself. This row created them as `player mesh`, so the list came back empty,
nothing was ever rotated, and the character slid around the atrium facing north.

One word, and the whole class of defect is a game reproducing a naming convention the engine
holds by hand. Fixed by using the name; the alternative — a `kMeshNodeName` constant both
sides share — is a real fix and a different row, because the engine spells this in one place
and the demo in another and neither knows about the third that will.

This is the deferred item on this card biting within the hour: `scripted-input` was not on the
`verification:` line, and the three arms that name it are the only check that would ever have
shown this.