---
id: C19
title: Cloth that hangs from where it is pinned
arc: C
size: L
verification: golden-11, tests-4, validation, trace
---

# C19 — Cloth that hangs from where it is pinned

A mesh whose name starts `FABRIC_` becomes a Jolt soft body at load, its per-vertex
`_PIN_WEIGHT` attribute becoming the inverse masses that hold it to whatever it hangs from. It
steps inside `PhysicsWorld::step`, so it collides with every body the scene already has, and its
solved vertices land in the buffer `skinning.comp` writes — which means it draws, casts and
reflects through the thirteen passes that already read that buffer, with no new pass and no new
binding rule.

## Why

The engine's only cloth is a lie, and the card that shipped it says so. `bannerCloth()` in
[`DemoWorld.cpp`](../../../game/demo/DemoWorld.cpp) came in with
[G11](../done/G11-a-mesh-made-in-code-can-carry-morph-targets.md): two morph targets a quarter
period apart, driven in quadrature, with pinning expressed as a `v` multiplier baked into the
delta — *"`v` scales both, which is what pins the cloth to its bar."* It is the right exemplar for
morph targets and it is not cloth. Nothing in the tree simulates a soft body, and a banner, a
curtain, a cape and a flag are the four things a level artist reaches for first.

The prior art is one directory over and worth reading before starting. `~/Projects/Tethered` —
this engine's clean-slate predecessor, last commit seventeen days before Substrate's first —
shipped a working fabric pipeline: a `FABRIC_` prefix, a `_PIN_WEIGHT` custom attribute, and a
seven-operation Verlet/PBD compute shader. It is proof the authoring convention survives Blender's
stock exporter against a real asset, and it is also a list of the ways this decays. Its
`isFabricMesh` predicate exists **four** times — three translation units plus a private copy in
the tests, so the tests test their own copy. Its constraint build runs on the CPU at every load
and is never cached. Several hundred lines of shadow-volume cloth code have no callers anywhere.
And the convention it depends on is documented in exactly zero places: its own physics guide says
"special mesh naming" and never names either string.

## The solver decision, taken rather than defaulted

**Jolt soft bodies, not the compute shader ported across.** Every `Physics/SoftBody/*` source is
already in this binary: [`Jolt.cmake:395-406`](../../../external/JoltPhysics/Jolt/Jolt.cmake#L395)
lists twelve of them unconditionally, with no option gating them, and `RegisterTypes()` — which
`PhysicsWorld::init` already calls — registers `SoftBodyShape`. The solver is linked, tested
upstream and costs nothing to reach.

Three things it gives that Tethered's did not:

- **`CreateConstraints()` builds edge, shear and bend constraints.** Tethered generated structural
  edges only, so its cloth resisted stretching and nothing else.
- **Collision against the whole world.** Tethered fed its solver one capsule, from the player, over
  the event bus. A soft body in the physics system collides with every collider
  [`Collider.h`](../../../engine/scene/Collider.h) can author.
- **It is hosted.** No Vulkan, no window, so `engine/scene/Cloth.{h,cpp}` joins
  `SUBSTRATE_HOSTED_SOURCES` and the solver runs under all four sanitizers. That is the strongest
  verification this board has, and a GPU solver forfeits it entirely.

`JPH::SoftBodySharedSettings::Vertex` carries `mPosition`, `mVelocity` and `mInvMass`, with zero
meaning pinned — the same representation the attribute maps onto, so nothing is being adapted.

The cost taken knowingly is a CPU solve and a per-frame upload where Tethered had neither. That is
a follow-on row if the trace says so, and *the trace* is what should say so — see the obstacle
below.

## The prefix runs against the grain, and that is the decision

Substrate has four authoring schemas and all four are glTF `extras`: `substrate_emitter`,
`substrate_collider`, `substrate_sound`, `substrate_light`, read by the one rapidjson pass
[C14](../done/C14-one-document-scan-and-a-package-that-bakes.md) introduced and tabulated in
[`systems.md`](../../architecture/systems.md). **Nothing in the loader reads a mesh name today** —
`GltfScene.cpp` touches `.name` twice, for a collider and for an animation clip, and neither
`Primitive` nor `SceneNode` has a name field at all. A name prefix is therefore the first
name-driven dispatch in the tree, and pretending otherwise is how a convention becomes folklore.

Taken anyway, for a reason that is about where the data already is. `asset.meshes[meshIdx].name` is
in hand at the top of the geometry loop in
[`GltfScene.cpp`](../../../engine/scene/GltfScene.cpp), so the check is one `compare` inside a loop
that is already running. The rapidjson `GltfDocument` in [`Json.h`](../../../engine/core/Json.h)
exposes only `nodes`; a `substrate_cloth` extras key would mean growing it a mesh table, a second
walk, and a per-node-to-per-mesh resolution that the four existing schemas do not need because they
are genuinely node properties. Cloth is a property of the geometry.

**One predicate, in one function, and the tests call that one.** This is the single thing to copy
from Tethered by not copying it.

## The shape

Per-vertex data goes in a parallel array, and [`SceneTypes.h`](../../../engine/scene/SceneTypes.h)
already argues the case for `SkinVertex`: Sponza has 150k vertices and no skin, so widening
`Vertex` by anything to describe an unused feature costs megabytes to say nothing. So:

- `scene::ClothVertex { float invMass; }` on `SceneData`, one array along from `skinVertices` and
  `morphDeltas`.
- `Primitive::clothOffset`, `UINT32_MAX` for none — the `skinOffset` spelling rather than
  `morphOffset`'s offset-plus-count, because there is no count here to serve as the flag.
- `invMass = (pinWeight >= 0.999f) ? 0.0f : 1.0f - pinWeight`, which is Tethered's mapping. A
  partial pin is a heavy-but-mobile vertex. Worth saying that **Tethered never exercised it** — its
  shipping asset's weights were strictly `{0.0, 1.0}` — so the unit tests are the only thing that
  will ever cover the fractional path, and they have to.

**The sidecar cost is not optional.** A new POD written to `<name>.gltf.scene` means a
`static_assert(sizeof(ClothVertex) == 4)` beside the ten in
[`SceneData.cpp`](../../../engine/scene/SceneData.cpp), a term in `layoutDigest()`, a writer and a
reader in the adjacent-function style, a round-trip case in
[`SceneDataTests.cpp`](../../../tests/SceneDataTests.cpp), and `kSceneCacheVersion` **3 → 4** in
[`SceneData.h`](../../../engine/scene/SceneData.h). Every existing sidecar is invalidated by that
and the next load is slow. **That is the cache working**, and it belongs on this card so the
one-off reload is not read as a regression by whoever runs
[C13](../done/C13-a-measured-load-time-baseline.md)'s baseline next.

The solver is `engine/scene/Cloth.{h,cpp}`, with Jolt confined to the `.cpp` exactly as
[`Physics.h`](../../../engine/scene/Physics.h) confines it — that header deliberately passes body
ids as bare `uint32_t` "so this header needs no Jolt", and a second header reaching into
`external/` would undo the property. It steps from *inside* `PhysicsWorld::step`, which
`Engine::simulate` already calls: cloth colliding with the world means being in the world's step,
not beside it.

**Solve in world space; the instance transform stays identity.** A soft body has no rigid transform
to push down a node hierarchy, so the load-time transform is baked into the vertices and the node's
transform is ignored thereafter. Tethered did this and it is right, but it is a real restriction —
a `FABRIC_` mesh cannot be moved by animating its parent — and it is a decision rather than a
detail.

## Why there is no new render pass

[`Renderer::drawSceneIndirect`](../../../engine/gfx/Renderer.cpp) already switches the bound vertex
buffer between the scene's and `skinnedVertices` per draw group, and `buildSceneAccelStruct`
already refits a dynamic BLAS per deformed instance over that same buffer. Everything cloth needs
downstream exists:

- `kInstanceCloth = 1u << 7` in [`InstanceTable.h`](../../../engine/scene/InstanceTable.h), OR'd
  into `kInstanceDeformed` beside `kInstanceSkinned` and `kInstanceMorphed`. That one line gives a
  cloth instance a `skinDestBase` range from `setAnimator`'s allocation loop and carries it through
  every geometry pass, the shadow cascades and the ray-traced BLAS with no new binding rule.
- `skinnedVertices` gains `VK_BUFFER_USAGE_TRANSFER_DST_BIT`. A per-frame host-visible staging
  buffer takes the solved vertices and one `vkCmdCopyBuffer` lands them in the cloth range, before
  the compute→vertex-input barrier `recordSkinning` already emits. The mapped-per-frame,
  one-per-frame-in-flight pattern is the one debug lines, sprites and particle spawns all use —
  **not** `gfx::Uploader`, whose header says every submit blocks and it is "fine for load time,
  never for the frame loop".

Two properties are inherited rather than chosen, and
[`limitations.md`](../../architecture/limitations.md) already records them for deformed instances:
an infinite bounds box means **no frustum culling and no LOD**. Cloth gets both restrictions. That
is a fact for this card, not a defect to fix inside it.

Adjacent and deliberately not taken: the solver computes a real AABB every step — the verification
below depends on it — so cloth is the first deformed instance that *could* have finite bounds and
be culled. Doing that means deciding what a skinned character's bounds are too, which is a
different problem with a different cost. Different card.

## The failure mode this row will actually ship, and the check that catches it

**Cloth that over-reacts.** Not cloth that fails to move and not cloth that explodes — fabric that
bounces, twitches and swings far more than it should, forever. It is the characteristic defect of
every cloth implementation and it is what this row should expect to get wrong. Its causes are all
energy the integrator adds and the constraints do not remove: a timestep too large for the
constraint stiffness, too few solver iterations so corrections never converge, damping applied to
the wrong quantity, and collision response that pushes a vertex out further than it came in.

**Tethered is the evidence, and it lost this fight in four places.** Its solver carries four
separate hard-coded constants, each added against the same symptom, none of them tunable and none
of them tested:

- a velocity threshold below which a vertex freezes entirely — *"this prevents slow accumulation
  that causes periodic bounces"*;
- a collision margin *"to prevent jitter"*;
- a minimum constraint correction, skipped *"to prevent jitter"*;
- `stiffness = 0.8`, to *"reduce overshooting in the Jacobi solver"*.

The first of those has a bug that follows directly from being a hack: a cloth starting *exactly* at
rest returns before gravity is applied and never begins to fall. Four constants, one real bug, and
nothing in that repository would have failed if any of them regressed — its cloth tests exercise a
hand-written CPU reimplementation of the maths, not the shader that ships.

**So the check is a bounding box, and it is worth more here than an image.** The cloth's AABB over
a run of steps is exactly the quantity that distinguishes fabric from a trampoline, it is cheap,
it is deterministic, and it is hosted — no device, no pixels, no reference to re-snap. Two
properties, both asserted in `tests/ClothTests.cpp`:

- **Envelope.** From its rest pose, a pinned cloth's AABB never leaves a stated box around that
  pose across several hundred steps. A curtain pinned along its top edge cannot swing above its
  pins, cannot travel further from the wall than its own height, and cannot reach the floor.
- **Convergence.** The maximum per-vertex displacement per step decays monotonically enough to fall
  below a threshold within a stated number of steps, and stays there. That is what says the cloth
  *settles* rather than oscillating in a bounded box forever — the envelope alone would pass a
  permanent twitch.

Both numbers go on this card when they are measured, because a threshold nobody wrote down is a
threshold that gets widened the first time it fails.

Jolt's advantage here is not incidental: over-reactivity is what its own substep and iteration
counts exist to control, and they are settings rather than shader constants. If the envelope fails,
the fix is those numbers and the reason is recordable — which is the difference between this and
adding a fifth constant.

## The obstacle, and what I expect to be wrong about

**Normals, not the solve.** Jolt returns positions. Tethered spent two of its seven compute
operations recomputing normals, atomically accumulating face normals per vertex and normalising in
a second pass — on the GPU, where it was nearly free. Here it is a CPU area-weighted pass per cloth
per frame, plus tangents, over the same vertices the solver just touched. **I expect the normal
recompute to cost more than the constraint solve**, and if the trace says so then the honest
outcome is not to hide it but to record the vertex count at which a GPU solve wins and open that as
its own row. Sizing this L rather than M is mostly this.

**`appendModel` refuses deforming models**, and `clothOffset` makes that worse rather than better.
`GltfScene::appendModel` already rejects them because `skinOffset` and `morphOffset` index
scene-wide arrays it does not extend; `clothOffset` is a third. Cloth from an appended model does
not work, this row does not fix it, and saying so here is cheaper than finding it during
verification.

## What this must not grow

- Self-collision, wind, tearing, or a cloth *material*. It draws through the existing G-buffer
  pipelines or it is not this row.
- A second deformed-vertex buffer. It writes into `skinnedVertices` or it is a different design,
  and a different design is a different card.
- A `substrate_cloth` extras key. The prefix is the convention this row commits to; carrying both
  is how two vocabularies start disagreeing.
- A `FABRIC_` comparison anywhere but the one function — including in the tests.
- A per-cloth tuning surface beyond what Jolt's settings already expose. Stiffness as a hard-coded
  shader constant was Tethered's, and it is not worth reproducing as a hard-coded C++ one.
- **A velocity threshold that freezes a slow vertex.** It is the obvious fix for the symptom above,
  it is what Tethered reached for, and it buys a quiet cloth at the price of one that never starts
  falling. If the envelope fails, the substep and iteration counts are the answer.

## Verification

- `./test.sh debug`, then `asan`, `release`, `tsan` — each its own invocation. A new
  `tests/ClothTests.cpp`, hosted, covering: a pinned vertex never moves; an unpinned vertex under
  gravity falls; a fully pinned cloth is static across a hundred steps; a fractional `pinWeight`
  yields `0 < invMass < 1` and a vertex that moves less than a free one; the `_PIN_WEIGHT` parse
  against a fixture, including a `FABRIC_` mesh with the attribute absent; and the one `FABRIC_`
  predicate against the near-misses — `Fabric`, `fabric_`, `FAB_`, `FABRIC`, the empty string.
  `tests/SceneDataTests.cpp` gains the `clothVertices`/`clothOffset` round trip.
- **The bounds envelope and the convergence threshold**, in the same suite and by the same
  invocations — the two properties argued above, with the box and the step count written on this
  card as numbers rather than left to whatever the code happened to do. Two scenes: a curtain
  pinned along its top edge, and a flag pinned along one side, which swings further and is the one
  an envelope catches first.
- `scripts/golden.sh` — eleven cases, byte-identical. Nothing in `engine/assets/` carries a
  `FABRIC_` mesh, so movement in any of the eleven is a defect in the shared geometry or instance
  path rather than in cloth. **Cloth does not get a golden case, and does not need one**: a
  byte-identical reference asserts that a frame is unchanged, and the thing worth asserting about
  cloth is that it stays inside a box and settles — which is a bounds test, is cheaper, is hosted,
  and says something an image never could. The uncovered remainder is narrow and worth naming: the
  transfer into `skinnedVertices` and the draw out of it are checked by validation layers and by
  looking, not by the suite.
- Zero validation errors with layers on, in a capture with cloth in the scene. The new buffer usage
  flag and the copy-before-barrier ordering are precisely what layers catch and review does not.
- `scripts/baseline.py --config release --zones --runs 3`, several runs an arm, with and without
  the cloth scene. `Frame` is the zone to quote, per [`CLAUDE.md`](../../../CLAUDE.md); the CPU
  split between solve, normal recompute and upload is the number this card exists to produce, and
  it goes in the Outcome.
- `scripts/make_test_scene.py` gains `build_cloth()` → `game/demo/assets/cloth.gltf`: a curtain
  pinned along its top edge and a flag pinned along one side, with `_PIN_WEIGHT` written straight
  into the JSON. **Demo tree, not engine tree**, following that file's own stated split — it is for
  looking at a feature by hand, and the golden set cannot pin it. This is also what makes the
  runtime testable with no Blender anywhere in the loop, which is the whole reason
  [chore-blender-authors-the-pins-the-engine-reads](../done/chore-blender-authors-the-pins-the-engine-reads.md)
  is a separate row rather than a dependency of this one.

## Reference update

[`systems.md`](../../architecture/systems.md) — a cloth section, and the name-prefix convention
recorded beside the four `extras` schemas as the deliberate exception it is, with the reason.

[`rendering.md`](../../architecture/rendering.md) — the deformed-vertex buffer has a third producer,
and one of them is a transfer rather than a dispatch.

[`limitations.md`](../../architecture/limitations.md) — cloth inherits no culling and no LOD; it has
no self-collision, no wind and no tearing; it is not appendable; and its node transform is ignored
after load.

## Outcome

**It landed whole.** A `FABRIC_` mesh becomes a Jolt soft body at load, collides with every
collider the scene authored, and draws, shadows and reflects out of the buffer
`skinning.comp` writes. `game/demo/assets/cloth.gltf` is a curtain that drapes over a crate
and a flag that folds; `tests/ClothTests.cpp` is 22 hosted cases including the envelope, the
convergence and a bitwise determinism arm; the eleven golden cases are byte-identical.

### Where the solve runs, and why it is not a choice this row had to make

**Inside `PhysicsWorld::step`, in Jolt's own `PhysicsSystem::Update`, with no stepping code of
its own.** `createCloth` builds a `SoftBodySharedSettings` and hands the body to the same
`BodyInterface` every crate goes through, so the solver that steps the world steps the cloth.
There is no cloth update call anywhere and no place one could go.

That answers §9 without straining: the rule forbids a *run* writing what a *later run* reads,
and nothing here is written to disk at all. The bake was considered and is not merely worse,
it is not the same feature — a baked cloth is a canned animation, and the entire argument for
Jolt over Tethered's ported compute shader was collision against a world that only exists at
runtime. Baking it would throw away the reason it was chosen.

**`Engine::simulate` is the wrong altitude and the card's phrasing was slightly off.** Cloth
does not run *beside* physics on the fixed clock; it runs *inside* physics, which is one level
further down and is what makes a curtain collide with a crate for free. What does sit in the
frame rather than the step is the **readback** -- `ClothSystem::update`, once per frame after
the step loop, because a frame runs zero to four steps, only the last pose is drawn, and the
normal recompute is the expensive half of reading one.

### Why `engine/scene/Cloth.{h,cpp}` is not the solver the card described

The card asked for the solver in its own translation unit with Jolt confined to the `.cpp`.
That is not what a soft body can be: creating one needs `PhysicsSystem` and `BodyInterface`,
both of which live inside `PhysicsWorld::Impl` in `Physics.cpp` **by design** -- that header
says in so many words that it passes body ids as bare `uint32_t` "so this header needs no
Jolt". A second file would have meant promoting `Impl` into a shared header, putting Jolt on
the include path of everything that touches a scene, or a type-erased hook. Neither is worth
a file boundary.

So the split moved to where it is real. `Cloth.{h,cpp}` holds everything about cloth that is
**not** the solver and all of it is arithmetic: the two convention strings and the pin
threshold, the `isFabricMesh` predicate, `clothInvMass`, the weld, the normal recompute and
`ClothSystem`'s per-frame bookkeeping. It names neither Jolt nor Vulkan. `PhysicsWorld` holds
the bodies. Both files are in `SUBSTRATE_HOSTED_SOURCES`, so the card's actual requirement --
*the solver runs under all four sanitizers* -- holds either way, and it does: 898 of 898 under
ASan and TSan.

### How the per-vertex data cannot go stale

G11 found `GltfScene::indexData()` stale because it was a snapshot of one array indexed by an
offset in another, and `createMesh` grew one of them. C17 answered the same hazard for LOD by
putting the chain *inline* on the record it describes. Per-vertex data cannot literally live
inline, so the answer here is one step over: **two representations, and the flat one is read
exactly once.**

- `SceneData::clothVertices` plus `Primitive::clothOffset` are the **file format** -- one more
  `podVector` in the sidecar beside `skinVertices` and `morphDeltas`, written by the bake and
  read by `readSceneCache`.
- `GltfScene::ClothSource` is the **runtime form**: one record per `FABRIC_` primitive
  carrying its own vertices, its own zero-based indices and its own inverse masses. Nothing in
  it indexes anything outside it.
- The loop in `GltfScene::upload` that builds the second from the first is **the only reader of
  either flat structure that ever runs**, and it runs in the same statement that adopts the
  scene. After it there is no pair of arrays to get out of step, because there is no first
  array left with a reader.

Two things close the remaining gaps. `appendModel` refuses a document carrying cloth --
`clothOffset` is now the third offset into a scene-wide array it does not extend, and the
refusal predicate names all three. And once the soft body exists, the authority for a vertex's
inverse mass is *Jolt's* vertex array; `ClothVertex` is never read again, so there is no copy
tracking a copy.

`Primitive` grew by four bytes and `SceneStats` by eight, so `kSceneCacheVersion` is **3 → 4**
and every existing `.scene` is invalidated. That is the cache working; the next load of each
parses the document, and `scripts/bake.sh` restores the fast path. It was verified during this
row that the two loads agree: golden is byte-identical with the showcase sidecar stale and
again after re-baking it.

### The determinism proof

Proved, not asserted, and in four arms in `tests/ClothTests.cpp`:

- **`TwoRunsOfOneSceneAgreeToTheBit`** -- two independently constructed `PhysicsWorld`s, the
  same construction order, 240 steps each, `memcmp` over the whole `Vertex` array. Equal.
- **`ASecondClothDoesNotChangeTheFirstsTrajectory`** -- the growable-container hazard as a
  test. A second soft body is created and `PhysicsWorld::clothes` grows; the first cloth's
  240-step trajectory is bit-identical to the same cloth alone. Nothing about the solve is
  keyed on a position in that array.
- **`ReadingThePoseDoesNotPerturbIt`** -- 180 steps read once against 180 steps read every
  step. Bit-identical. **This one caught a real defect** (below) rather than confirming a
  design.
- **`WeldingIsAFunctionOfTheFileAndNotOfTheHashTable`** -- the simulation mesh itself. The weld
  numbers particles in first-sight order and never iterates the hash map, so two welds of one
  input are equal element for element. An order-dependent simulation mesh would be an
  order-dependent solve.

Upstream of all four, the world's own three guarantees are unchanged and inherited: one job
system (`workerThreads = 0`), a fixed step, and bodies created in the order the scene declared.

### What it costs

`scripts/baseline.py --config release --zones --runs 3`, four runs an arm, on
`cloth.gltf` -- two 13x13 sheets, 338 vertices, 10 solver iterations:

| Zone | Falling | Settled |
|---|---|---|
| `simulate` (CPU; the solve) | **0.171 ms** | 0.002 ms |
| `Cloth` (CPU; readback, normals, tangents, bounds) | **0.009 ms** | 0.009 ms |
| `Renderer::record/Skinning` (CPU; memcpy + copy record) | 0.001 ms | 0.002 ms |
| `Skinning` (GPU; the `vkCmdCopyBuffer`) | 0.007 ms | 0.007 ms |
| `Lighting` (GPU) | 0.300-0.344 ms | 0.399-0.456 ms |
| `Frame` (GPU) | 0.925-1.323 ms | 1.082-1.159 ms |

**The card's central prediction about cost is inverted, and by a factor of nineteen.** It
expected the normal recompute to cost more than the constraint solve and sized itself L mostly
on that. Measured: the solve is 0.171 ms and the readback-plus-normals-plus-tangents is 0.009
ms. An area-weighted normal pass is one cross product and two adds per triangle over 338
vertices; a soft-body solve is ten Gauss-Seidel iterations over roughly 900 constraints. There
was never a contest. **The follow-on row the card wanted opened if the trace said so is
therefore not the GPU normal pass it imagined** -- if cloth ever needs to go to the GPU it is
the *solver* that goes, and the vertex count at which that wins is far higher than this,
because the CPU cost is linear in particles and this scene's 338 cost 0.17 ms.

The second number worth stating is the one nothing predicted: **a settled cloth costs
nothing.** Jolt puts a soft body to sleep, and `simulate` falls from 0.171 ms to 0.002 ms.
That is not a threshold this row added -- it is the solver's own sleep, it wakes on contact,
and it is the reason a level full of hanging banners is affordable at all.

`Lighting` and `Frame` are the zones CLAUDE.md says to quote and both are the cloth scene's
own; there is no before to compare them against, because no scene in the tree had cloth. The
before/after that matters is the eleven golden cases, which are byte-identical -- gating on
the attribute is what keeps that true, and it does.

### Ten findings

1. **A glTF vertex is not a simulation vertex, and without a weld cloth tears at every seam.**
   Exporters duplicate a vertex wherever a UV seam, a smoothing split or a material boundary
   needs two normals at one place; a soft body built from those has two unconnected particles
   at one point. Not on the card, and the feature does not work on a Blender export without it.
   `weldCloth` merges by quantised position -- a grid rather than a radius, so the answer
   cannot depend on which vertex was seen first -- keeps the *minimum* inverse mass so a seam
   with a pinned side stays pinned, and drops faces the weld made degenerate because Jolt
   asserts on those. The generated scene has no duplicates at all, which is exactly why it
   could not have caught this and why the weld is tested directly.
2. **Bend compliance zero is a rigid plate, and it produced the over-reaction this card
   predicted.** Jolt's `mBendCompliance` defaults to `FLT_MAX`, which *disables* bend
   constraints; passing 0 -- the obvious reading of "stiff everywhere" -- makes them infinitely
   stiff. The result was the card's stated failure mode exactly: the curtain hung in the right
   place and twitched two millimetres a step forever. **And raising the iteration count from 8
   to 30 made it worse**, which is the diagnostic: more iterations of an unsatisfiable system
   is more energy, not less. Limp fabric is both the correct model and the stable one.
3. **The envelope is enforced by the solver, not asserted after it.** Jolt's long-range
   attachment constraints with `ELRAType::GeodesicDistance` cap how far a vertex may get from
   its nearest pinned one *measured along the edges*, which is the same sentence
   `ClothTests.cpp` asserts. It is a constraint the solver satisfies alongside the others, not
   a clamp -- and it is the standard answer to the one artefact inextensible cloth keeps after
   everything else is right, which is that a chain of edge constraints propagates a correction
   one link per iteration and the far corner learns about its pin several frames late.
4. **The tangent recompute was path-dependent and the shading depended on the frame rate.**
   Gram-Schmidt against the *previous frame's* tangent is a fold over its own output: two runs
   reaching identical positions produced different tangents depending on how many times
   `update` had been called. `ReadingThePoseDoesNotPerturbIt` failed on it. The fix is a stored
   rest tangent per vertex, 16 bytes, and the answer is now a pure function of the positions.
5. **A partial pin weight makes a vertex heavy, not slow, and the card said the opposite.** The
   card asked for a test that a fractional weight "moves less than a free one". That is true of
   a particle falling alone and false of a particle in an inextensible sheet: a position-based
   constraint splits its correction in proportion to the two inverse masses, so a heavier
   vertex receives *less* of the pull back toward its neighbour and sags marginally further.
   Measured over 30 steps: 126 um against a free vertex's 94. The test now asserts what the
   weight actually controls -- at or above 0.999 nailed down, anywhere below mobile.
6. **`indexCopy` needed a third condition and the symptom was invisible in the raster.**
   `GltfScene` retains a CPU index copy "only for a scene that deforms", and the test named
   skins and morphs. Cloth is the third. Without it a `FABRIC_` instance reached
   `buildSceneAccelStruct` with no index array to rebase, fell back to the *static* tier, and
   traced its rest pose forever -- a curtain that drew correctly and reflected as a flat sheet
   in mid-air. The log line is what caught it: `0 refitted` where it should have been 2.
7. **The deformed vertex buffer only existed when there was a rig.** `setAnimator` gated
   everything -- the buffer, `skinDestBase`, both deformed command sweeps -- on
   `animator != nullptr`, because until now a skeleton was the only thing that deformed. A
   scene whose only moving geometry is a curtain has no rig. The gate is now `deforms()` and
   the animator is allowed to be null the whole way through the buffer's creation.
   `PhysicsWorld::empty()` had the identical problem one subsystem along: a cloth-only world
   reported itself empty and `step()` returned before the solver ran.
8. **The first test geometry asserted that nothing happened.** A curtain modelled *hanging* and
   pinned along its top edge is already in its equilibrium pose: every vertex moved a fraction
   of a millimetre, and the envelope passed by measuring stillness. A sheet modelled **flat**
   has to swing through ninety degrees, which is the motion an envelope is worth checking --
   and it is what an artist actually authors, since a sheet is easier to model and UV lying
   down. Both `ClothTests.cpp` and `cloth.gltf` build them flat, and `making-a-game.md` says so
   first.
9. **The demo scene demonstrated the solver and not the collision, twice over.** The crate was
   placed *near* the curtain rather than under its pin line -- but a sheet pinned along one edge
   comes to rest in the plane of that edge, so it missed the crate entirely and rendered as a
   perfectly flat rectangle. A scene that cannot tell a working collision from an absent one is
   worth as little as a test that cannot.
10. **`readback.sh`'s five sprite cases are red on an unmodified tree, and this row is not why.**
    The sprite is drawn one *virtual* pixel down and right of where the harness computes it
    should be -- three device pixels at 3x, so `actual(3,3)` holds what `expected(0,0)` holds --
    and every thin feature aliases against the shifted phase. `sheet-cell1`, `sheet-cell2`,
    `sprite-letterbox` and the lit silhouette are the same defect through the same pass.
    **Measured, not argued**: HEAD checked out into a detached worktree, built, and run,
    produces the *same five failures with the same numbers to the digit* -- 26820/27648 texels,
    mean delta 22.1285, max delta 255 at (3,3), and the identical `[6,12)-[150,141)` box for the
    lit case. The last card to run this suite reported 9 of 9, so the regression is somewhere in
    the seven commits since; it wants a bisect and a card, and it is neither cloth's nor this
    row's to fix.

    Two smaller things fell out of chasing it, both worth their own line. **`scripts/check_ascii.sh`
    passes or fails on the same bytes depending on the tree it is run in**: its final
    `grep -P '[^\x00-\x7F]'` reads stdin without `--binary-files=text`, so when some file in the
    walk makes grep decide the stream is binary it prints `binary file matches` to stderr, emits
    nothing to stdout, and the guard passes. Four committed headers carry U+00A7 and the guard
    lets them through in the main checkout and refuses them in a worktree. And **`readback.sh`
    aborts before its summary when a case's log is missing**, because `set -euo pipefail` kills
    the `sed | head` in the failure branch.

### The numbers, written down

The envelope, one sentence for both shapes: **a vertex is never above the pin line, and never
further from it than the fabric is long.** For a 2 m sheet pinned at y = 2, that is
`y <= 2.05`, `y >= -0.05`, and 2.05 m in the swing axis, with 5 cm -- 2.5% of the sheet -- of
slack for the solver's residual. Both shapes are swept for 400 steps and the union of the
per-step AABB is what is tested, because the thing an envelope catches is the *overshoot* and a
cloth that swings twice as far as it should looks identical at rest.

Convergence: **1e-4 m per step, met within 360 steps**, checked and then held for 240 more.
Measured rather than chosen -- the curtain reaches *exactly* zero at 300 steps (5 s), when Jolt
sleeps it, so 360 is that with a fifth again of margin and 1e-4 is a hundred times tighter than
the two millimetres a step finding 2 twitched at.

### Corrections to what this card assumed

- The geometry loop is in **`SceneParse.cpp`**, not `GltfScene.cpp`; D9 moved the CPU half out
  of the runtime. `asset.meshes[meshIdx].name` is in hand there exactly as the card said.
- **The `_PIN_WEIGHT` parse cannot be unit-tested and that is structural.** `SceneParse.cpp` is
  `SUBSTRATE_SCENE_PARSE_SOURCES`, which `substrate_tests` deliberately does not link -- the
  suite has no document to parse and would otherwise pull in fastgltf and stb for nothing. So
  the card's "the `_PIN_WEIGHT` parse against a fixture" is not available. What is tested
  instead is everything on both sides of it: the one predicate, the mapping including the
  fractional and NaN paths, and the two producers agreeing. The accessor read itself is covered
  by `check_pins.py` on the authoring side and by running the scene.
- **Size L was wrong; this is XL.** The diff is eleven engine files, two new ones, a generator,
  a test file and four documents. The weld, the two-representation design and the renderer's
  three structural gates were none of them on the card.
- The golden suite is **eleven** cases.

### Verification

- `scripts/golden.sh check release` -- **11 of 11 byte-identical.** Nothing in `engine/assets/`
  carries a `FABRIC_` mesh, and gating every cloth path on the attribute is what keeps that
  true. Run again after re-baking `showcase.gltf.scene`, since the version bump invalidated it:
  identical both ways, which is the sidecar's stated cache property holding.
- `./test.sh debug` -- **898 of 898.** `release` -- 898 of 898. `asan` -- 898 of 898. `tsan` --
  898 of 898. 22 of those are `ClothTests.cpp`, so the solver, the weld and the readback all run
  under both sanitizers.
- `tests/SceneDataTests.cpp` gains the `clothVertices` round trip and a non-default
  `clothOffset` on the primitive, and `TheWriterAndTheReaderAgreeOnTheLength` is what catches a
  field written and not read.
- `scripts/locomotion.sh release` -- **8 of 8 arms pass.**
- **Zero validation errors**, layers on, `cloth.gltf` driven for 300 frames in a debug build --
  which is the run that covers the new `TRANSFER_DST` usage and the copy-before-barrier
  ordering, the two things review does not catch.
- `python3 scripts/check_pins.py game/demo/assets/cloth.gltf` -- 2 cloth primitives, 0 errors,
  0 warnings, 13 of 169 vertices pinned on each. **This is the card's "two producers agreed"
  check**, which `chore-blender-authors-the-pins-the-engine-reads` could not run: the generator
  writes what the validator accepts and what the loader reads, and `ClothTests.cpp` asserts the
  engine's spelling of both strings and the threshold literally, so a change to either side
  without the other fails.
- `scripts/baseline.py --config release --zones --runs 3` -- the table above.
- **Looked at.** `cloth.gltf` at frame 360: the curtain has fallen through ninety degrees and
  drapes around the crate with the fold shaded correctly and shadowed onto the floor; the flag
  hangs with a fold in it. `Cloth: 2 soft bodies, 338 vertices` and
  `2 static geometries + 2 refitted` in the log.
- `scripts/readback.sh release` -- **5 of 9, plus the resize soak.** The five overlay cases are
  bit-identical and the four sprite cases and the lit silhouette fail. **They fail identically
  at an unmodified HEAD**, which is the tenth finding and is written up below.
