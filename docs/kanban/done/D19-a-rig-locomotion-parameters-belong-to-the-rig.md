---
id: D19
title: A rig locomotion parameters belong to the rig
arc: D
size: S
verification: tests-hosted, golden-11
---

# D19 — A rig locomotion parameters belong to the rig

Afterwards each `LocomotionDriver` pair carries its own parameter names, so two rigs that spell
their blend parameters differently both animate. Today there is one `Parameters names;` member
for the whole driver ([`Locomotion.h:83`](../../../engine/scene/Locomotion.h#L83)), one
`setParameters`, and the loop in `Locomotion.cpp:34-37` looks up `names.speed`,
`names.airborne` and `names.jump` in *every* pair's machine.

**The header already argues against the code it sits on.**
[`Locomotion.h:37-43`](../../../engine/scene/Locomotion.h#L37): "the parameter names… belong
to the *rig*… the whole point of a machine per character is that two characters may run
different ones." They cannot. A second rig spelling its parameter `Speed` or `locomotion/speed`
gets `findParameter` → `kAnyState` → silently skipped, and the header's own "a rig with no
`airborne` is a rig that does not care" rule turns a name mismatch into an animation that
simply never blends. Worse, `setParameters` is global, so naming the second rig's parameters
un-names the first's.

**The same defect was already fixed one layer up, which is what makes this a D row.** C23 moved
the state machine off the animator and onto the character —
[`Animation.h:609-614`](../../../engine/scene/Animation.h#L609), "This character's machine, not
the animator's" — and gave each character its own root node. `LocomotionDriver` was written
against that plural animator and kept a singular field. The inconsistency is between two
adjacent layers, not between the code and a want.

Who this blocks: any game with a bestiary — a human, a horse and a spider from three exporters
— and anything importing marketplace rigs, which is most games. The workaround is renaming
every vendor's parameters to match, in the asset, forever.

`Pair` is `{PhysicsCharacterId controller, AnimatorId rig}` and gains a `Parameters`. `pair()`
gains an optional argument; the existing `setParameters` either becomes the default for
subsequently-created pairs or goes away. Deciding which is most of the row.

Expected to be wrong about: whether the names belong on the pair or on the `SceneAnimator`
character itself, beside the machine C23 put there. The second is tidier and touches a class
this row otherwise leaves alone.

## Verification

- `./test.sh debug`, then `./test.sh asan`. `scene/Locomotion.cpp` and `scene/Animation.cpp`
  are both in `SUBSTRATE_HOSTED_SOURCES`, and `tests/LocomotionTests.cpp` already builds two
  characters on two machines from two colliders — the case extends to two *parameter
  vocabularies*, both blending.
- `scripts/golden.sh` — eleven cases, byte-identical. No golden case runs a locomotion pair.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the animation and locomotion
sections.

## Outcome

**On the pair, not on the animator character**, and the card's "the second is tidier" does not
survive being asked what it would cost: `SceneAnimator` would have to know that a locomotion
parameter is a thing, and that `speed`, `airborne` and `jump` are the three of them. The animator
has no business knowing there is such a concept — its machines are a general blend graph, and
the names are the *driver's* contract with one, not the machine's own data. Tidier by line count,
one layer worse.

**`setParameters` neither became a template nor went away; it does both halves.** The card said
deciding that was most of the row, and the precedent that settled it is the one the card itself
points at: C23's `setStateMachine` with no character writes every existing character *and* seeds
the ones made later. Doing only the second buys an order dependence — pair-then-name behaving
differently from name-then-pair — which is exactly the class of defect this row is closing.
`TheDriverWideCallStillReachesEveryPairWhicheverOrderItIsMadeIn` is that argument as a test, and
`TheNamesAreTheRigsAndCanBeReplaced` (which pairs first) kept passing unchanged, which is the
evidence the existing contract survived.

One thing neither the card nor the header raised, found while writing the code: **re-pairing
replaces the controller and keeps the names.** A rig handed a new body — a respawn, C29's
placement into a fresh controller — would otherwise slide silently back onto the defaults, which
is the same failure this row exists to remove, one call further along.

**Verification.** 989 tests, debug and ASan, four of them new: two rigs on two vocabularies both
blending, naming one leaving the other alone, the order-independence of the driver-wide call, and
re-pairing keeping the vocabulary. `scripts/golden.sh` — 11 of 11, byte-identical; no golden case
runs a locomotion pair.
