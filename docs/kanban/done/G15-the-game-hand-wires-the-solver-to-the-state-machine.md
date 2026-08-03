---
id: G15
title: The game hand-wires the solver to the state machine
arc: G
size: M
verification: golden-11, tests-hosted, inspection
---

# G15 — The game hand-wires the solver to the state machine

`SceneAnimator` will read the character controller itself, so that a rig with a `speed`,
`airborne` or `jump` parameter is driven from the solver without a game writing a line. What
the tree holds afterwards is a driver in `engine/scene/` that maps a `PhysicsCharacterId` to
an `AnimatorId` and writes the parameters that pairing implies, and a `DemoGame` that has
deleted most of `driveLocomotion` rather than gained a second copy of it.

Today that wiring is the game's, and it is eighty lines of it: read `characterSpeed`, divide
by a magic 4.0 to normalise, read `characterOnGround`, read `characterJumped`, then loop over
every character writing three parameters and firing one trigger. It works for the one rig the
demo has. **A second rig is a second function**, because the parameter names, the normalising
divisor and the set of triggers all belong to that rig rather than to the engine, and nothing
about the current shape lets two rigs disagree about them without two copies of the loop. That
is the objection this card exists to answer, and it was raised against the demo rather than
against a hypothetical.

The normalising divisor is the part that has to move somewhere honest rather than merely
somewhere else. `speed / 4.0` is the game asserting that the machine's `run` threshold sits at
4 m/s, which is a fact about the *rig's* thresholds and the *collider's* `moveSpeed` — two
things the engine holds and the game had to guess consistently with. Driving from a normalised
`speed / moveSpeed` makes the assertion checkable, and makes a rig authored against a different
top speed work without editing a game.

What this does not do is decide *what the character is trying to do*. That is a separate layer
and a separate card ([C24](C24-a-planner-decides-what-a-character-is-trying-to-do.md)); this
one only stops the game from re-deriving, per rig, facts the engine already has. Landing it
first is deliberate — it is what makes C24 a small card instead of a large one, because a
planner that emits an intent has somewhere to emit it to.

The parameter names stay strings and stay the rig's. An engine that hard-coded `"speed"` would
be an engine that decides what a state machine may contain, and the whole point of
[C23](../done/C23-a-state-machine-per-character.md) was that two characters may run different
machines. The driver looks the names up, writes what it finds and is silent about what it does
not — a machine with no `airborne` parameter is a rig that does not care whether it is in the
air, not a misconfiguration.

## Verification

- `./test.sh debug`, then `./test.sh asan`, each its own invocation. `Physics.cpp` and
  `Animation.cpp` are both in `SUBSTRATE_HOSTED_SOURCES`, so the pairing and every parameter
  it writes are testable without a device — including the case this is really about, which is
  two characters on two machines fed from two colliders.
- `scripts/golden.sh` — eleven cases, byte-identical. This moves no pixel: `skin` is the only
  case with a rig and it has no controller, so a driver reading a controller has nothing to
  say about it. A difference there is the finding.
- `inspection` for the demo itself, because the property that matters is one no test states:
  `driveLocomotion` must be *shorter* afterwards, and a second rig must need nothing added to
  it. A card that moved the eighty lines into the engine and left the game calling them one by
  one has passed its tests and failed its purpose.
- The demo's locomotion trace unchanged at `0 changes over 200 steps, drift 0.02` with no
  input, which is the negative control that catches a driver animating a character nobody is
  driving — the failure this exact area produced twice already.

## Reference update

[architecture/systems.md](../../architecture/systems.md), in the animation section beside
C23's per-character split: what the engine now writes on a game's behalf, and which parameter
names it looks for.

## Outcome

`scene::LocomotionDriver` in `engine/scene/`, `PhysicsWorld::characterMoveSpeed`,
`Engine::pairLocomotion` and `Engine::locomotion()`, nine hosted cases, and the game's
wiring deleted. `DemoGame::driveLocomotion` loses its three `findParameter` lookups, its
three solver reads, the magic divisor and the loop that wrote to every character — what is
left is the three things that genuinely are the *game's*: which joint holds root motion, how
the mesh turns to face where it went, and the trace G12 verifies the chain with.

**The pairing is derived, not declared, and that is what makes "a second rig needs nothing
added" true.** A `CharacterVirtual` is a capsule with no rig; an animator character is a pose
with no collider. What joins them is a skinned mesh: the scene bound the instance to the
collider's node, and `InstanceTable::characterOf` says which pose deforms it. Both places
where `Engine` binds physics to the tree — `initPhysics` and `addModel` — now pair on the way
past, one line each. `Engine::locomotion()` is the door for a rig the engine cannot see.

**The divisor was wrong, and now it is checkable.** The card predicted that `speed / 4.0` was
the game asserting the machine's `run` threshold sits at 4 m/s. The collider it was asserting
about tops out at **3.2** — so the parameter could never exceed 0.8, **the top fifth of every
blend was unreachable**, and nothing anywhere could have said so. `scripts/locomotion.sh`
carried the same guess as a derived constant and its comment stated it outright: *"the machine
compares a `speed` parameter the demo normalises against 4.0, so 0.66 and 0.2 are 2.64 m/s and
0.8 m/s."* Both are `MOVE_SPEED`-derived now, at 2.11 and 0.64, and the eight arms pass at the
new thresholds with every path, transition step, distance and facing ratio intact.

**After the step, not before it** — the ordering is the one design decision worth recording.
The demo guarded `stepIndex() == 0` because a `CharacterVirtual` that has never been stepped
reports its constructed ground state, which reads as *in the air*, and driving from it made
every character fall and land in the first three steps of every run. The driver runs after
`PhysicsWorld::step`, so there is no such moment to guard: the guard is gone from the engine
side and survives in the demo only around the *measurement*, which reads a transform.

**Nine hosted cases, and the one that matters is `TwoCharactersOnTwoMachinesFedFromTwoColliders`.**
Two colliders at 4.0 and 1.0 m/s, two machines that spell `speed` at *different indices* — the
second lists it last — and the driver has to get both right at once. The slow one walking flat
out reads 1.0 against its own top speed rather than 0.25 against the other one's. That is the
case the demo cannot show and the objection this card was raised against.

**A pre-existing failure was found and is not this card's.** `scripts/locomotion.sh` asserts
`drift <= 0.02` on all eight arms and six report more — 0.05 walking, 0.50 through a jump.
Suspecting itself, this card built the tree at **`36522a9`**, the commit this session started
from, and got the identical 0.50. The hold is working (the counterfactual is 3.17 m); what has
happened is that `poseDrift` measures all three axes while `setRootNode` deliberately keeps Y,
so every centimetre of authored hip bob lands in a number bounded at two. Opened as
[bug-the-pose-drift-check-has-been-red-since-it-was-written](../backlog/bug-the-pose-drift-check-has-been-red-since-it-was-written.md),
with the more interesting half recorded there: **`locomotion.sh` is not in the closing gate**,
so it went red without a single card noticing.

To verify this card against a suite one assertion of which is already red, the eight arms were
run with that one bound widened and nothing else changed: **8 of 8 pass**.

943 tests in each of debug, release, asan and tsan, up from 934. `scripts/golden.sh check
release`, eleven of eleven byte-identical — `skin` is the only case with a rig and it has no
controller, so a driver reading a controller has nothing to say about it, and it did not.
