---
id: G17
title: A game names its players
arc: G
size: M
verification: golden-11, tests-hosted, validation
---

# G17 — A game names its players

Afterwards a game can say that it has four players, or none, and everything the engine keys on
"the player" works for each of them. Today `Engine` holds one slot:

```cpp
// engine/Engine.h:632-633
scene::PhysicsCharacterId playerCharacterIndex;
scene::NodeId playerCharacterNode;
```

It is assigned first-wins from whichever `Character` collider the loader walks
([`Engine.cpp:717`](../../../engine/Engine.cpp#L717) and
[`:1385`](../../../engine/Engine.cpp#L1385)), and `setPlayerCharacter` replaces rather than
adds.

**This is the G arc's exact shape, and G16 is why the row is worth opening rather than
closing.** G16 added the setter and named its own uncertainty: "Expected to be wrong about:
whether one setter is enough." It was not. The tables underneath are already plural —
`PhysicsWorld` holds a slot vector with generations and a free list, `LocomotionDriver` updates
every live pair, and `Engine::pairLocomotion` derives pairs per instance without reference to
the player slot. The singular hole is punched at the top, in the convenience accessor, and
everything that guards on `playerCharacter().valid()` inherits it.

Who this blocks: four-player local co-op, where three players are unnameable; a party RPG with
a controlled leader and a swap key; an RTS or city builder, which has no player character at
all and gets an engine field that is permanently invalid plus a heuristic that silently
promotes an NPC; a possession game where the player's body changes and both bodies must stay
addressable.

Composes with [C26](C26-input-devices-keep-their-identity.md), and the order matters: naming
four players is not useful while every gamepad is merged into one state before the input map
sees it. C26 is the one to land first; this row is what a second player is *for*.

The question the row has to answer is whether "player" survives at all as an engine concept.
The engine uses the slot for nothing but binding — camera, input, navmesh and gait are all the
demo's or already plural — so the honest answer may be that `Engine` stops holding it and the
game does, with the loader's first-wins guess becoming a returned list rather than a stored
choice.

Expected to be wrong about: how much breaks. `playerCharacter()` reads as harmless and is
reached by the demo, `--input-script` and the follow camera; retiring it is a wider edit than
its two members suggest.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. No golden case has a player character,
  which is what G16 relied on too.
- `./test.sh debug`, then `./test.sh asan`: two controllers, two rigs, two locomotion pairs,
  both driven and both animating, with no device.
- Zero validation errors with layers on.

## Reference update

[architecture/systems.md](../../architecture/systems.md) and
[limitations.md](../../architecture/limitations.md) — "The game API" section, which has eleven
rows from G9 and G12 and does not have this one.

## Outcome

**"Player" does not survive as an engine concept, and the row's own suspicion was the answer.**
`playerCharacterIndex`, `playerCharacterNode`, `playerCharacter()`, `playerNode()` and
`setPlayerCharacter` are gone rather than plural. `Engine::authoredCharacters()` replaces them:
a `std::vector<AuthoredCharacter>` of every `Character` collider the loader walked, each with
the node it was attached to — the loader's guess returned as a list rather than stored as a
choice, which is what the card proposed.

What decided it was a count. Grounding the row turned up **five reads of the slot in the whole
engine, and not one of them is a use**: two are the accessors handing it out, two are
`!playerCharacterIndex.valid()` guarding its own first-wins write, and two are
`a.character == playerCharacterIndex` comparing it against itself to find the node.
`playerCharacterNode` is never read in `engine/` at all. Nothing in the frame loop, the
renderer, the audio, the navigation or the locomotion driver ever asked who the player was. So
the singular was never a constraint the engine was under — it was a promise the API made, and
the promise was wrong: three of four players unnameable, and an RTS handed a permanently
invalid field plus a heuristic that silently promoted the first NPC the walk found.

Making it plural would have meant maintaining a table for a reader that does not exist. Nothing
`Engine` could do with a second player it cannot already do with a second `PhysicsCharacterId`.

The card was right that this is wider than its two members suggest, though not in the way it
expected — the breakage is entirely on the game's side of the line, which is the point. The
demo now holds `DemoWorld::Player {character, node, rig}` and a `std::vector<Player>` (which
absorbed `playerRig`), with `playerCharacter(p)`, `playerNode(p)` and `playerRig(p)` accessors
so a call site that only ever means player zero still reads as one token. Nineteen call sites
moved from `e.` to `world.`, and `DemoGame::init` now runs the adoption the engine used to:

```cpp
if (world.players.empty()) {
    for (const Engine::AuthoredCharacter& c : e.authoredCharacters()) {
        world.players.push_back({c.character, c.node, {}});
    }
}
```

**That block is load-bearing for the golden set.** `physics.gltf` is one of the eleven and
authors a `Character` collider; for those scenes `buildDemoWorld` returns early and builds
nothing, so without it the demo would have had no player, `driveLocomotion` would have fallen
through to `driveCharacters`, and the case would have animated differently. Moving the
heuristic into the game rather than deleting it is what keeps it a decision — visible, in one
place, and taking every authored character rather than only the first.

Two smaller things fell out. `initPhysics` now clears the list it rebuilds, which the old
fields never did on any path — `applyPendingScene` and `removeModel` both left
`playerCharacterIndex` naming whatever the previous scene had. And the demo's planner had a
local `ai::WorldState world` shadowing the member `world`; harmless until the member grew a
`playerNode()` the same block reads, at which point it was a compile error rather than a
silent wrong answer. It is `state` now.

### Verification

- `./test.sh debug` — 1003 tests from 102 suites, all passed.
- `./test.sh asan` — 1003 tests from 102 suites, all passed.
- One new hosted case, `LocomotionDriver.TwoPlayersOnTwoPadsDriveTwoCharactersToTwoDifferentGaits`:
  two `InputMap` players on two pads, two controllers, two rigs, two locomotion pairs, both
  driven and both animating, with no device. The two rigs are deliberately identical, so
  anything that differs at the end differs because the *players* did.
- `scripts/golden.sh` — all 11 cases match, byte-identical. The card was right that no golden
  case has a player character; `physics.gltf` has an authored one, which is what the demo's
  adoption block exists to keep driving.
- `scripts/locomotion.sh debug` — 9 of 9 arms pass, which is the check that the demo's
  migration off the engine's slot did not change what the character does.
- Validation clean: `./run.sh demo debug -- --headless --locked --audio-null --frames 200
  --validation on` exits zero with zero VUIDs and zero layer messages.

**The golden suite failed once before it passed, and it was not this change.** The first run
reported `FAIL lit -- 1 of 11 cases differ`; the case had never rendered, because `run.sh`'s
`[ ! -x "$BIN" ]` guard fired straight after `build_game.sh` returned successfully. An
immediate re-run gave `all 11 cases match`. Recorded rather than shrugged at, as
[its own card](bug-a-golden-case-can-fail-because-the-binary-vanished.md) — a harness flake
that presents as a pixel regression, and whose log the re-run destroys, is exactly the thing
that makes the next real failure cheap to misread.

