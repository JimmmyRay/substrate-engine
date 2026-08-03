---
id: bug-the-funnel-leaves-a-corner-in-a-path-across-an-open-xy-plane
title: The funnel leaves a corner in a path across an open XY plane
arc: bug
size: S
verification: tests-4, inspection
---

# bug-the-funnel-leaves-a-corner-in-a-path-across-an-open-xy-plane — The funnel leaves a corner in a path across an open XY plane

`NavMesh.APathAcrossAFlatWorldStaysInTheFlatWorld` is **red in the tree** and has been since
`81bcc1c`, "Let the navmesh be baked in the plane the bodies are actually in":

```
tests/NavMeshTests.cpp:617: Failure
Expected equality of these values:
  path.size()   Which is: 3
  2u            Which is: 2
```

The path across a 4x4 open plane baked with `p.up = {0, 0, 1}` comes back with **three**
waypoints where the equivalent XZ case straightens to two. The test's own comment says why that
matters: "the funnel is the part that would break if the rotation had flipped handedness and
left and right had swapped." So the assertion is not incidental tidiness — it is the check on
the rotation the commit introduced, and it is the one assertion in the file that reads it.

Everything else about the case passes. `nav.empty()` is false, `nearest` lands within 1e-4 on
all three components, every waypoint's `z` is 0, and the total path length is still
`sqrt(2) * 3` to within 0.01 — the path goes to the right place by the right distance, with a
corner in it. All 26 other `NavMesh` tests pass, including
`APlane2DBodyWalksAPathBakedInItsOwnPlane`, which is the row's stated claim.

**The two readings, and picking between them is the work.** Either the funnel's left/right test
is subtly wrong under the up-vector rotation and the extra waypoint is a real defect, in which
case a longer path in a more constrained XY world will show a worse one — or a straightened
path across an *open* plane is too strong a claim to make of the funnel in general, the XZ case
passes it by luck of vertex order, and the assertion should be `<= 3` with the length check
carrying the weight. **Do not weaken the assertion without establishing which.** It was written
as the handedness check and softening it silently removes the only thing watching that.

Worth a bisect against `81bcc1c` first: the same test at the prior commit says whether this is
a regression the commit introduced or an assertion the commit's new parameter first made
reachable.

**Provenance.** Confirmed red at `8bad63f` by
`./test.sh release -- --gtest_filter='NavMesh.*'` — 27 tests, 26 pass, this one fails. It was
found while executing
[chore-a-light-contributing-nothing-visible-still-pays-a-ray](../done/chore-a-light-contributing-nothing-visible-still-pays-a-ray.md),
whose full-suite run was `1015/1016`; that card touched nothing outside the lighting path, and
`NavMesh` is hosted, so the two are unrelated.

## Verification

- `./test.sh debug` → `./test.sh release` → `./test.sh asan` → `./test.sh tsan`, each its own
  invocation. All green — the point of the card is a red suite, so a green one is the whole
  contract.
- `inspection`: nothing outside the unit suite covers the funnel, and a navmesh path has no
  golden image. Record on the card what the extra waypoint actually was.

## Reference update

[systems.md](../../architecture/systems.md), wherever the navmesh bake and its up-vector are
described, if the funnel's guarantee turns out to be narrower than the test asserted.

## Outcome

**Reading two, with a root cause — and the card's premise was wrong in three places.**

There was nothing to bisect: `81bcc1c` introduced `NavBuildParams::up`, `flatWorldXY` **and this
test** in one commit, and `NavMesh.{h,cpp}` and `NavMeshTests.cpp` are byte-identical between it
and `HEAD`. The assertion was failing from the moment it was written.

**The equivalent XZ case does not straighten to two either.** The card said it did; measured, XZ
4x4 and XZ 12x12 both return the same collinear extra waypoint. The XZ test that passes,
`AnOpenPlaneGivesAStraightLine`, is a **6x6** world — a different world, passing for exactly the
luck the card suspected. XY 4x4: `(0.5, 0.5, 0)` → **`(2, 2, 0)`** → `(3.5, 3.5, 0)`. XZ 4x4:
`(0.5, 0, 0.5)` → **`(3, 0, 3)`** → `(3.5, 0, 3.5)`. Both extras are exactly collinear — walked
length 4.242640 against a straight line of 4.242640.

**And it was green in Debug, red only in Release.** Same funnel output and the same corridor in
both; what flipped was `corridorClear(start, goal)` — 1 in Debug, 0 in Release. The card
described it as unconditionally red.

The cause: the funnel emits a waypoint wherever the path runs exactly *through* a portal
endpoint, which a diagonal across square cells does at every corner, because a zero signed area
reads as the sight lines having crossed. Smoothing cannot take it back out — the segment runs
along the shared edges it is asking about, and `raycastNav` finds no exit edge for a ray leaving
through a vertex, so it reports blocked for a line lying entirely on the mesh. Which of the two
comparisons rounds to exactly zero is what decides whether the vertex survives, and that is the
Debug/Release split. **Nothing to do with handedness**: a mirrored funnel produces a *longer*
path, and these are exact to six decimals.

`findPathNav` now ends with a corners-only pass — drop a waypoint whose perpendicular distance
from the segment joining its kept neighbour to its successor is under 1e-4 m, measured in 3D so
a ramp's crest (collinear from above, a corner from the side) survives. Removing a point that
lies on the segment moves the polyline by at most the tolerance, far under any clearance
`agentRadius` bought, so it cannot shorten across a wall. It also repaired a genuine detour
found in a hand-built world: a path that backtracked to the origin for 5.657 m where the
straight line is 4.243 m now takes the straight line.

**The assertion was not weakened.** `EXPECT_EQ(path.size(), 2u)` stands and now passes for a
reason rather than by rounding.

**The handedness check the card was protecting did not exist.** `AnLShapedCorridorHugsItsInner
Corner` carried a comment calling itself "the test that proves the funnel's left and right are
not swapped"; swapping `l` and `r` in the portal construction — the exact mirror the code's own
comments warn about — left **all 1017 tests green**, because the smoothing pass reconstructs the
same waypoints from a mirrored funnel across a single turn. Two turns cannot be repaired that
way, so the new `AUShapedCorridorInXYTurnsInsideBothOfItsCorners` — a 7x7 U in XY asserting four
waypoints, both inner corners, flat z and walked < 16.5 — is the check: with `l`/`r` swapped it
is the **only** failing test in the suite, giving five waypoints out on the outer wall and
17.56 m walked against 16.045 m correct. The L's comment is corrected to say what it does check.
`floorOfXY` was added as `floorOf`'s cell-for-cell counterpart, and `flatWorldXY` delegates to
it.

Verification, four separate invocations on the final tree: `./test.sh debug`, `release`, `asan`,
`tsan` — **1017 tests, 104 suites, all four PASSED** (1016 before; the U test is the new one).
`inspection` recorded above: the extra waypoint's coordinates in both planes, and nothing outside
the unit suite reaches the funnel.

**Deferred, with a destination.** `raycastNav`'s degeneracy is untouched — a ray leaving a
triangle through a vertex still reports blocked, `NavMesh::raycast` is public API a game calls
for line of sight, and a tilemap-shaped 2D world runs along tile boundaries as its normal case.
Opened as
[bug-a-nav-ray-leaving-a-triangle-through-a-vertex-reports-blocked](../backlog/bug-a-nav-ray-leaving-a-triangle-through-a-vertex-reports-blocked.md).
Reference updated: `systems.md` gains "The funnel emits a point wherever the path runs through a
portal endpoint" under navigation.
