---
id: C12
title: Navigation
arc: C
size: XL-L
verification: golden-12, tests-4, validation
---

# C12 — Navigation

Navmesh bake, pathfinding, steering -- all three, as the row named them. `scene::NavMesh`: a **triangle** navmesh (weld, slope filter, edge adjacency, region labelling) with A\* over the triangle graph, the funnel algorithm for string-pulling, and a visibility pass over a mesh walk that straightens what the funnel could not. `PathFollower` and `steer` are the third stage and are a struct and a free function, not an `Agent` class. ~~**Needs C9**~~ **It did not** -- the index is over instance bounds and a navmesh is over triangles; they share no data. Baked from the scene's *static mesh colliders*, which is the surface an agent can actually stand on. 30 hosted tests; the demo paths its character on `V`. **What it is not** is a voxel navmesh, and the two things that costs -- clearance and true radius erosion -- are written into the header rather than discovered later

## What C12 cost, and the one bug that would have shipped

**The funnel's left and right were backwards, and it still produced walkable paths.** That
is the finding worth recording. `triArea2`'s positive half-plane is the funnel's *right*,
not its left, so the geometrically intuitive assignment -- with normals up and a consistent
winding, the far vertex of a shared edge really is on the left -- is the wrong one. Getting
it backwards does not crash, does not leave the mesh, and does not fail any test that asks
whether an agent arrives: the funnel simply restarts at every portal and hands back the
corridor's own vertices. **An agent walks it perfectly well and the path is 30% too long.**

It was caught because the tests assert path *shape* rather than path *success* -- an open
plane must yield two points, and an L must yield three with the middle one on the inner
corner. A suite that only checked "does the agent get there" would have passed on the
broken version, and so would every eyeball on a demo.

Two smaller ones, both from the same test pass:

- **The slope filter was taking the absolute value of the normal's Y.** That accepts
  ceilings, and therefore bakes the underside of every floor in the scene as walkable. It
  is now signed, which is Recast's rule and costs only that a floor with reversed winding
  is silently not walkable -- the same trade every backface-culling renderer here already
  makes.
- **The path follower advanced on a radius test alone**, so an agent that overshot a
  waypoint walked back to it. This needs no teleport to happen: 5 m/s and one 400 ms frame
  is enough. It now also advances past any waypoint it is already beyond along the outgoing
  segment.

**And one premise that was wrong before the row started.** C12 was gated on C9. The two
share nothing: `SpatialIndex` indexes instance bounds and refits against an
`InstanceTable`, `NavMesh` indexes triangles that never move. C12 has its own BVH, and that
is the second in the tree rather than an abstraction waiting to happen -- the Rule of
Threes says two occurrences are a coincidence, and these two differ in what they index,
what invalidates them, and what they return.

**What is deliberately not covered.** Box, capsule and sphere colliders carry no triangles,
so a level whose floor is an authored box bakes no navmesh. That is a real authoring case
and it is the strongest argument for the voxel row that would follow this one -- a
rasterising bake takes every shape, and would bring clearance with it.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- The unit suite in four configurations, each its own invocation:
  `./test.sh debug`, `release`, `asan`, `tsan`.
- Zero validation errors with layers on, in every capture.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

Recorded above, under *What C12 cost, and the one bug that would have shipped*.
