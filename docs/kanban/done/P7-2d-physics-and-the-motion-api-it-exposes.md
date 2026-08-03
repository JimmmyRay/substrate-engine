---
id: P7
title: 2D physics, and the motion API it exposes
arc: P
size: M
verification: tests-4, golden-11, readback, validation
---

# P7 — 2D physics, and the motion API it exposes

`Freedom::Plane2D` on `ColliderDesc`, plus `addImpulse` / `setLinearVelocity` / `setTransform`. **The second half is the larger one and is a gap neither other arc names**

## A body can be pushed, and can be told to stay in a plane

```cpp
BodyId crate = e.physics().createBody(ColliderDesc{
    .motion  = Motion::Dynamic,
    .freedom = Freedom::Plane2D,               // XY translation, Z rotation
});
e.physics().addImpulse(crate, {5.0f, 0.0f, 0.0f});
```

---

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- The unit suite in four configurations, each its own invocation:
  `./test.sh debug`, `release`, `asan`, `tsan`.
- Zero validation errors with layers on, in every capture.

Added on the way in, because the two above would pass a constraint that did not work.
**Nothing in the list as written asks whether a confined body stays confined**, and the
`tests-4` line is a count of configurations rather than a claim about coverage:

- **A body confined to a plane stays on it, checked as an equality rather than a
  tolerance.** Three hundred steps of gravity, floor contacts and impulses pushed along the
  forbidden axis, with the plane coordinate asserted *exactly* unchanged every step. The
  solver constrains by zeroing the inverse mass, so exactness is the property; an
  implementation that clamped the position after each step would pass a tolerance and fail
  this.
- **A control arm.** The same world with `freedom` left at its default must drift off the
  plane under identical impulses, or the check above is measuring nothing.
- **A confined body still collides and settles**, which is the claim that constraining Jolt
  beats introducing a second solver.
- Physics is deterministic here and the layer is in `SUBSTRATE_HOSTED_SOURCES`, so all of
  this is provable in the unit suite rather than by eye.

- `golden-11`, added for the same reason: the `physics` case is directly exposed to
  anything this row touches in `createBody`, and a motion API must move no rendered pixel
  until something calls it.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

**Landed: four calls and one enum field.** `PhysicsWorld::addImpulse`,
`setLinearVelocity`, `linearVelocity`, a widened `setBodyTransform`, and
`ColliderDesc::freedom` — `ColliderFreedom::All` or `ColliderFreedom::Plane2D` — which
becomes Jolt's `EAllowedDOFs` in `createBody`. About 90 lines of engine, 200 of tests and
the documentation; the **M** was generous, and the reason is that the interesting half was
deciding rather than writing.

**How it relates to G3's `setBodyTransform`: it subsumes it, rather than complementing it
or being unified with it after the fact.** The card asked for `setTransform` and there was
already a method doing exactly that, added by G3 for kinematic bodies only and refusing
everything else. Adding the card's verb beside it would have produced two spellings of one
operation differing only in which motion type they refused, so the existing one was widened
to dynamic bodies and no new verb landed. The refusal G3 wrote was *"the solver owns that
transform"*, and the widening is the observation that the solver owns how a body **moves** —
a respawn, a level reset and a portal are not movement. Static is still refused, and now
for the only reason that was ever specific to it: `finalize()` decided its place in the
broad phase. **A teleport keeps the body's velocity**, which is what makes a portal
expressible and a respawn two calls; that is a large part of why `setLinearVelocity` and
the widening landed together rather than in either order.

**The naming is not uniform and that was the decision, not an oversight.**
`setBodyTransform` keeps its prefix because `bodyTransform` and `characterTransform` both
exist, and a getter and its setter spelled two different ways is worse than a long name.
The three velocity verbs are unprefixed because velocity has no character counterpart and
will not grow one — a `CharacterVirtual` is asked for motion through `setCharacterInput`,
which is a request rather than an assignment — so the handle type is the only thing that
has to say which kind of thing is moving, and it does. That is the same argument `destroy`
already runs on.

**What the verification found.** Nothing failed, and the two checks added to this card on
the way in are why that is worth stating rather than a shrug: the four configurations and
the validation run would all have passed a `freedom` field that was read, stored and never
reached the solver. The plane test asserts an *equality* — `positionOf(body).z ==
start.z`, every step, for 300 steps, through gravity, floor contacts and 90 kg·m/s pushed
straight along the forbidden axis — and it holds because Jolt zeroes the disallowed rows
and columns of the inverse mass and inverse inertia, so the body is never solved off the
plane and there is nothing to correct. The control arm drifts to z > 4 under identical
treatment. The prediction that had to be checked before writing the test that way was that
the constraint is exact in float rather than nearly exact; it is, because a masked velocity
component is exactly zero and `position += 0 * dt` is exact.

**One thing found and fixed on the way, and it was documentation rather than code.** Two
headers — `engine/Engine.h` and `engine/scene/WorldSave.h` — each argued that the save
system does not restore rigid bodies *because there is no `PhysicsWorld::setBodyTransform`*.
That had been false since G3 and this row makes it emphatically so. The refusal itself is
still right, so both now give the reason that survives: a body has no identity in the file,
because the section is keyed by instance slot and a body is not an instance.

**Deferred, each with its trigger**, and all six are written into
`architecture/limitations.md` rather than only here: an off-centre `addImpulse` and the
angular trio (a caller that wants a hit to impart spin), `addForce`/`addTorque` (a caller
integrating something continuous — an impulse is `force * step` and a fixed-step game can
spell it), a third `freedom` (nothing; a game that wants another plane rotates its world),
and **a body driving a sprite** — a `BodyId` and a `SpriteId` are two handles into two dense
tables, binding them is four lines in a game's loop today, and the trigger is P5 or P6
needing it, at which point it is an `Attachments` field on a G3 node rather than a third
table.

**Found and left alone**, because it belongs to other rows: `limitations.md`'s Physics
section still says *"Kinematic bodies exist and nothing drives one"* (G3's scene sweep
drives them) and *"Contacts are drawn and never delivered"* (G7 delivers them). Both are
stale in the arcs that landed them, not in this one.

**Verification.** `./test.sh debug` 733/733, `release` 733/733, `asan` 733/733,
`tsan` 733/733 — nine new cases, five `PhysicsMotion` and four `PhysicsFreedom`, plus the
`freedom` spelling round-trip in `ColliderTests`. `scripts/golden.sh check release` 11 of
11 byte-identical, including `physics`. `scripts/readback.sh release` 7 of 7 bit-identical
plus the resize soak. A 200-frame headless run of `engine/assets/physics.gltf` in debug
with the validation layer on: zero errors, zero warnings from the layer.
