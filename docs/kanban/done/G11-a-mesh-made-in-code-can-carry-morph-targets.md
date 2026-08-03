---
id: G11
title: A mesh made in code can carry morph targets
arc: G
size: M
verification: golden-11, tests-4, validation
---

# G11 — A mesh made in code can carry morph targets

M

## Why

Split out of **G9**, whose content table asked for *"a morphed banner and a few hundred
instanced urns"*. The urns landed. The banner could not, and the reason is a gap rather
than an oversight:

- `scene::MeshData` — the input to `Engine::createMesh` — carries vertices, indices, a
  material, a transform, two bounds and two alpha flags. **There is no target array.**
  `Primitive::morphOffset` and `morphTargets` exist and `createMesh` leaves both at zero,
  so procedural geometry is morph-free by construction.
- Loading the banner from a file instead does not work either. `Engine::addModel` appends
  geometry, materials, textures and render instances and **hands nothing to
  `SceneAnimator`** — a morphed mesh is driven by a `weights` channel in the rig, and only
  `Engine::loadScene` moves a rig into the animator.
- Which leaves the primary scene file, and G9's whole argument is that content a game
  authors should not have to be smuggled through `GameSetup::scene`'s glTF.

So the one thing S2.1 built has exactly one door — a glTF loaded at startup — and a game
cannot make a flag wave, a face talk or a cloth breathe in code.

## Scope

Two halves, and the first is much the smaller:

1. **`MeshData` gains targets**, and `GltfScene::createMesh` writes them into the same delta
   array the loader fills, setting `morphOffset` and `morphTargets` on the primitive. The
   deformation dispatch already reads exactly that; nothing in a shader changes.
2. **A weight has to be drivable.** `InstanceDesc::morphWeightOffset` points into the
   *pose's* flat weight array, which the animator owns and sizes from a rig. A mesh with no
   rig has no slot in it. The decision this row makes is whether a code-made morph target
   gets a weight block from the animator (a "character" with no skeleton, which S2.1's own
   note says a morph-only mesh already is) or a small array of its own on the instance
   table. **Pick one and say why on the card**; two would be two things that mean the same
   thing.

Explicitly **not** in scope: making `addModel` wire a rig. That is a bigger row about what
an appended model contributes, and it is named as a limitation rather than solved here.

## Verification

- `scripts/golden.sh check release` — eleven cases byte-identical. `morph.gltf` is not a
  golden case, so the check here is that the loader path is unchanged; a moved pixel means
  the file path and the code path did not stay one path.
- **A test that a code-made morph target and a file-authored one produce the same
  deformation** for the same weights, in the hosted suite. This is the check the row turns
  on: `MeshData` is hosted, the delta packing is hosted, and comparing the two producers
  against each other is stronger than comparing either against a snapshot.
- At least four cases: zero weight is the base mesh, full weight is the target, a weight
  between them interpolates linearly, and two targets sum.
- A validation-layer run of a scene holding both, with zero errors.
- The demo grows the banner G9 could not build, driven from `fixedUpdate`.

## Reference

[architecture/systems.md § A mesh made in code can morph](../../architecture/systems.md),
[architecture/limitations.md § The game API](../../architecture/limitations.md#the-game-api),
[guides/making-a-game.md](../../guides/making-a-game.md).

## Outcome

**G9's framing held, and it was two gaps short.** Everything the Why claims is true of the
tree: `skinning.comp` computes morph-before-skin for any instance, `kInstanceMorphed`,
`MorphDelta`, `Primitive::morphOffset`, `InstanceDesc::morphWeightOffset` and
`DrawRange::morph*` all exist and are wired end to end, and `MeshData` really did carry no
targets. Nothing in a shader changed and nothing in the dispatch changed. But the card
scoped the work as *"targets, plus a decision about where a weight lives"*, and two more
things had to move before a single deformed pixel appeared:

- **`Renderer::setAnimator` is a load-time verb.** It sizes the deformed vertex buffer, the
  delta buffer, the weight region and one output range per deformed *instance* from the
  state as it stands when it is called, and `Engine::loadScene` was its only caller. A mesh
  created afterwards is not in `skinDestBase`, so the command build drops it: the banner
  would have been authored, uploaded, given a character — and drawn nothing at all, with no
  error anywhere. `Engine::createMesh` now calls it again, and **only for a mesh that
  deforms**, because it tears down and rebuilds both acceleration-structure tiers.
- **`GltfScene::indexData()` was a snapshot nothing kept in step, and it is the defect this
  row found.** `buildSceneAccelStruct` rebases a deformed primitive's indices onto the
  deformed vertex buffer *on the host*, reading `sceneIndices[firstIndex + k]` — and that
  vector is the copy the *loader* took. A morphed mesh made afterwards has a `firstIndex`
  past its end, so the build was handed whatever followed the vector in memory and the GPU
  hung on a structure built over nonsense: five seconds later, as `VK_ERROR_DEVICE_LOST` on
  an unrelated fence, with the validation layers on and silent. **This is exactly the shape
  G12 found in `PhysicsWorld::snapshot`** — two arrays laid out to match, one of them grown
  — one array along and with a worse failure mode. `createMesh` now writes the copy, and the
  invariant is stated positionally on `indexData()` so the next writer has something to
  satisfy rather than a habit to copy.

### The decision the card asked for, and why

**The animator owns the weights**, as a character with no skeleton — the first of the two
options, and the argument is that the second was never really available. Every consumer
already assumes it: the shader's `weightBase` is `weightOffset(character) +
morphWeightOffset`, `updateInstances` fills the weight region by walking characters, and
`GpuInstance::meta.w` is what an instance names. A small array on the instance table would
have been a second buffer, a second upload and a second addressing rule for one quantity.

What the card did not anticipate is that `create` cannot serve it. A character's weight
block is sized from `AnimationRig::bind.weights` — what the *file* declared — so in a scene
whose glTF has no morph target at all, which is every scene in this repository but
`morph.gltf`, every character gets a block of length zero. Worse, `update` rebuilds each
pose from the bind pose every step (so a clip driving rotation alone cannot accumulate
drift), which would have **resized a game's block out of existence one frame after it was
made**. So `createMorphed(targets)` is a second verb rather than an argument to the first:
it takes the block size from the mesh, and its weights are held beside the pose and written
back after sampling. `setMorphWeight` is the only writer; no clip and no state machine can
reach these targets, because nothing in the rig names them.

### What landed

`MeshData::morphTargets` (a vector per target, each exactly as long as the mesh, refused
rather than padded); the append and the index-copy maintenance in `GltfScene::createMesh`;
`SceneAnimator::createMorphed`/`setMorphWeight`/`morphWeight` with `createSlot` factored out
of `create` for the two of them; `Character::held`; the restore in `update` and the zero in
`destroy`; `Engine::createMesh` creating and attaching the character and re-calling
`setAnimator`, `Engine::morphCharacterOf`, `removeModel` retiring the character and
`applyPendingScene` retiring all of them; and in the demo, `bannerCloth` — a
double-sided 442-vertex hanging cloth with two standing waves a quarter period apart, driven
in quadrature from `stepDemoWorld` so their sum is a wave that *travels*, which is the one
thing a single target cannot fake.

The banner's placement moved twice, and for the reason G9 recorded about the braziers:
`Camera::frameBounds` stands a quarter of the longest axis back, so the mirror-image
position put a two-and-a-half metre sheet three metres in front of the lens.

### What the tests prove, and what they do not

Fifteen new hosted cases, and the split between them is the honest part.

**`CodeMadeMorphWeights` (8, `tests/AnimationTests.cpp`)** is the half the row turned on and
is proved outright: a rig declaring no weights still yields a block; zero targets is no
character; a weight survives twenty updates with a clip playing (the defect the design
exists to avoid); two morphed blocks tile the region without overlapping and neither base
moves when the second is created; a morphed block packs behind a skinned one without either
prefix sum drifting; a retired character keeps its base, goes to zero and stays unwritable
through its stale handle; a target past the block's end does not reach the neighbour's.

**`MorphAddressing` (7, `tests/InstanceTableTests.cpp`)** covers the card's four cases —
zero weight is the base mesh, full weight is the target, a weight between interpolates, two
targets sum — plus the comparison the card said the row turns on: a file-authored primitive
and a code-made one, same geometry and same targets, agree on the same weights and
**disagree the moment their weights differ**. That negative control is load-bearing: without
it every case passes for an implementation with one weight run pretending to be two.

**What none of it proves is the deformation.** The weighted sum is `skinning.comp` and
running it needs a device the unit suite does not have. `morphedPosition` in the test file is
a *transcription* of that loop, so what these cases pin is the addressing and the data model
— which is where the defect class lives, and is not the same claim. The arithmetic is
checked by the banner rendering: frames 60 and 105 of a `--locked` run are visibly different
shapes on the same geometry, and both were looked at.

The golden set is the weak half by construction and was expected to be: eleven cases, none
of which loads the demo's world, so byte-identical says the loader path did not move.

### Deferred, each with its trigger

- **`addModel` wiring a rig**, named out of scope by the card and now stated in
  `limitations.md`: a morphed mesh *appended from a file* is still not driven.
- **Reclaiming morph deltas.** They join placements and material slots as a third array that
  only grows, and for the same reason — an instance carries `morphOffset`. Trigger: a game
  that cycles morphed meshes during play.
- **The per-mesh `setAnimator` cost.** One banner is invisible; twenty morphed props at load
  pay twenty acceleration-structure rebuilds. Trigger: a stated count, or a morphed mesh
  created during play rather than in `init`.
- **Real world bounds for a deformed instance.** It gets an infinite culling box, so a
  morphed mesh is drawn from every view every frame and its declared bounds reach only the
  spatial index and the inspector. Trigger: a scene with many of them.
- **`applyPendingScene` re-initing the animator.** Found here, belongs to C10; a streamed
  scene animates against the first scene's rig. This row only stops the *characters it made*
  from being uploaded forever afterwards.

### Verification

- `scripts/golden.sh check release`: **11 of 11 byte-identical.**
- `scripts/readback.sh release`: **9 of 9 bit-identical**, plus the lit silhouette and the
  resize soak.
- `./test.sh debug`: **794 of 794.** `./test.sh asan`: **794 of 794.** Fifteen of those are
  this row's, and ASan is the arm that matters for it — the whole row is arrays sized from a
  count somebody else supplied.
- **Validation, 400 scripted steps of the demo scene, zero errors.**
  `--input-script 60:Player.Forward+,150:Player.Run+,240:Player.Run-,300:Player.Forward-,330:Player.Jump`,
  `--headless --locked --audio-null`, with the banner morphing throughout.
- `scripts/locomotion.sh release`: **3 of 3 arms**, path and numbers unchanged from G12 —
  `idle > walk > run > walk > idle > jump > fall > land > idle`, 8.40 m travelled, peak rise
  0.93 m. Worth running because this row restructured `stepDemoWorld`'s early return.
- The demo reports the banner itself: *"240 urns and a 2-target morphed banner"*, and
  `Deformation: 28816 vertices, 66 joints and 2 morph weights across 2 characters` is the
  line that says the second `setAnimator` sized what it was supposed to.
