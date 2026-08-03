---
id: C30
title: The solver vectors and turning toward a heading
arc: C
size: M
verification: tests-hosted, scripted-input, golden-11
---

# C30 — The solver vectors and turning toward a heading

Afterwards the vector quantities the solver already computes are readable, and turning a thing
to face where it is going is a call that takes a **node and a direction** rather than a
`PhysicsCharacterId`. Today neither exists, so no game can turn anything toward its motion and
the demo re-derives a heading by differencing world transforms — which is the defect in
[bug-a-carried-character-turns-to-face-what-carries-it](bug-a-carried-character-turns-to-face-what-carries-it.md).

**The engine computes the right number and throws it away.** The character step subtracts the
ground's velocity to get a genuinely ground-relative motion, then keeps only its magnitude:

```cpp
// engine/scene/Physics.cpp:1146-1150
const JPH::Vec3 under = cv.GetGroundState() == ...OnGround ? cv.GetGroundVelocity() : Vec3::sZero();
const float vx = v.GetX() - under.GetX();
const float vz = v.GetZ() - under.GetZ();
c.speed = std::sqrt(vx * vx + vz * vz);
```

`characterSpeed` returns that scalar and the direction is unrecoverable. The same paragraph
applies to two more quantities the solver holds and reports nowhere: the ground *normal* (no
slope lean, no ski, no wall-run) and the ground *body* — a game cannot ask what it is standing
on, so a moving platform, a conveyor and an enemy's head are indistinguishable.

**The turning half must not be keyed on a character.** A vehicle facing its velocity, a turret
facing a target, a projectile, a boat, an aircraft, an AI agent on a navmesh and a kinematic
lift all want the identical shortest-arc slew toward a heading; none of them is a
`PhysicsCharacterId`. The demo's version — `shortestTurn`, a rate clamp, a floor speed below
which a step is rounding rather than a heading, and an `angleAxis` written onto every child
node named `mesh` ([`DemoGame.cpp:644-665`](../../../game/demo/DemoGame.cpp#L644) and
[`:1456-1474`](../../../game/demo/DemoGame.cpp#L1456)) — is the shape, and its inputs are a
node, a direction, a rate and a floor. Nothing in it is about being a character.

G13 built that code and was right to build it in the demo: it was one caller, and the arc rule
said so. It is now the third thing that wants it and the first that cannot have it.

The rig-authored-forward offset is a parameter, not a constant: `DemoGame.cpp:1466-1467`
records that a rig authored facing some other way "wants that offset subtracted here", which
is true of every imported rig that is not Mixamo's.

Expected to be wrong about: whether the turn belongs on the scene tree as a per-node property
the sweep applies, or as a call a game makes each step. The tree version composes with
attachment and gets N-of-them for free; it also puts a policy into `Scene` that a game may
want to own.

## Verification

- `./test.sh debug`, then `./test.sh asan`. Both halves are hosted: a character carried by a
  kinematic body reports a ground-relative velocity of about zero while its world displacement
  is not zero, and the slew reaches its heading by the shortest arc across the ±π seam.
- `scripts/locomotion.sh`, whose `facing >= 0.85` arm reads the rotation back out of the scene
  tree rather than off the angle the game wrote — so a turn that never reached the tree scores
  zero.
- `scripts/golden.sh` — eleven cases, byte-identical. No golden case has a moving character.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the physics character surface, and
whichever section gains the facing verb.

## Outcome

**The two halves landed a commit apart and the split was not planned.**
`characterVelocity` went in with the carried-character bug, because that row could not be
fixed without it — which is the card's own claim that the bug is C30's first caller, arriving
as a fact rather than a prediction. What was left here is the other two solver quantities and
the whole turning half.

`characterGroundNormal` and `characterGroundBody` are the pair the card named, and both filter
the air case rather than reporting it: Jolt keeps the last ground it found, so a normal from a
face the character has left reads as a slope it is not on. Falsy body, +Y normal.

**The turn is `Scene::turnToward`, and the card's "expected to be wrong about" was decided
toward the call rather than the tree property** — but not quite as either option was written.
It is a call *on* `Scene`, which is what a verb taking a node has to be, and the policy the
card worried about handing over stayed with the caller: when a thing turns and what toward is
the game's, the shortest arc and the rate clamp are the engine's. What the demo kept is four
lines; what it lost is `facingYaw`, and that turned out to be the interesting part. The angle
and the tree were two copies of one fact, and the copy `locomotion.sh` asserts against is the
tree's — so the verb reads the yaw out of the node it is about to write, and there is nowhere
to keep a second one.

The rig-authored-forward offset is a parameter as the card said, and `std::remainder` does the
seam in one call rather than the demo's four-line wrap. One thing the card did not name: the
angle handed back is folded into (-π, π], because a turn *through* the seam is exactly what
produces an unbounded angle in a caller that keeps one — which the first version of the seam
test caught by asserting 3.19 rad against π - 0.05.

**Verification.** 982 tests, debug and ASan, six of them new — four on the turn (the seam, the
rate, the floor, the offset) and two on the ground pair. `scripts/locomotion.sh debug` — 9 of 9
arms, every number identical to the run before the turn moved into the engine, which is what
says the extraction changed nothing. `scripts/golden.sh` — 11 of 11, byte-identical.
