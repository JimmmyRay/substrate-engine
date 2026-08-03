---
id: C18
title: Brush geometry solved on the CPU
arc: C
size: L
verification: golden-11, tests-4
---

# C18 — Brush geometry solved on the CPU

**Weighed before it was started, and declined.** There is no `engine/csg/`. A blockout is
boxes, and this engine already draws, collides, unloads and simplifies boxes a game hands it.

## What the card used to say

> `engine/csg/` takes a list of half-space brushes and returns a `scene::MeshData`, which a
> game hands to `Engine::createMesh` exactly as `DemoGame.cpp` already does with
> `unitCube()`. [...] Hand-written plane clipping against glm, no new submodule.

Sized **L**, verified by twelve golden cases and four test configurations, with a stated
plan to take the module count in [`principles.md`](../../architecture/principles.md) and
[`architecture/README.md`](../../architecture/README.md) from four to five. None of that is
what landed. The card also carried two tokens that were already stale by the time it was
read: the golden suite is **eleven** cases, not twelve, and it cites
[G10](../done/G10-a-game-links-only-the-subsystems-it-names.md) as *"the leaf case of G10's
pattern"* — a pattern G10 declined to build.

## The reconsideration

Three questions decided it, in this order.

### 1. What does a blockout actually need a solver for?

The row's premise is that a blockout needs half-spaces clipped against each other. That was
true of the engine the technique comes from and it is not true of this one.

**Quake solved brushes on the CPU because a BSP tree, a PVS and a lightmap all consume the
*surfaces* of the union.** Interior faces had to be clipped away before any of the three
could be built, and a face that survived clipping was the unit the lightmapper packed and
the unit the visibility set referenced. This engine has none of those three. It has a depth
buffer, and two interpenetrating opaque solids drawn through a depth buffer show exactly the
boundary of their union — so **the union half of CSG is not a cheaper way to get the right
picture, it is a way to get the same picture with fewer triangles.**

That number has already been measured against this engine, on this arc, and it does not
bind. [`limitations.md`](../../architecture/limitations.md#lod-does-nothing-in-sponza-and-the-thresholds-margin-is-thin)
records C17's arm: **20% fewer triangles moves `GBuffer` 0.481 -> 0.474 ms and `Frame`
3.384 -> 3.339 ms**, on a scene of a hundred architectural draws. The room in
[`tests/BrushGeometryTests.cpp`](../../../tests/BrushGeometryTests.cpp) — floor, ceiling,
four walls, a window and a doorway — is **148 triangles**. A solver that halved it would be
saving seventy-four.

This is P8's lesson applied again, and it is worth stating as the general one:
**a row deferred to the end of an arc must have its justification re-read against what the
arc landed.** C17 measured the thing C18 was going to be worth.

### 2. What is left is the subtraction, and it is rectangle arithmetic

Strip the union and one case remains: an opening in a wall. Two things are true of it.

- **An opening that reaches an edge is not a subtraction at all.** A doorway is a left pier,
  a right pier and a lintel. That is what a mapper builds, it is three boxes, and
  `doorwayWall` in the test file is four lines.
- **An opening strictly inside a face is eight rectangles.** `windowWall` is a 3x3 grid with
  the middle cell missing, on both faces, with the four narrow sides split along the same
  rows and columns so no edge of one face ends in the middle of an edge of another, and four
  reveals lining the hole. Thirty-two quads, and it comes out a closed, consistently wound
  surface.

**The card sized itself L for numerical robustness, and that is exactly the cost the
arithmetic does not pay.** Its own words were *"three planes meeting at a near-degenerate
vertex, coplanar faces between adjacent brushes, and epsilon choice for point-on-plane are
the classic failures"*. All three are properties of a **clipper**, not of the geometry:

| The card's failure | Where it comes from | What the alternative does |
|---|---|---|
| A near-degenerate vertex | Three planes intersected numerically | Corners are the numbers the author typed. `EveryCoordinateIsOneTheAuthorWrote` checks every vertex component against the authored set with `memcmp` |
| Coplanar faces between brushes | Deciding whether two computed planes are the same plane | Nothing decides. Abutting brushes share the same literal, and overlapping ones are settled by the depth buffer |
| Epsilon for point-on-plane | The solver needs one | There is no comparison to make. The word `epsilon` does not appear in the alternative |
| T-junctions | A clip that splits one face and not its neighbour | The author splits both. Four extra quads, and `everyEdgeIsMatched` fails without them |

There **is** a tolerance question, and it belongs to the author rather than to the engine:
how close two brushes may be before they should have been one. That is a level-design
decision, and no value the engine picked would be right for a stone arcade and a
sixteen-pixel platformer at once.

### 3. If it were worth building, it would not be built here

§9 landed today with [D9](../done/D9-the-scene-baker-leaves-the-runtime.md): *the running
process never writes a file a later run reads as an input*, and its corollary is that
geometry production is offline and reproducible. A CSG solve at load does not violate the
letter of that rule — it writes no file — but it is on the wrong side of the line the rule
was drawn along. `substrate-bake` is where a mesh gets its LOD chain (C17), and a solver is
the same kind of thing: a build-time transform from an authored description to vertices.

So the offline shape is the only one worth arguing for, and pushing it there is what
exposes the real gap: **an offline tool needs an input format, and there is no brush asset
in this tree to read.** That is a refusal this repository has already made twice at smaller
scale, in the same words:

> **A sprite sheet *file* format** — *"Aseprite's JSON and TexturePacker's agree about
> almost nothing, neither is in the asset tree, and a parser for a format no asset uses is a
> parser nobody can test."*

> **A tilemap subsystem** (P8) — *"Tiled's TMX and LDtk's JSON agree about less than
> Aseprite and TexturePacker do, neither is in the asset tree, and an engine-invented third
> would be a fourth artefact for a bake pipeline held at three."*

Brushes are the stronger case of the same argument. Quake's `.map`, Radiant's variants and
TrenchBroom's dialects are a family rather than a format; none is in this tree; and a
`substrate_brush` extras block would be an engine-invented fourth artefact for a bake
pipeline the C arc is
[deliberately holding at three](../../architecture/limitations.md#the-third-bake-and-why-it-is-one-decision-rather-than-two).
And the tool that would consume such a file already exists outside this repository: Blender
and TrenchBroom both export glTF, which is the format the engine reads.

**The card refused the one thing that would have made a solver worth having.** Its own *What
this must not grow* section begins *"a brush editor, a gizmo, or any UI"* — and an editor is
where a CSG solver's value is. Nobody blocks out a level by typing plane equations into C++.
Without the editor, a game writing brushes in code is strictly worse off than a game writing
a `MeshData` in code, because it has to describe a box as six half-spaces and then wait for a
solver to turn them back into the eight corners it started from.

## What a game writes instead

Not a sketch. [`tests/BrushGeometryTests.cpp`](../../../tests/BrushGeometryTests.cpp) is the
whole recipe, compiled against the real headers and run in the suite under every sanitizer —
because a refusal whose replacement is a code fragment in a document is a refusal nobody can
check. Ten cases, and none of them is engine code.

```cpp
// A blockout is brushes, and a brush is a pair of corners. Seven boxes and one grid make
// the room; `appendBrush` is a concatenation and an index rebase, not a boolean.
MeshData room;
for (const Box& b : shellBrushes()) appendBrush(room, boxBrush(b));
appendBrush(room, windowWall(northWall(), 3.0f, 5.0f, 1.0f, 2.0f));
for (const Box& b : doorwayWall(southWall(), 3.0f, 5.0f, 2.0f)) appendBrush(room, boxBrush(b));
// const auto blockout = e.createMesh(std::move(room));
```

| The brush wants | What answers it | Row |
|---|---|---|
| A solid, as geometry | A `MeshData`, handed to `Engine::createMesh` | G4 |
| A union of solids | Concatenation, or nothing at all — the depth buffer draws the union | G4 |
| A subtraction | Eight rectangles, or three brushes instead of one | — |
| A face's texture coordinates | A world-space projection the game picks, four lines | — |
| The solid as collision | One `ColliderDesc` per brush, `ColliderShape::Box` | S4.2 |
| Unloading a blockout | `Engine::removeModel` | C10 |
| Coarser levels of it | `substrate-bake`, offline | C17 / D9 |

The fourth row is the card's own expected-wrong-estimate answered: *"a brush face has no
natural UV, and whether that is a projection axis per face or an authored value decides
whether this stays L."* It is a projection axis per face, it is four lines, and it is
**world-space** — which is why `AdjacentBrushesTextureContinuously` holds without anything
welding anything. It is also the clearest thing on the list that an engine could only have
got wrong for somebody: a side-scroller wants one axis, an interior wants three, a stylised
game wants the number the artist typed.

## What is genuinely given up

Stated plainly, because a refusal that only lists what it saves is a sales pitch.

- **A boolean between two solids that are not axis-aligned.** A rotated box cut out of a
  rotated box is not eight rectangles, and nothing above produces it. Blockout is
  axis-aligned by convention and by grid snap, so this is a real hole in a place nobody
  currently stands.
- **Interior faces are drawn and hidden.** Overdraw where brushes overlap, which is the
  triangles the union would have removed and the measurement above priced.
- **Coplanar overlapping faces can z-fight.** Two brushes whose faces are exactly coincident
  and facing the same way is an authoring mistake with a visible symptom, and no solver is
  needed to avoid it — offset one by a millimetre or overlap them properly.

## Verification

The line said `golden-12, tests-4`. **Twelve was already wrong** — the suite has been eleven
cases since `no-ibl` was retired — and it is `golden-11` here.

**Not reduced, unlike P8's.** Nothing under `engine/` changed, so `golden-11` is a
neutrality claim rather than a feature check; it is kept precisely because `CMakeLists.txt`
did change and a build that adds a translation unit to the test target must move no pixel.
`tests-4` is kept rather than dropping to `tests-hosted`, because the new file is hosted and
therefore runs in all four.

- `scripts/golden.sh check release` — eleven cases, byte-identical.
- `./test.sh debug`, `release`, `asan`, `tsan` — each its own invocation.
- **Both arms**, as [`principles.md`](../../architecture/principles.md#how-a-claim-gets-made)
  requires of any check that matters. Two deliberate mismatches were planted and both were
  caught before the file was kept:
  - the window wall's narrow sides left unsplit, which is a T-junction —
    `everyEdgeIsMatched` went false on the wall and on the whole room.
  - a hole coordinate nudged by one part in ten million, which is what a plane intersection
    produces where an author writes a literal — `EveryCoordinateIsOneTheAuthorWrote` reported
    `x 3.0000004768371582 was computed rather than written`.

The `nm -C --undefined-only` check the card named for the leaf property has nothing to check:
there is no `csg::`.

## Reference update

[`architecture/limitations.md`](../../architecture/limitations.md) — *CSG brush geometry*, in
the runtime arc's declined table, with both triggers.

**No module count moves.** `principles.md` and `architecture/README.md` still say four, which
is the outcome of this row rather than an omission from it: C18 was the one row on the board
that would have made it five, and the count is load-bearing enough to be worth recording that
it was tested and held.

## Outcome

**The row was weighed before it was started and declined, and that closed it.** No engine
code was written, no fifth module exists, and the module count in `principles.md` and
`architecture/README.md` is still four. What landed is a recorded refusal with two named
triggers and ten hosted test cases that hold the alternative to it honest.

**What decided it was a measurement that already existed and had not been read against this
row.** The card's case for a solver is that a union of brushes is what gets drawn; the
engine's answer to "what gets drawn" is a depth buffer, so the union buys triangles and
nothing else, and C17 had already measured triangles against this renderer and found 20%
worth 0.045 ms of a 3.4 ms frame. The room a game writes instead is 148 triangles. That is
the same shape as P8's close — an obsolete premise found by checking the argument against the
tree rather than against itself — and it is the second time on this board that the rows in
front of a deferred one turned out to have answered it.

**The estimate was wrong in the direction the card could not see.** It said **L**, 800–2000
lines, and named numerical robustness as where the lines would go. The delivered work is
647 lines and every one of them is a test. The robustness cost was real and it was
**created by the solver**: three planes intersected numerically produce a vertex nobody wrote,
and every classic failure the card listed follows from that one fact. Vertices an author
typed have no tolerance to choose, and the two planted-mismatch runs are what turned that from
a claim into a check — a single-ULP nudge of one coordinate is caught by `memcmp`, and it is
exactly the magnitude a clipper's arithmetic produces.

**Two things were weighed and did not survive.** An in-tree solver, because the engine has no
BSP, no PVS and no lightmap and therefore no consumer for clipped surfaces; and an offline
one in `tools/` beside `substrate-bake`, which is the right *shape* under §9 and fails on the
same argument P8 and the sprite-sheet row already made — the `.map` family agrees about less
than the two sprite-sheet formats do, none of it is in the asset tree, and an engine-invented
brush format would be a fourth artefact for a bake pipeline deliberately held at three.

**One thing was found that is not this row's and is recorded rather than fixed.** The card
justified its leaf property as *"the leaf case of G10's pattern"*, and G10 closed by
**declining** that pattern. The conclusion survives the citation — a module nothing in
`engine/` names is free without any mechanism, which is *why* G10 declined the mechanism — but
a card citing a card that no longer says what it cites is the failure mode the board's own
README warns about, and it was written down before it was used.

**Verification.** `scripts/golden.sh check release` — **11 of 11**, byte-identical.
`./test.sh debug`, `release`, `asan`, `tsan` — **869 of 869** each, one invocation apiece, ten
of them new. Nothing under `engine/` was touched, so the validation run, the readback set and
the trace have nothing to say about this row, and the card says so rather than listing checks
that would have compared a build against itself.
