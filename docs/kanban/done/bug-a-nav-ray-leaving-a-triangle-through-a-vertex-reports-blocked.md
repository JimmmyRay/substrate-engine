---
id: bug-a-nav-ray-leaving-a-triangle-through-a-vertex-reports-blocked
title: A nav ray leaving a triangle through a vertex reports blocked
arc: bug
size: S-M
verification: tests-4, inspection
---

# bug-a-nav-ray-leaving-a-triangle-through-a-vertex-reports-blocked — A nav ray leaving a triangle through a vertex reports blocked

`NavMesh::raycast` answers **blocked** for a ray that leaves a triangle exactly through a
vertex. The exit-edge search finds no edge because the separation test ties at zero, so a line
lying entirely on the mesh comes back as an obstruction.

`raycast` is **public API a game calls for line of sight**, which is what makes this worth a
card rather than a note. It is also what `corridorClear` is built on, and that is where it was
found: a straight path across an open plane was smoothed in Debug and not in Release, because
which of two comparisons rounded to exactly zero decided the answer. See
[bug-the-funnel-leaves-a-corner-in-a-path-across-an-open-xy-plane](../done/bug-the-funnel-leaves-a-corner-in-a-path-across-an-open-xy-plane.md),
whose corners-only pass removes that **symptom** for collinear waypoints and leaves this cause
untouched.

**The shape it bites is the one D18 just opened the door to.** A tilemap-shaped 2D world is
square cells on a grid, so any path running along a tile boundary — which is most of them —
travels through vertices the whole way. A 3D scene baked from arbitrary triangles hits it
rarely; a 2D game hits it as the normal case.

The fix is a decision about the degenerate case rather than a search for a defect: when the
separation test ties, the ray is passing through a shared vertex, and the question is which of
the two edges meeting there it should be treated as crossing. Either answer continues the walk;
answering "neither" is the bug. Whether an epsilon or an explicit vertex-fan step is the right
shape is the work.

**Provenance.** Established at `fb47938` while executing the funnel card. Not measured for cost:
`raycast` has no benchmark and no golden, and the unit suite is the only thing that reaches it.

## Verification

- `./test.sh debug` → `./test.sh release` → `./test.sh asan` → `./test.sh tsan`, each its own
  invocation. **A test that fails today**: a ray along a tile boundary of a grid world, asserted
  clear, in both XZ and XY. It must be red before the fix and green after, and the Debug/Release
  split above means the case has to be run in both — a case that ties at zero is precisely the
  one whose answer changes with optimisation level.
- `inspection`: nothing outside the unit suite covers `raycast`, and a nav ray has no golden
  image.

## Reference update

[systems.md](../../architecture/systems.md), the funnel subsection under navigation, which
currently states this limit as the reason the corners-only pass exists.

## Outcome

**Fixed by simulation of simplicity, not by an epsilon.** `raycastNav` now runs the walk as the
walk of the ray displaced infinitesimally off its own line, with the displacement direction
computed once from the start triangle and held for every step; a vertex with zero signed area
counts as being on that side. Two files: `engine/scene/NavMesh.cpp` (`raycastNav` only) and
`tests/NavMeshTests.cpp` (one helper, two tests).

An absolute epsilon was rejected because `triArea2` is an *area* — one epsilon is a different
tolerance at every world scale, and one loose enough to catch the tie also picks edges the ray
genuinely misses, which sends the walk into the wrong triangle and answers **clear across a
hole**. A vertex fan turned out to be unnecessary: under a consistent nudge the walk is that of a
line in general position, so every crossing after the first is strict, and the fan rotation the
degenerate case appears to demand is an artefact of nudging *inconsistently*.

**Which way to nudge is the whole of it, and getting it wrong is a silent regression.** Nudging
always to the same side — what the old `>= 0.0f` does — means a start triangle lying wholly on
the ray's left is never entered by the perturbed ray at all, and along a tile boundary exactly
one of the two triangles sharing it is that triangle. The rule `tieLeft = anyRight || !anyLeft`
nudges into the start triangle. A triangle with a vertex strictly on each side is entered either
way, so no non-degenerate case moves. The sign was inverted on the first attempt and the tree
caught it: `RaycastSeesAcrossAnOpenFloor`, whose (0.5,0.5)→(5.5,5.5) diagonal runs along the cell
diagonals, is the same degeneracy and is now the guard on the sign.

The other comparison, `triArea2(a, b, to) >= 0.0f`, is deliberately left exact. A tie there means
`to` is on the edge's line — either an edge collinear with the ray, whose endpoints are then both
on it so the separation test rejects it anyway, or `to` on the edge itself, which containment has
already answered. **Relaxing both, which is the obvious epsilon reading of this card, is what
would break the walk.**

Red first, as required. `--gtest_filter='NavMesh.ARayAlongATileBoundary*'` before the fix, in
Debug and Release: 2 tests, 0 passed, **11 individual failures** — `raycast` false from triangle
6 at (3, 0.5), from triangle 24 at (0.5, 3), from triangles 6/18/19 at (3, 1), plus two
`corridorClear` failures, and the XY case's equivalents from triangles 4, 36, 5 and 16. Green in
both after.

`tests-4`, four separate invocations: **1019 tests, 104 suites, PASSED** in debug, release, asan
and tsan (1017 before; the two new tests).

**The corners-only pass is not dead, and that was measured rather than assumed.** With the fix
in, the full unit suite records **zero** drops from it — so the suite alone would have licensed
deleting it. A sweep of 6852 paths over 400 pseudorandom worlds (4-12 cells square, open planes
and scattered-wall mazes, XZ and XY, radius 0 and 0.3, endpoints on cell centres, edge midpoints
and grid corners) says **274 paths still lose a waypoint to it, against 400 in a control with the
fix reverted**. The fix removes about a third of what it was cleaning up; the rest are genuine
multi-waypoint collapses on walled worlds — `9 → 7`, `6 → 4`, `5 → 3` — the same class as the
5.657 m / 4.243 m detour the funnel card cited. Left untouched.

**Three corrections to this card's text.** The Debug/Release split it asks the test to cover does
not apply: the new test is red *identically* in both, with the same triangle indices, because the
tie is between exactly-representable coordinates on an integer grid and is `0.0f` either way.
What varied between configs in the funnel card was which of the two boundary triangles `nearest`
returned, and the funnel's own zero-area comparison downstream — the new test asserts from
**both** triangles deliberately, so the property is checked rather than sampled. Both configs
were run before and after regardless. Second, "which of two comparisons rounded to exactly zero"
understates it: only one of the two ties here, and the other must stay exact. Third,
`NavMesh::raycast` and `corridorClear` have **no caller anywhere** in `engine/`, `game/` or
`tools/` outside `NavMesh.cpp`; `DemoGame.cpp` calls only `findPath`. The card's framing of
`raycast` as API a game calls for line of sight is a statement about intent, not current use —
which does not weaken the fix, since `corridorClear` is on the path-smoothing hot path, but it
does mean nothing outside this file would have noticed.

`inspection`, recorded as the card asked: nothing outside the unit suite reaches `raycast` or
`corridorClear`, no golden scene exercises a nav ray, and `scripts/golden.sh` has no navigation
case. `NavMesh` is in `SUBSTRATE_HOSTED_SOURCES`, so the unit suite is the whole of the coverage
and ASan and TSan do reach it.

Reference updated: `systems.md`'s funnel subsection now carries the resolution, the nudge-side
rule and its guard test, why the second comparison stays exact, and the sweep showing the
corners-only pass still earns its place.
