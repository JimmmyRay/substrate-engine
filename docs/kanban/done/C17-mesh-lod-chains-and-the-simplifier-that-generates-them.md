---
id: C17
title: Mesh LOD chains and the simplifier that generates them
arc: C
size: L
verification: golden-11, validation, tests-hosted, trace
---

# C17 — Mesh LOD chains and the simplifier that generates them

Split out of [C11](C11-occlusion-culling.md), which shipped its occlusion
half and then sat in `blocked/` for months because this half shared its card. **The thirty
lines of screen-coverage selection in `cull.comp` are not the work.** They are the last of
it, and they cannot be written first: there is nothing to select between, so every branch
would resolve to LOD 0 and no test could tell a correct implementation from an empty one.

The work is the four things that have to exist before those thirty lines mean anything, and
the recheck that closed C11 confirmed all four are absent from the tree:

1. **A simplifier.** Either `meshoptimizer` as a submodule or an in-tree quadric edge
   collapse. The submodule is the obvious call — this is a solved problem and the
   conventions say to reach for a library that solves one — but it is a dependency decision
   and should be taken as one rather than by default.
2. **Somewhere to put the result.** LOD is **optional per mesh**: a mesh may carry a chain or
   it may not, and nothing requires one. That is the decision C11 took and it is the one that
   keeps this from touching every asset. It means a level count and a per-level index range
   on the mesh representation in `engine/scene/SceneTypes.h` and `engine/scene/GltfScene.h`,
   with the levels sharing the vertex buffer G4 made growable.
3. **Sidecar read and write**, in `engine/scene/SceneData.{h,cpp}`, with the format version
   bumped so C15's invalidation drops every stale cache rather than reading a chain out of
   bytes that do not hold one. Generating chains at load time is the thing to avoid: it is
   seconds of work per mesh, which is what the bake step exists to move offline.
4. **An asset that authors one.** Without it the selection path is a parameter that always
   resolves to LOD 0, which is precisely the state C11 refused to ship. Generating chains for
   a scene already in the tree during the bake is the cheapest way to get one, and it makes
   the check a comparison against that scene's own goldens rather than against a new
   reference nothing else uses.

**Then the selection.** Screen-space coverage of the instance bounds against a threshold, in
`cull.comp`, beside the frustum and Hi-Z tests that already run there. It shares the
per-command loop with both, so it is genuinely small once the data exists.

## The verification problem this row has and C11 did not

**C11's occlusion half could be exactly image-preserving; this cannot.** That was the whole
argument for its two-pass shape — nothing visible is ever dropped, so byte-identical goldens
remained the check. A lower-detail mesh drawn at distance *is* a different image by
construction, and no amount of care makes those bytes match.

So the check has to be stated rather than inherited, and the honest form is a threshold:

- **Eleven golden cases byte-identical at the reference cameras**, because the selection must
  resolve to LOD 0 at every distance those cameras stand at. A silhouette that changes in a
  golden shot has failed. This is a real check — it is what catches a coverage formula off by
  a factor, which is the likely bug — but it deliberately proves the feature does *nothing*
  in the reference set.
- **A second measurement the golden set cannot make**: the same scene from a camera far
  enough out to select a lower level, compared against the LOD-0 render of the same view,
  with a stated pixel-difference budget. Where that camera goes and what the budget is are
  decisions this row has to make and record, not inherit.
- `scripts/baseline.py` over several runs per arm, quoting `Lighting` and `Frame`. **This is
  the row where a triangle-count win should finally show up**, and if it does not, the same
  honesty C11 applied to its own null result applies here: Sponza is a hundred large draws,
  not the thousands of small ones the technique is for.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. Selection that changes a silhouette at
  a reference camera has failed, not passed.
- Zero validation errors with layers on, in every capture.
- `scripts/baseline.py` over several runs per arm; quote `Lighting` and `Frame`.
- A distant-camera comparison against the LOD-0 render of the same view, within a
  pixel-difference budget this row states before it starts.
- `./test.sh debug` and `./test.sh asan` — hosted tests for the chain data model and the
  coverage arithmetic, neither of which the golden set can see.

## Reference

`architecture/rendering.md` for the cull passes, `architecture/systems.md` for the bake,
`architecture/limitations.md` for what is not built.

## Outcome

### What landed

`external/meshoptimizer` at **v1.2**, as a submodule wired exactly as the other ten are —
`.gitmodules`, the `add_subdirectory` block with its options forced off, the CMake submodule
guard, the system-include list, and `build.sh`'s pre-configure check, which now names it
beside glfw and fastgltf. **The dependency decision was taken on `CLAUDE.md`'s own rule** —
*"reach for a third-party library that solves a solved problem; those are dependencies, not
architecture"* — and the argument is recorded in `principles.md` §6 rather than only here.
Mesh simplification is the definition of a solved problem, a simplifier takes an index array
and returns a shorter one so it is neither indirection nor over Vulkan, and reimplementing a
quadric collapse would have bought ownership of the bugs and nothing else.

The four preconditions, in the card's order:

1. `engine/scene/MeshLod.{h,cpp}` — hosted, so the bake needs no device and the arithmetic is
   reachable from the unit suite. `buildLodChains` simplifies each primitive up to three
   times, each level reducing the level above it rather than the original.
2. `Primitive` grew `LodRange lods[3]` and `lodCount`, and levels share the vertex buffer:
   `meshopt_simplify` returns indices into the original vertex array, so a chain costs
   indices and no vertices at all. **Zero levels is the default**, which is what "optional
   per mesh" comes to — nothing in a glTF authors a chain and nothing requires one.
3. The sidecar carries it for free, because the chain rides *inside* `Primitive` and
   `Primitive` is written by the same `podVector` that writes the range it extends.
   `kSceneCacheVersion` 2 → 3, and `sizeof(Primitive)` 60 → 88 moves the layout digest as
   well, so a stale sidecar is dropped twice over.
4. Sponza itself, at bake time: **51 of 103 primitives take a chain, 650,946 indices added
   (83% over the original), the sidecar 12.4 → 14.3 MB.** No new asset was generated —
   generating chains for a scene already in the tree is what makes the check a comparison
   against that scene's own goldens.

Then the selection: `screenCoverage` and `selectLod` in `cull.comp`, ~25 lines, beside the
frustum and Hi-Z tests. `--no-lod` / `render.meshLod` and `--lod-threshold` /
`render.lodThreshold` are the arms a measurement uses.

### How the per-level ranges cannot get out of step

This session fixed the same defect three times — `PhysicsWorld::snapshot`,
`GltfScene::indexData` under the BLAS build, and the joint packing C1 avoided by never
repacking. Every one was two arrays laid out to match, one of which grew. A per-level index
range is exactly that shape, so it was designed not to be one, in three places:

- **On the CPU there is no second array.** The chain is an inline fixed-size member of
  `Primitive`. It is therefore rebased by the same statement that rebases `firstIndex` in
  `appendModel`, serialised by the same `podVector` that serialises the primitive, and moved
  by the same `std::move` in `upload`. A per-primitive side array would have needed a second
  loop in each of those, and would have been wrong the first time one was forgotten.
- **`lodCount` counts the array it indexes**, not a total including LOD 0. There is no
  off-by-one available: `lods[l]` is valid for `l < lodCount` and that is the whole rule.
- **On the GPU there is no second array either.** The chain rides in `GpuCommandBounds`,
  which is written in the same statement as the indirect command, and **level 0 is copied out
  of that command rather than out of a second description of it**. So a merged instance run,
  a variant sweep reordering the list, or the static/deformed split cannot desynchronise
  anything — there is nothing laid out beside the command array to desynchronise. The
  coarser levels are read through `inst.meta.x`, the primitive index the instance already
  carried for the material lookup.

The build loop has the same hazard in miniature and refuses it the same way: `buildLodChains`
**copies** each primitive's index run into a local vector before appending to
`data.indices`, because a pointer into a vector that is about to grow is precisely the
mistake this row exists not to make.

### The budget and the camera, and why the first camera was thrown away

**Stated before the first run:** at most **1% of the frame** (14,400 of 1,440,000 pixels at
the suite's 1600x900) may exceed a per-channel difference of **8/255**, against the
`--no-lod` render of the identical view. One percent because at the coverage where a level is
selected the geometry occupies a small part of the frame, so an honest silhouette shift is a
thin outline while a chain naming a wrong index range replaces a mesh outright and misses by
an order of magnitude. Tolerance 8 rather than the golden suite's 2 because what is being
permitted is a *shading* difference from changed normals, not sampling noise.

The first camera — Sponza's own focus pulled back to 80 units and pitched down 25 degrees —
**passed at zero pixels and was rejected for it.** From outside, Sponza is a closed box: 19%
fewer triangles were drawn and not one of them was visible, because every chained mesh is
interior geometry behind a wall that wins the depth test. A check that cannot fail is worse
than no check, so it was tightened rather than kept.

**And then Sponza refused to provide a distant camera at all**, which is the finding rather
than an inconvenience. The atrium is thirty units long; the camera meets the end wall at
twelve, and every viewpoint outside the building sees the box above. There is nowhere to
stand that makes a chained mesh both visible and small. So the measurement moves the
*threshold* instead of the camera, which reaches the same coverage — a threshold of `T` at
the reference camera is the coverage the default threshold sees from `sqrt(T/T0)` times
further away — and the equivalence is stated rather than implied:

| threshold | equivalent distance | triangles drawn | pixels over tolerance 8 | budget |
|---|---|---|---|---|
| 0.002 | 2.9x | 239,724 (−4.2%) | 561 (0.039%) | pass |
| 0.02 | 9.1x | 200,848 (−19.7%) | 10,587 (0.735%) | pass |
| 0.2 | 28.6x | 163,542 (−34.6%) | 51,914 (3.61%) | **fail, and correctly** |

The third row is what gives the budget teeth: at 0.2 a mesh filling a fifth of the screen
drops levels, which is not a coverage any camera in this scene produces, and the budget says
so. The image at that threshold is still a correct, recognisably coarser Sponza — no holes,
no stray triangles, which is what `meshopt_SimplifyLockBorder` is there for: primitives here
have no neighbour information, so an unlocked border opens a crack between a wall and the
floor it stands on.

### The threshold was measured, not chosen, and the margin is thin

The first default was 1/512 — a principled 47x47-pixel footprint at 1080p — and **it failed
7 of 11 golden cases**: 1,155 of 1,440,000 pixels, max delta 143, all of them on the potted
plants at the far end of the atrium. That is the check working. Bisecting found the edge
between **0.0004 and 0.0005**: the smallest chained primitive covers about 0.00045 of the
viewport at the reference camera, roughly a 25x26-pixel footprint.

The shipped default is **1/4096 = 0.000244**, the largest round fraction under that edge — a
margin of **1.8x**, which is thinner than is comfortable to state and is stated anyway. It is
the honest consequence of a reference frame that contains objects at every scale at once, and
it is recorded in `limitations.md` rather than left in a constant. A floor helps and is not
enough on its own: `kLodMinIndices = 3000` keeps every mesh under a thousand triangles out of
the selection path entirely, which is why 52 of Sponza's 103 primitives can never select
anything.

### Was there a win? No, and here is the number

Release, 717 frames over 3 runs per arm, `scripts/baseline.py`:

| | LOD on | `--no-lod` |
|---|---|---|
| `Lighting` | 1.851 | 1.851 |
| `Frame` | **3.365** | **3.398** |

At the reference camera that is a comparison of the feature against itself, because the
threshold guarantees nothing selects a level there — which is the point of the golden check
and makes this pair a cost measurement rather than a benefit one. **The cost is not
measurable**: `Cull` 0.021 against 0.022 ms, `GBuffer` 0.482 against 0.481.

Forcing selection with `--lod-threshold 0.02` — 19.7% fewer triangles drawn, the equivalent
of standing nine times further away:

| | LOD on | `--no-lod` |
|---|---|---|
| `GBuffer` | 0.474 | 0.481 |
| `Lighting` | 1.834 | 1.850 |
| `Frame` | **3.339** | **3.384** |

**A fifth of the triangles removed buys 0.045 ms of a 3.38 ms frame, about 1.3%**, and
`GBuffer` — the only pass that draws geometry — moves 0.007 ms. Sponza's G-buffer is 262k
triangles on a 3060 Ti and is not triangle-bound; it is bound by the four multisampled
attachments it writes. **This is C11's result again and for C11's reason.** The feature is
correct, it is measured, and the scene does not exercise it. It is left on by default for
C11's reason too: with it on, every golden run re-proves that no reference camera selects a
level, and with it off the code rots.

The measurement needed something to measure, so `cull.comp` now accumulates a second counter
beside the visible-instance one: **triangles actually drawn, after selection**. The CPU's
`stats.triangles` is what the scene holds, and the whole point of a chain is that a draw
stops being that number — so `drawn` is on the overlay and in the frame log line, and it is
what every triangle count above comes from.

### One thing not explained

During the threshold investigation — the runs at 1/512, where selection was firing on
dozens of primitives at the reference camera — two golden runs died with
`VK_ERROR_DEVICE_LOST`, on a different case each time and both times *after* the case that
lost it had already reported a clean image. That is the exact signature C11 documents for a
missing barrier, so it was chased: standard validation is silent over 240 frames, sync
validation reports **18 hazards per frame, identical with `--no-lod`** and identical to the
count C11 established as this SDK's over-report, and a four-run 240-frame soak with selection
firing throughout is clean. It has not recurred in the roughly forty runs since, including
two full golden suites, the readback suite and twelve 240-frame baseline runs. **It is
recorded rather than explained.** If it returns, the thing to suspect is a barrier's access
mask, which is where C11 found its own.

### Deferred

- **A per-frame skinned bounds computation.** Skinned instances get no LOD, for the same
  reason they bypass frustum culling — a chain would index vertices `skinning.comp` was never
  asked to deform. Both wait on the same missing box. Trigger: anything that needs a skinned
  mesh culled.
- **Shadow cascades select LOD 0 always.** An orthographic view covering the world has no
  "fraction of the viewport" to test, and a caster dropping a level while the surface it
  shades keeps its own is a shadow that stops fitting what casts it. Trigger: a distance-based
  rule for cascades specifically, which is a different formula rather than this one applied
  twice.
- **Blended geometry gets none.** The forward pass builds its own commands on the CPU and
  never reaches `cull.comp`. Trigger: blended draws becoming a cost worth culling, which is
  the same trigger that would move that pass onto the GPU at all.
- **A scene that exercises any of this.** The honest gate on the whole row, shared with C11.
  Trigger: thousands of small distinct meshes.

### Verification

- `scripts/golden.sh check release` — **11 of 11 byte-identical**, run twice, with Sponza's
  sidecar baked so the chains are live. The run *before* the threshold was corrected failed 7
  of 11, which is the check doing its job.
- `scripts/readback.sh release` — **9 of 9 bit-identical, plus the lit silhouette.**
- `./test.sh debug` — **805 of 805**, including 11 new `MeshLod` cases.
- `./test.sh asan` — **805 of 805.**
- Distant-camera comparison — **561 of 1,440,000 pixels (0.039%) over tolerance 8 against
  a 1% budget** at the stated equivalent of 2.9x the reference distance; the table above for
  the sweep either side of it.
- Standard validation, 240 frames — **zero errors.** Sync validation, 18 hazards per frame,
  unchanged by `--no-lod` and unchanged from C11's documented count.
- `scripts/baseline.py`, 717 frames over 3 runs per arm — the two tables above.
