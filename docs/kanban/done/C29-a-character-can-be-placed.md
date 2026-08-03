---
id: C29
title: A character can be placed
arc: C
size: S
verification: tests-hosted, golden-11
---

# C29 — A character can be placed

Afterwards `PhysicsWorld` has a verb that moves a character controller to a stated transform,
so a respawn, a checkpoint, a portal, a level transition and a loaded save are all expressible.
Today the character surface
([`Physics.h:380-412`](../../../engine/scene/Physics.h#L380)) is complete and contains no such
call: `characterTransform`, `setCharacterInput`, `characterSpeed`, `characterMoveSpeed`,
`characterOnGround`, `characterGround`, `characterJumped`. A character is placed once, from
its `ColliderDesc`, and thereafter only walks.

Rigid bodies already have this, and their doc already makes the argument this row needs —
`setBodyTransform` ([`Physics.h:275-293`](../../../engine/scene/Physics.h#L275)) names the
cases outright: "a respawn, a level reset and a portal are the same operation". None of them
is available to the one thing in a game most likely to need them.

The consequence composes badly with what already exists. `Engine::loadGame` restores instance
transforms and cannot put the player back where they stood, so the engine's own save path is
incomplete for any game with a character in it. A networked client applying a server position
correction has no call. An editor's "drop the player here" has no call.

**The interesting part is not the setter, it is what it must do to the solver.**
`CharacterVirtual` carries velocity, a ground state and a stick-to-floor step; a teleport that
leaves the previous frame's velocity in place arrives falling, and one that leaves the
interpolation snapshot behind smears the mesh across the map for the rest of the frame.
`setBodyTransform` already writes both snapshots for exactly that reason
([`Physics.cpp:1258-1264`](../../../engine/scene/Physics.cpp#L1258)) and is the precedent to
follow.

Expected to be wrong about: whether one verb is enough, or whether a teleport and a
"reposition but keep moving" (a conveyor, a moving-platform hand-off, a network correction)
are two different calls. Guessing they are one is the cheaper mistake to undo.

## Verification

- `./test.sh debug`, then `./test.sh asan`. `scene/Physics.cpp` is in
  `SUBSTRATE_HOSTED_SOURCES`, so this runs with no device: place a character mid-fall, assert
  it arrives with no residual velocity, is grounded on the step after landing, and that both
  interpolation snapshots agree so no frame reads the old position.
- `scripts/golden.sh` — eleven cases, byte-identical. No golden case moves a character.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the physics section's character
subsection, which lists the surface.

## Outcome

One verb, `setCharacterTransform`, and the guess that a teleport and a "reposition but keep
moving" are the same call was not tested by anything this row did — it stands, and the cheaper
mistake is still the one available to undo.

**What the estimate did not predict is where the work was.** The card said the interesting part
is what the setter must do to the solver, and named velocity and the interpolation snapshots.
Both were one line each and neither could have failed quietly. The part with teeth was a third
thing the card did not name: `CharacterVirtual` caches its ground state, `step()` reads that
state **before** it sweeps, and `SetPosition` does not invalidate it. So a character teleported
off a floor reports standing on it for one more step — which is a whole coyote window, and long
enough to jump off ground a hundred metres away. `RefreshContacts` at the placement is the fix,
and the two-armed test is the evidence: with the refresh commented out, a character placed 10 m
up jumps; with it, only the arm placed on the floor does. The two arms differ in that number
alone, so an implementation that simply refused every jump after a placement fails the other.

The jump windows are reset to what `createCharacter` leaves — `airSteps` saturated,
`coyoteSpent` true — rather than to zero, and that is the same argument arriving twice: a
character that has not been swept is not standing on anything.

**Verification.** 976 tests, debug and ASan, three of them new. `scripts/golden.sh` — 11 of 11,
byte-identical, as expected: no golden case moves a character.
