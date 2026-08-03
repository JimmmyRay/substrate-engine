---
id: bug-a-kinematic-platform-teleports-so-nothing-can-ride-it
title: A kinematic platform teleports so nothing can ride it
arc: bug
size: S
verification: tests-4, golden-11
---

# bug-a-kinematic-platform-teleports-so-nothing-can-ride-it — A kinematic platform teleports so nothing can ride it

A character standing on the demo's sliding platform stayed where it was while the platform
went out from under it.

**`CharacterVirtual` does not carry itself.** `PhysicsWorld::step` already knows that and
already does the work: it reads `GetGroundVelocity()` and adds it to whatever the controller
asked for, so a rider's velocity is the platform's plus its own. That side was right the
whole time, which is why the defect reads as a physics bug and is not one.

What was wrong is one line at the other end. `setBodyTransform` wrote every body with
`SetPositionAndRotation` -- a **teleport**, which puts the body somewhere new carrying the
velocity it already had. For a platform driven from a scene node that velocity is zero and
stays zero, so `GetGroundVelocity()` answered zero and the rider was told the ground was
still. `MoveKinematic` is Jolt's verb for the other thing: it sets the velocity that arrives
at the target in one step, and *that* is the velocity the character reads.

The path in is `Scene::update`, which pushes a node's world transform into any kinematic body
attached to it -- so this reaches every kinematic body in the engine, not the demo's platform
alone. Nothing had ridden one before, which is why it lasted.

The step handed to `MoveKinematic` is `PhysicsConfig::step` rather than a measured frame
delta, because that is the clock the solver runs on. The cost is stated in the comment: a
frame running two steps asks for the whole frame's travel in one step's time and overshoots
by one step, which the next sweep corrects. That is visible only below the step rate, and the
alternative is threading a per-frame delta through a verb whose whole point is being callable
from outside the step loop.

## Verification

- `./test.sh` in four configurations -- 923 tests.
  `PhysicsCharacter.RidesAKinematicPlatformInsteadOfStandingStillWhileItLeaves` builds a
  waist-high kinematic slab, stands a capsule on it, writes the slab once per step for two
  seconds and asserts the rider covered at least 80% of the travel, that the slab itself
  arrived, and that the rider is still on the ground.

  **Checked in both directions**, which is the half worth recording: with the
  `MoveKinematic` branch disabled the test fails on "the rider was left behind by the
  platform", so it is testing the fix rather than agreeing with whatever the code does.

  The assertion is a fraction of the travel and not a position. The rider is not glued to
  the slab -- friction and the one-step lag between asking and arriving mean it tracks
  closely rather than exactly -- and pinning the offset would pin Jolt's solver instead of
  the behaviour.

- `scripts/golden.sh` -- eleven cases, byte-identical. `physics.gltf` has no kinematic body
  in it, so none of the eleven exercises this; that gap is why the unit test is the real
  check here.

## Reference update

None. The distinction lives in the comment at the branch, which is where someone reaching for
`SetPositionAndRotation` because it is the obvious call will meet it.

## Outcome

Fixed in `engine/scene/Physics.cpp`. `ColliderMotion::Kinematic` was the third motion value
and the demo's platform was the first thing in the tree to use it, so this is the first time
anything asked a kinematic body to behave like one.
