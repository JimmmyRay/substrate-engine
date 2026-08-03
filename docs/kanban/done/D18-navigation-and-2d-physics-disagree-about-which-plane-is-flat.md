---
id: D18
title: Navigation and 2D physics disagree about which plane is flat
arc: D
size: M
verification: tests-hosted, golden-11
---

# D18 — Navigation and 2D physics disagree about which plane is flat

Afterwards the two subsystems that have an opinion about which plane a flat world lies in
agree, and a 2D game can use both. Today they contradict each other, and the contradiction is
stated in each one's own header.

Navigation is XZ with Y up. [`NavMesh.h:44-48`](../../../engine/scene/NavMesh.h#L44): "Y is
up… 'Slope' is the angle between a triangle's normal and +Y". `steer`
([`NavMesh.h:260-274`](../../../engine/scene/NavMesh.h#L260)) drops Y from both the direction
and the arrival distance.

2D physics is XY with Z the rotation axis.
[`Collider.h:84-95`](../../../engine/scene/Collider.h#L84): `ColliderFreedom::Plane2D` is "X
and Y translation, Z rotation", and it declines a switch — "a game that wants another rotates
its world rather than the engine growing a switch."

Both choices are defensible alone. Together they mean a 2D game's bodies live in XY and the
only navmesh it can bake lives in XZ, so the two cannot both be used and one of them has to be
rewritten by the game. That is a D row by the letter of the rule: no capability is missing,
two parts of the tree are inconsistent, and the inconsistency was introduced at different
times by rows that did not know about each other.

The P arc is the arc that cares — this is what a flat world needs, and P rows are where
"the world can be flat" is argued. It is filed as D because the finding is the disagreement
rather than the 2D capability.

**The decision half is the row.** The retrofit is small either way; choosing is not. Rotating
the navmesh to XY breaks nothing in the tree today (nothing bakes one in a 2D scene) but makes
every 3D navmesh a special case. Making `Plane2D` authorable contradicts a stated refusal that
was argued rather than defaulted. A third answer — that both take an up axis and neither
hardcodes one — is more code than either and is probably the right one.

Expected to be wrong about: whether `Plane2D`'s refusal is load-bearing. It cites Jolt's own
constraint shape, and if Jolt's 2D freedom is genuinely XY-only then the decision is made and
this row is the navmesh's to move.

## Verification

- `./test.sh debug`, then `./test.sh asan`. Both `scene/NavMesh.cpp` and `scene/Physics.cpp`
  are in `SUBSTRATE_HOSTED_SOURCES`: bake a mesh in the chosen plane, path across it, and step
  a `Plane2D` body along the result with no device.
- `scripts/golden.sh` — eleven cases, byte-identical. No golden case bakes a navmesh in
  anything but XZ, so a moved pixel means the 3D path changed.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the navigation and physics
sections, both of which state their plane, and
[limitations.md](../../architecture/limitations.md), which records neither as a limit.

## Outcome

**`Plane2D`'s refusal is not load-bearing on Jolt, and that is the first thing the row had to
settle.** `EAllowedDOFs` is a bitmask of six individual axes; `Plane2D` is the *name* Jolt gives
`TranslationX | TranslationY | RotationZ`, and `TranslationX | TranslationZ | RotationY` is
exactly as expressible. So the card's "if Jolt's 2D freedom is genuinely XY-only then the
decision is made" resolves the other way: nothing external decided it, and the refusal is this
engine's own — argued from gravity being -Y, the orthographic camera looking down -Z and a
sprite's layer being its depth. Three conventions already agree with `Plane2D`, and none agrees
with navigation's +Y. **So navigation is the side that moved**, and the refusal in
`limitations.md` is kept with that argument written into it.

**The third answer landed, in its cheap form.** The card called "both take an up axis" more
code than either alternative, and it would have been if the solver had been rewritten
axis-agnostically: the slope filter, the barycentric containment, the BVH bounds tests and the
funnel are all written in XZ. They are all unchanged. What `NavBuildParams::up` does is decide a
rotation at `bake`, apply it to the incoming triangles, and undo it on the way out of every
query — the solver never learns there was another frame.

Two things that had to be right and were not obvious:

- **A rotation, not an axis swap.** A permutation of the axes flips handedness, and the funnel
  reads a portal's left and right off a winding that only holds in a right-handed basis. The
  `path.size() == 2` assertion on an open XY plane is what would have caught it: a flipped
  funnel restarts at every portal and returns the corridor's own vertices, which still walks and
  still arrives.
- **Public wrappers over private `*Nav` bodies.** `corridorClear` asks `nearest` and `raycast`,
  and `findPath` asks `findCorridor` and `corridorClear` — every one of them public. Rotating
  inside the public methods alone would have rotated the same point two or three times, and the
  symptom would have been a path that works in 3D and wanders in 2D.

`+Y` performs no arithmetic rather than multiplying by an identity quaternion, so a 3D navmesh
is bit-for-bit the one it was — which is why the existing 33 navigation tests needed no changes.

**`PathFollower::up` is the half the card did not name.** `steer` takes no `NavMesh` and never
should, so the axis had to travel with the follower. Left at +Y against an XY path, the agent
measures its progress along the one axis it is not travelling on and stops at the first
waypoint's X — the control arm was run and the cross-subsystem test fails on it.

**Verification.** 995 tests, debug and ASan, three of them new: an XY floor baked against +Y is
a wall and bakes to nothing (which is what the tree did), the same floor with `up = +Z` bakes 32
triangles in one region and hands its vertices back in world space, and a `Plane2D` body walks a
path baked in its own plane to the goal without ever leaving it. `scripts/golden.sh` — 11 of 11,
byte-identical.
