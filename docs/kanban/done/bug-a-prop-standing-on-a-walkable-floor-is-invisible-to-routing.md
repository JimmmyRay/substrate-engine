---
id: bug-a-prop-standing-on-a-walkable-floor-is-invisible-to-routing
title: A prop standing on a walkable floor is invisible to routing
arc: bug
size: M
verification: golden, tests-4, validation
---

# bug-a-prop-standing-on-a-walkable-floor-is-invisible-to-routing — A prop standing on a walkable floor is invisible to routing

`NavMesh::bake` filtered the triangle soup by slope and kept what an agent could stand on. A
floor authored as one large quad with props resting on it passes that filter whole, so the
props contributed nothing but their tops and **every route across the floor came back as a
straight line through all of them**.

`game/battle_arena` is the caller that found it, and the cost is worth writing down because
nothing on screen showed it. The arena is a floor and a 7x4 grid of columns; the floor's
`.collider` is one 78 m quad, so the bake produced 850 triangles of which **two** were the
floor and 840 were 28 unreachable column tops ten metres up. `findPath` between any two
points on it returned two waypoints, the arena was solid, the fighters stood on it, and
`Nav: 850 triangles, 1812 vertices, 30 regions` looked exactly like success. `regionCount()`
does not warn either — a blind navmesh reports *more* regions, because every prop top is one.

**The evidence was already in the soup and the bake threw it away one test later.** A prop's
sides are too steep to walk; the slope filter drops them; they are the only thing that says
the prop is there. So the fix is where the discard is:

- Triangles are classified before the weld. A too-steep triangle that reaches above a
  walkable surface and touches or crosses it is *standing on* it, and where it meets that
  surface's plane is a trace in the floor's own 2D.
- Each walkable triangle is split by the traces that reach it — by the trace's line, but only
  through pieces the trace's own extent enters, which is what keeps the output at hundreds of
  convex pieces rather than the arrangement of every line against every other.
- A piece is dropped when the nearest surface above its centre faces up, which is what being
  inside a solid means. The nearest one rather than the parity of all of them: a centre that
  lands on the edge two triangles share is counted twice or not at all by a parity test.
- Splitting pieces independently leaves T-junctions, so a pass gives every polygon the
  corners its neighbours put on its edges before anything is triangulated.
- Pieces are fanned from a boundary corner rather than from the middle. A fan rooted inside a
  piece is a pinwheel: both ways round it look the same to a corridor search over centroids,
  and the funnel then pulls the path through the portals of the wrong half. Measured at 9.16 m
  against an 8.32 m optimum before the change, and exactly 8.32 m after.

**Standing on and above stay different questions**, which is what lets this need no agent
height. A bridge over a floor takes nothing from it, and neither does the underside of the
floor itself. `agentHeight` is still absent and still refused for the reason
[`NavMesh.h`](../../../engine/scene/NavMesh.h) always gave, and the hole is the footprint
rather than a quantisation of it — the pieces are convex and the splitting is arithmetic, so
there is still no cell size to tune.

**What this is not.** Not the voxel row. Clearance and true radius erosion are still absent
and still argued in the same file comment; this closes the case where a floor and the things
resting on it are both in the soup and only one of them was read.

## Verification

- `./test.sh release`, `./test.sh debug`, `./test.sh asan`, `./test.sh tsan` — 1081 tests, all
  passing in each. Five new `NavMesh` cases: a pillar is cut out and the floor stays one
  surface, the cut is the footprint to the centimetre rather than a quantisation, a tile
  wholly inside a pillar is not walkable, a pillar elsewhere does not bend a path across open
  floor, and a floor under a bridge is still walkable.
- `scripts/golden.sh check release` — 13 of 13, byte-identical. A navmesh draws nothing, and
  this says so rather than assuming it.
- `scripts/arena.sh release` — 8 of 8 arms. The probe route the length of a column row is 3
  waypoints where it was 2, and `game/battle_arena`'s enemy walks to the player without ever
  asking to move and standing still for more than five steps running.
- Zero validation errors over 60 frames of `./run.sh battle_arena debug --validation`, and
  zero warnings in the log.

## Reference update

[architecture/limitations.md](../../architecture/limitations.md) — a `## Navigation` section,
which it did not have: overhangs and clearance are still unmodelled, and the entry says what
that leaves and what is now covered.

[`engine/scene/NavMesh.h`](../../../engine/scene/NavMesh.h) — the file comment gains "What
stands on a floor is cut out of it", beside the two limits it already argues.

## Outcome

Landed with the arena's `arena.glb` byte-for-byte unchanged, which is the point: the first fix
attempted was a script that cut the column footprints out of the asset, and a game having to
pre-cut its collider to be navigable is the engine's defect wearing a disguise.

The estimate did not predict the two failures that cost the most, and neither was in the
cutting itself. Splitting each piece on its own shattered the arena's floor into **264
regions** — every T-junction is two triangles that visibly touch and are not adjacent, and the
weld cannot close one because both positions are correct. And the first triangulation fanned
each piece from its centre, which is a pinwheel a corridor search enters from the wrong side;
the arena still worked and the paths were quietly 10% long.

The bake costs what it now does: 12.6 ms of `battle_arena`'s 16.2 ms `NavMesh::bake`, once at
load, over 3508 collider triangles. Sponza authors no colliders and bakes no navmesh at all,
so nothing in the golden set pays it.
