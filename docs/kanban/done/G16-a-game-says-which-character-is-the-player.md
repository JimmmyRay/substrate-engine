---
id: G16
title: A game says which character is the player
arc: G
size: S
verification: golden-11, validation
---

# G16 — A game says which character is the player

Afterwards `Engine::setPlayerCharacter(character, node)` exists, and a game that spawned a
character controller itself can hand it to the engine as the player. Today only the engine
can decide that, and it decides it by walking a loaded file's colliders.

`playerCharacterIndex` is written in exactly two places — `Engine.cpp:717` inside
`addModel`'s collider walk, and `Engine.cpp:1379` inside `initPhysics` — both taking the
first `ColliderMotion::Character` they meet. `playerCharacter()` and `playerNode()` read it.
There is no setter, so a controller made through the perfectly public
`PhysicsWorld::createCharacter` can never become the player, and the eight or so paths that
guard on `playerCharacter().valid()` — WASD through `setCharacterInput`, the follow camera,
the navmesh follower, gait-from-speed, the jump — are unreachable for it.

This is the G arc's own shape: the capability exists, the engine uses it, and a game cannot
say the word. It surfaced from `chore-the-demo-loads-a-scene-a-script-baked`, which replaces
a baked composite with a runtime import and then has nowhere to put the character it just
made.

The alternative considered and rejected is authoring a capsule onto `character.gltf` so the
import carries its own collider. It works, and it drags two unrelated scenes with it: the
stage that file draws has no ground collider, so a character controller in it falls out of
the world, and `tooling.md:1168` quotes that scene's cost at 256 and 1024 characters with no
physics running. A four-line setter costs neither.

Expected to be wrong about: whether one setter is enough, or whether `playerNode` wants to be
derived rather than passed. The engine currently creates that node itself while walking
colliders; a game that made its own controller has a node already and should not be made to
create a second.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. No golden case has a player character
  in it, so any movement is a defect elsewhere.
- Zero validation errors with layers on. The real exercise arrives with the chore that needs
  it: a character imported at runtime that then answers WASD and is followed by the camera.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the locomotion section, which
already says `Engine::locomotion()` is the door for a rig a game spawned itself. This is the
other half of that door.

## Outcome

Two engine doors, not one. The card scoped `setPlayerCharacter` and the row turned up a
second gap behind it that the first is useless without.

**`Engine::setPlayerCharacter(character, node)`** is as scoped: it writes
`playerCharacterIndex`, and writes `playerCharacterNode` only when handed a valid node, so a
game re-pointing the controller need not know what the engine attached. Clearing the
character clears both -- a node naming a destroyed controller is what every `playerNode()`
reader would otherwise hold.

**`Engine::initPhysics` returned early on a scene with no colliders**, and that is the one
that actually cost the afternoon. The test read "nothing here needs physics" and meant "no
file said so", so a game composing its world in code had `createBody` and `createCharacter`
refused by an uninitialised world -- silently, for every prop it made, with the demo's ground,
ramp, six crates, four barrels, platform and player all quietly non-physical. It now also
tests `GameSetup::bodyBudget`, which is the game stating it has bodies of its own; zero still
means "size me from the scene", so nothing that already worked changes.

`instancesOf(ModelId)` was **not** added, because it already exists -- the same capability,
already public, already documented. Found by writing it a second time and having the compiler
refuse the overload, which is the cheapest possible way to find out and still one worth
recording: the engine surface is large enough that "a game cannot reach X" is worth grepping
before it is worth believing.

## Verification results

- `scripts/golden.sh check release` — **all 11 cases match**, byte-identical. No golden case
  has a player character in it, which is the point: this row adds a door and moves no pixel.
- `--validation on`, 200 frames, the player driven by script: **zero** `VUID-`/validation
  errors, and the log carries `Locomotion: idle -> walk at step 65 (0.83 m/s, grounded)` then
  `walk -> run at step 156 (2.44 m/s, grounded)` — a state machine driven by a controller the
  *game* made and handed over, which is the whole of what this card adds.
- `scripts/locomotion.sh`: **four arms pass with zero assertions failed** — `still`,
  `modifier`, `walk-run-jump`, `jump-buffered` — and `walk-run-jump` is the real one, landing
  the six-state path, 8.21 m travelled and the 0.899 m jump arc inside bounds tuned against a
  character the *file* used to declare.

  The suite did not finish, twice, and not on this row's account: `vkCreateDevice` hung for
  twenty-three seconds and returned `VK_ERROR_DEVICE_LOST`, on `jump-eaten` the first time and
  `jump-buffered` the second. A different arm each time, with `golden.sh` creating eleven
  devices back to back in the same session without a stumble. Card:
  `bug-the-device-is-lost-part-way-through-a-suite`. **Three arms are unrun**, and they are
  camera-relative movement rather than the pairing this card is about.
