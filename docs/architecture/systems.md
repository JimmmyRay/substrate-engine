# Systems

Everything that is not the renderer: the scene representation, asset loading, animation,
particles, physics, audio, input and the interface.

---

## The simulation half

`engine/scene/Simulation.{h,cpp}` holds what a step moves — the sprite table, the animator,
the locomotion driver, the particle system, the physics world, the cloth, the audio engine,
the fixed clock, the scene tree and the body each sound rides — and `Simulation::step` is the
**only** place the order they move in is written. It names no Vulkan type, so it is in
`SUBSTRATE_HOSTED_SOURCES` and links into `substrate_tests` and `substrate-sim` with no driver
present.

`Engine` owns one and delegates: `Engine::simulate` is a line. **The encapsulation is not the
point** — a headless loop with its own copy of the call order is a loop that will disagree
with the drawn one on the frame it matters, and disagree quietly. One implementation, two
callers.

The order itself, and why each position is what it is, is in the source. Two of them are worth
repeating because they are easy to undo: **the locomotion driver runs after the physics step**,
because every parameter it writes is read back out of the solver and a `CharacterVirtual` that
has not been stepped reports itself in the air; and **every subsystem is called
unconditionally**, so an empty one costs a named zero in the profile rather than a missing
row — a guard at the call site makes a free system and an absent system look identical.

`Engine`'s members are references onto `sim`'s, declared at exactly the point the originals
were. That is not cosmetic: members are destroyed in reverse declaration order, cloth holds
bodies in the physics world, and every subsystem this absorbed was declared after that point.

What stayed in `Engine`: the camera, the input map, the scene loader, the spatial index and
the instance table. None is moved by a step. What did *not* move with it is the world **build**
— `initPhysics` walks colliders and binds nodes, sounds and rigs while it walks, and that walk
is still device-side; see [limitations.md](limitations.md#a-headless-loop-steps-a-world-it-did-not-build).

---

## The instance table

`engine/scene/InstanceTable.{h,cpp}` — the keystone of every dynamic-scene feature.

Structure-of-arrays over dense storage, stable slots with a free list, addressed
externally by an opaque `InstanceId` of index plus generation. **Slot stability is the
load-bearing property**: five consumers key off it.

The arrays are split **by consumer rather than by field**, and each is flat and uploads in
one copy:

| Array | Consumer | Size |
|---|---|---|
| `shading` (`GpuInstance`) | Every vertex shader, via `gl_InstanceIndex` | 128 bytes/slot |
| `bounds` (`GpuInstanceBounds`) | The cull dispatch | 32 bytes/slot |
| `prevTransforms` | The velocity pass | 64 bytes/slot |
| `ranges` | The command builder | CPU only |
| `generations` | Id validation | CPU only |

`GpuInstance` is exactly 128 bytes with a `static_assert` — a cache-line pair, keeping the
stride a power of two. The normal matrix is **stored rather than derived**:
`inverse(transpose(mat3(model)))` is about thirty flops, and paying that per *vertex* to
save 48 bytes per *object* is the wrong side of the trade.

That size is also why the previous-transform history is its own array rather than a second
`mat4` in the record: 64 more bytes on a record every vertex shader fetches, to carry a
value one shader reads.

### Adding a row from a game

`Engine::addInstance(model, material, transform, motion)` (G14) — one more copy of a mesh
the scene already holds, at a transform, with no second model and no second copy of the
geometry. It fills the seven fields out of the model's first `Primitive` and calls
`InstanceTable::create`.

**The verb is on `Engine` rather than beside `addPlacementInstances` because of what has to
happen afterwards, not because of what it copies.** `InstanceTable::create` cannot tell the
renderer anything, and two things then go wrong quietly: a slot past the renderer's instance
capacity is written past the end of a mapped staging range, and a *new* slot is invisible to
`staticTierStale` — which walks the slots the acceleration structure baked — so the instance
draws in every raster pass and appears in no ray. `Renderer::instancesGrew()` handles both,
and defers the rebuild to the next `rebuildAccelIfStale` so that a loop of `addInstance`
calls costs one rebuild rather than one each.

`motion` is a `scene::InstanceMotion` with **no default**, which is the one thing about the
signature worth arguing. It decides whether the instance writes a velocity for TAA and which
acceleration tier it lands in; wrong, it is a shadow that stays behind after the thing
casting it has been knocked over — a bug that reads as a renderer defect and is a call-site
typo.

### The upload

**The load-bearing decision is the upload, not the data model.** The buffer is
device-local with a staged copy, not host-visible. Host-visible reads as the obvious choice
— the table is written once per revision — but it is read once per **pass**, and there are
thirteen of them. Measured, that costs **0.431 → 0.567 ms** on the cascade pass alone and
turns a rock-steady zone into one varying 0.51–0.82.

The table is written once per revision per frame slot, not once per frame: a static scene
costs two memcpys at load and nothing afterwards.

**The history array is the exception and is not revision-gated.** It changes at the end of
every frame whether or not the table did, so a slot that skipped the copy would serve a
history from two frames before it last uploaded — which reads as a stationary object
smearing, intermittently, on whichever frame slot is stale.

`endFrame()` rolls transforms into the history and must run **before** simulation. Called
after — which reads more naturally — it copies the transforms that frame just wrote, so
every motion correction is zero and the velocity pass runs, costs its clear and its draws,
and reports that nothing moved.

### Flags

`kInstanceLive`, `kInstanceBlended`, `kInstanceDynamic`, `kInstanceVisible`,
`kInstanceSkinned`, `kInstanceMorphed`, `kInstanceMasked`. `kInstanceDeformed` is the union
of skinned and morphed — one mask rather than two tests at eight call sites, because the
causes are independent: a mesh may be skinned, morphed, or both.

`kInstanceVisible` is written by the cull dispatch into the **GPU's** copy and is never
read back. The CPU-side bit is clear for everything, always.

### Why not entt

**entt cannot own this table**, and that is the accurate reason rather than "no gameplay
components yet". `entt::basic_storage` pages its components (4096 by default), so the
dense array is chunked rather than flat and cannot be uploaded in one copy; and removal is
swap-and-pop, so dense indices are not stable — which is precisely what the five consumers
require.

entt therefore sits *beside* the table, an entity holding an `InstanceId` as a component,
which is what Unreal reaches with scene proxies and Bevy with render-world extraction. The
two stores have genuinely different access patterns: gameplay data is heterogeneous,
sparse and queried by combination; instance data is homogeneous, dense and uploaded
wholesale.

**Adopting it later is a CMake line and a registry object**, and all four enabling
properties now hold: (i) instances addressed by opaque id; (ii) creation is an explicit
`addSceneInstances(scene, table)` call rather than a side effect of `load()`; (iii)
transforms pushed via `setTransform(id, m)`; (iv) no gameplay-shaped fields in the record.

One temptation refused deliberately: the table's free list, generation counters and
id-to-slot map amount to roughly what `entt::sparse_set` already is. Taking the dependency
for that alone is the worst of both — ~120 lines of well-understood code against entt's
conceptual weight, which is the registry model rather than the sparse set.

---

## glTF loading

fastgltf, with a **targeted rapidjson pass over the same bytes** for the four `extras`
schemas. fastgltf reaches `extras` through a callback handing out a
`simdjson::dom::object`, which would put a header fastgltf downloads into its own `deps/`
at configure time onto this engine's include path. The rapidjson pass costs a few
milliseconds at load and keeps the schemas in a hosted translation unit the unit suite can
reach.

### The four `extras` schemas

| Key | Declares | Notes |
|---|---|---|
| `substrate_emitter` | A particle emitter | Nineteen fields of authoring data with no behaviour — a flat block is the right shape for a schema an artist writes in JSON |
| `substrate_collider` | A rigid body or character | Box, sphere, capsule, cylinder, hull, mesh, or `auto`, resolved by motion in exactly one place |
| `substrate_sound` | An audio source | Bus, gain, attenuation, stream/decode, occlusion |
| `substrate_light` | A light override | One boolean, `castsShadows` |

All four are placed by the node's world transform in **one node walk**, and for the same
reason: the same thing under two nodes is two things. Every key is optional, as `Config`'s
are.

**There is a fifth authoring convention and it is deliberately not in this table**, because
it is not an `extras` key: cloth is a `FABRIC_` mesh name plus a `_PIN_WEIGHT` vertex
attribute. `extras` is a per-node dictionary and a pin weight is a value per vertex, so the
schema this table describes cannot express it — see [Cloth](#cloth) for the decision and its
cost.

`substrate_light` exists because a point light inside its own emissive mesh shadowed
itself to black — the mesh encloses the light, so all six cube faces record the sphere at
near-zero distance. `kLightNeverShadowed` had existed since the light budget landed and
was reachable **only from C++**. A one-boolean schema normally reads as anticipation; here
it is the smallest thing that makes a lamp expressible.

### A world is composed out of imports (C41)

There is no scene the engine loads on a game's behalf. `Engine::init` brings up an empty
world and `Game::init` fills it: `scene().create(name)` for the node, then a `scene::Model`
component naming the asset -- `scene().add<scene::Model>(node, {path})` -- and the node's world
transform is where the import lands. Adding that component is what performs the import, and it
is the one component whose addition does work; the component left behind records which
`ModelId` the node holds, so the tree answers what is where.

**The node is the receiver, not a parameter.** The verb was `Engine::addModel(node, path)` when
C41 landed, which says a model is something the engine has and the node is an argument to it.
It is the other way round: the scene has nodes, a node has components, and a model is one of
them. `Engine` still does the import -- it owns the device, the uploader and the geometry
buffers -- but it does it through `Scene::setImporter`, a function pointer and an opaque
context, which is the same type erasure `ComponentStore` uses and keeps `Scene.h` free of
`Engine.h`.

**Each instance is attached at `inverse(placement) * instanceTransform`, and identity is the
trap.** The import's transform is already baked into every instance it produced, which reads
as "so the attachment offset is identity" -- and the sweep then writes
`node.worldTransform * offset` back over the instance every frame, erasing whatever the
*document's* own node hierarchy contributed. A file whose meshes all sit at its origin
survives that; one with a node translation loses it, and the geometry lands somewhere else at
exactly the right triangle count. It is the same arithmetic a driven mesh gets in
`bindDrivenNodes`, for the same reason.

The ordering that made the old shape necessary is gone. Physics, particles, lights, voices
and the bindless texture array all size themselves from what a game actually makes and grow
past it (C40, and `GltfScene::reserveTextureSlots` for the array), so nothing has to be
counted out of a document before `Game::init` runs. The physics world is created whenever
physics is enabled rather than only when the loaded document declared a collider -- the old
early-out is exactly what refused a controller to a game that composed its world in `init`.

`--scene` still loads one document into the world before `Game::init`, and `game/viewer` is
what runs against it: a game that composes nothing, which `run.sh` builds whenever no game is
named. That is why no game in the tree asks whether its world was replaced -- the harness has
a binary of its own instead of borrowing one.

**Scale is `Engine::setWorldScale`, not a scale on the node.** `scaleSceneData` holds rigs
and dynamic colliders at their authored size and carries light ranges, intensities and audio
falloff distances with the factor; `placeSceneData` does none of that, so a scale written into
a placement matrix stretches a rig and leaves a lamp lighting the wrong radius. The demo
imports Sponza at 2 through the first and places its props with the second.

### A second document, at a transform

`Engine::addModel(path, transform)` loads another glTF into the running world. The transform
is not a convenience on top of "load and then move it": a document arrives with lights,
emitters, sounds and colliders as well as geometry, and each of those is placed by its node's
world transform during the one walk above — so the place has to be known before the walk, not
applied to its output. `placeSceneData` is where it is applied, and it is the reason a light's
*direction* takes the rotation alone: running a direction through the full matrix drags it to
wherever the import was placed, which points every spot in the file at the same spot in the
world.

Two consequences a caller has to know, both of them ordering:

- **Colliders arrive with the import.** The import binds them to bodies and rebakes
  the navmesh when the import brought any, so an arena imported in `Game::init` is solid and
  walkable. It binds through the same two functions `initPhysics` does -- see below.
- **An instance is baked into the ray-tracing structure at the transform it was created
  with.** `setInstances` builds that structure synchronously, and a game that creates
  instances and then places them by attaching them to scene nodes has already handed over the
  wrong transforms. `Engine::frame` asks `staticTierStale` once per frame, straight after the
  scene sweep, and rebuilds if anything the static tier baked has since moved or been told it
  is dynamic — see [Skinned acceleration structures](#skinned-acceleration-structures) for
  the tiers themselves. That rebuild frees the structures frames still in flight are reading,
  so it takes `vkDeviceWaitIdle` first; the two load-time builds do not need one and do not
  take it.

What this replaces is a scene *baked by a script*: `make_composite_scene.py` grafted Sponza,
a character, an orb and a ground plane into one `showcase.gltf` at build time. That file is
gone — the demo imports Sponza and composes the rest in code, which is what this verb plus
C22's rig merge made possible.

### The scene sidecar, and the LOD chains it carries

`scene::SceneData` is the whole CPU-side result of a load, and `<name>.gltf.scene` beside
the source is that struct written out. Invalidated three ways before a byte of payload is
read — a format version, a layout digest folded from `sizeof` of every POD written, and the
source's size and mtime — so a stale cache produces *no cache* rather than a bad load.
Deleting every sidecar reproduces the golden set exactly, which is the property that keeps
it a cache rather than a second asset format.

**`scene::buildLodChains` runs inside the bake and nowhere else.** It walks the primitives,
calls `meshopt_simplify` for up to three halvings each, appends every level's indices to the
same array LOD 0 lives in, and records the ranges on the primitive. That is seconds of work
whose answer never changes between runs, which is exactly the kind of work the sidecar
exists to move offline — so a load from a document has no chains, and `substrate-bake` is
what gives a scene any (D9: the bake is a host-only tool, not a flag on the engine). On Sponza: **51 of 103 primitives take a chain, 650,946 indices added
(83% over the original), sidecar 12.4 → 14.3 MB.**

Three kinds of primitive are skipped, each because the selection could not reach them
anyway: under a thousand triangles, blended (drawn by the forward pass, which builds its own
commands), and deforming (drawn out of the buffer `skinning.comp` wrote). The chain lives
*inline* on `Primitive` rather than in an array indexed by primitive, so it is rebased,
serialised and appended by the same statement that moves the primitive — see
[rendering.md](rendering.md#mesh-lod-in-the-same-dispatch) for the selection that reads it.

### Textures and KTX2

`scripts/ktx2.py` writes `<image>.png.ktx2` beside every image; `engine/gfx/Ktx2.{h,cpp}`
reads it. Textures **272.0 → 90.7 MB**, scene VRAM **693.3 → 421.4 MiB**, texture load
**336.5 → 90.0 ms**, total load **368.0 → 120.8 ms**.

It is a **cache, not a second asset format**: nothing rewrites the glTF and nothing
declares an extension. `KHR_texture_basisu` would have been the conformant pointer and it
declares a *Basis* payload, which these files are not, so claiming it would be a lie about
the contents.

The reader is **not a transcoder and refuses to become one**. A UASTC or BasisLZ payload
needs the Basis codec to become something a GPU can sample, so the offline tool does that
(`ktx create --encode uastc` then `ktx transcode --target bc7`) and the reader rejects,
with a reason, anything it cannot memcpy. sRGB-ness is decided by the tool from the
material slot, because block compression is a property of an image *in a slot*, and the
loader warns when a stale cache disagrees.

### Texture residency

Bindless: one variable-count descriptor array binds the whole scene, so swapping a slot's
contents touches no pipeline. `acquireTextureSlot`/`releaseTextureSlot` over 64 free slots
past the loaded set, with a Debug self-check at load that takes two slots, returns the
first, and fails loudly if the next acquire does not hand it back. A reserved 1x1 white
slot is what every free descriptor points at.

**The 64 is a starting point, not a ceiling** (C41). `reserveTextureSlots` doubles the array
when an import needs more than the free list holds, appending slots past the end so every
index a material already names keeps its meaning, and re-creates the pool, layout and set --
which is why a caller has to rebuild anything holding the layout, and why `Engine::addModel`
calls `Renderer::setScene` after every import. A world composed out of imports starts with 65
slots and would otherwise run out on the first building.

**The transfer queue found a real constraint:** a transfer-only family cannot record
`vkCmdBlitImage`, so a batch that *generates* a mip chain must go to the graphics queue
and only pure copies can go to the DMA engine. A texture whose mips were built offline can
stream while the GPU draws; one that needs its chain generated cannot.

No eviction or prefetch **policy**, deliberately — that is a game's decision about its own
content.

**This is not the array a game loads an image into.** There are two, and they agree about
almost nothing on purpose: this one is the scene's, built at `setScene` and gone at
`destroy`, and its slots are named by a bare `uint32_t` nothing outside `GltfScene.cpp`
asks for. The other is `gfx::ImageTable` — growable, handle-keyed, generation-checked, and
the thing `e.images().load` and `ui::Context::image` speak to. See
[rendering.md](rendering.md#the-image-table-and-the-one-descriptor-array-that-grows). The
two are not unified because `acquireTextureSlot` still has no caller outside its own file,
and merging them would be promotion with nobody asking; the trigger and what has moved it
are in [limitations.md](limitations.md).

### Growable geometry, and geometry that never came from a file

`load()` sizes the vertex and index buffers to the file it was handed, with headroom.
Everything appended afterwards — `appendModel` for another glTF, `createMesh` for a mesh
built in code — goes through `reserveGeometry`, which **doubles the buffer and copies the
old contents forward** when the next model does not fit, and logs the reallocation with
its new size. Overflow used to be a refusal; it is now a copy.

The copy is why all three buffers carry `TRANSFER_SRC` as well as `TRANSFER_DST`. Their
usage flags are named once each, in `vertexBufferUsage`/`indexBufferUsage`/`kMaterialUsage`,
because they are now written in two places — `upload` makes the buffers and `growBuffer`
remakes them — and a grown buffer missing a flag the original had is a bug that surfaces
as a device loss somewhere else entirely.

**A grown buffer is a new `VkBuffer`, so every descriptor set that named the old one is
stale.** The bindless texture array does not care, because it names images; the skinning
set does, and pointed at freed memory until `writeSkinSet(slot, allocate)` was split out
of the allocate-and-write path so `setScene` can rewrite all `kFramesInFlight` copies
after a growth. That failure is worth stating plainly because it is silent without
validation layers: the shader reads whatever now lives at that address.

`createMesh(MeshData)` is then the same call path with the file reading removed — vertices,
indices, a material index and a transform, straight into the shared buffers. It costs an
allocation out of the same `RangeAllocator` a loaded model uses, and `removeModel` returns
it the same way; a thousand create/destroy cycles leave the allocator's largest free run
exactly where it started.

### A mesh made in code can morph

`MeshData::morphTargets` is a vector per target, each **exactly as long as the mesh**, in
the `MorphDelta` form the loader produces — so a code-made target and a file-authored one
are appended to one array in one target-major order and `skinning.comp` cannot tell which
producer a run came from. A target of the wrong length is refused with the whole mesh
rather than padded: the shader addresses a displacement as
`morphOffset + target * vertexCount + vertex`, so a short target reads the *next* target's
first rows.

Three arrays have to move together and none of them is the delta array alone:

- **The weights.** A character's weight block is sized from `AnimationRig::bind.weights` —
  what the *file* declared — so a scene whose glTF has no morph target gives every
  character a block of length zero. `SceneAnimator::createMorphed(targets)` asks for the
  block directly, and its weights are **held beside the pose and written back after every
  sample**, because a rig-driven pose is rebuilt from the bind pose each step and would
  otherwise resize a block belonging to no node of the rig out of existence.
  `setMorphWeight` is the only writer; no clip and no state machine reaches these targets.
- **The renderer's sizing.** `Renderer::setAnimator` reads `totalWeights()`, the delta
  array and one output range per deformed instance, all as they stand when it is called —
  so `Engine::createMesh` calls it again for a mesh that deforms, and only for one.
- **The CPU index copy.** `buildSceneAccelStruct` rebases a deformed primitive's indices
  onto the deformed vertex buffer *on the host*, reading `GltfScene::indexData()`. That
  copy is a snapshot the loader took; a morphed mesh made afterwards indexes past the end
  of it. `createMesh` extends it, and the invariant is positional — element `i` is element
  `i` of the device index buffer wherever the copy is written at all.

Morph deltas, like material slots, are **never freed and never repacked**: an instance
already carries `morphOffset`, so reclaiming a run out of the middle would renumber every
later primitive under instances that still index them. A retired character keeps its
weight block, zeroed, for the reason C1 gives about joint blocks — `GpuInstance::meta.w`
names a slot and crosses to the GPU, so a stale reference must read something harmless.

### Materials that change

`createMaterial` and `setMaterial` make the material buffer mutable, counted by
`materialRevision()` in the same shape as `InstanceTable::revision()`: each frame in
flight remembers the revision it last uploaded, and re-uploads only when the two differ.
A static scene therefore uploads once and a mutated one costs a `vkCmdUpdateBuffer` —
chunked to the 65536-byte limit that command carries, which is cheaper than owning a
staging buffer for a table this small.

`GpuMaterial` did **not** gain the `shader` index and `params` vec4 the row sketched. No
shader reads either field yet; G5 is what gives them meaning, and a struct field the GPU
ignores is a layout change that has to be got right twice.

---

## Animation and skinning

`engine/scene/Animation.{h,cpp}` plus `engine/shaders/skinning.comp`.

A clip sampler with LINEAR/STEP/CUBICSPLINE and slerp for rotations; a joint hierarchy
resolved parent-before-child; morph targets as one **target-major** `MorphDelta` array of
nine packed floats, so the weighted sum walks one contiguous run per target rather than
striding by target count.

`AnimationRig` is shared and immutable; a **character** owns a pose, a world array, a
joint block and a playback cursor. Joint blocks are laid end to end in one flat numbering
because that numbering **is** the dispatch's `jointBase` push constant.

`SceneAnimator::Character` is eleven members, the largest struct in the engine that is not
a GPU layout. It is a struct rather than a class because nothing outside the animator
touches it.

**The pose resolve allocates nothing.** `resolve` marks placed nodes in one buffer the
animator owns, not a `vector<bool>` per call — a heap allocation per character per fixed
step, worth 179 ns each. **Sharing one buffer across characters cannot alias, because it
carries no length of its own**: `assign(pose.nodes.size(), false)` sets both its length and
every element from the character being resolved, one statement before the first read, and
the loop that reads it is bounded by the same count. Nothing is laid out beside it to fall
out of step with when one of the two grows, which is the shape every aliasing defect here
has had. Determinism is checked rather than asserted — the same character playing the same
clip reaches the same world transforms whether it resolves first or fourth, and the marks
are read across several passes of the resolve loop, so a leaked one leaves a node unwritten
rather than merely off by an epsilon.

**The output is per instance, not per primitive**, because the bind pose is the input and
one skinned mesh may be instanced at several poses. The indirect command's `vertexOffset`
maps a primitive's absolute indices onto its instance's own range, so no index rewriting
is needed — which makes every geometry pass two indirect draws rather than one, still
O(passes).

Skinning and morphing are **one dispatch**: the displacement is added to the bind-pose
vertex and *then* the joints are applied. That is the order the spec fixes, and not a
preference — skinning first puts a character's expression in world coordinates and leaves
their face behind when they turn their head. **0.041 ms for five characters at 142k
vertices**, which is launch overhead more than work.

### A rig that arrives at runtime

A rig comes from the file. `GltfScene` builds one `AnimationRig` per load and
`SceneAnimator::init` takes it **by move**, which is also why `init` is the one door an
import cannot use: it clears every character, so a second file handed to it would destroy
the world's own characters along with the poses they hold.

`merge(extra)` is that second door. **Existing indices never move** — appended nodes are
numbered from the end, and their parents, their `firstWeight`, their skins' joint lists and
their clips' channel nodes are all shifted by the base counts on the way in. That is what
keeps an `AnimatorId`, a root-motion node index and a `GpuInstance::meta.w` written before
the import still meaning what they meant afterwards. It returns the index the first appended
skin landed at, or `kNoSkin` for a file that only added nodes: a rigless import gets no
character of its own, because the scene already has the one that exists so a hierarchy of
animated crates has something to resolve.

Three things have to grow with the nodes, and one of them is not in the rig at all:

- **`nodeNames`, laid out parallel to them.** A file that names nothing leaves the vector
  short, and a short `nodeNames` makes `findNode` search a prefix and `setRootNode`
  unreachable for every node past it.
- **Every existing character's `world`.** A pose is copied from `rig.bind` each step and so
  grows on its own; `world` does not, and the first resolve after a merge writes past the end
  of it. `merge` resizes them all and then resolves once, so an instance drawn on the import
  frame draws the appended bind pose rather than whatever the allocator had there.
- **The host index copy.** `appendModel` extends `indexData` for an appended primitive that
  deforms, for the reason [A mesh made in code can morph](#a-mesh-made-in-code-can-morph)
  gives about `createMesh`. `buildSceneAccelStruct` rebases a deformed primitive's indices on
  the host out of that copy, so a copy that stops short of the appended ones is a device loss
  on the next TLAS build rather than a wrong pixel.

The import is **two calls into the scene rather than one**, because the rig has to leave the
model before the model's skins can be renumbered: `takeAppendedRig(id)` moves the imported rig
out, `merge` says where its skins landed, and `rebaseAppendedSkins(id, base)` writes that base
into the placements. `Engine::addModel` does all three *before* it creates any instance, so no
instance ever exists at a skin index the merge has not yet defined.

What a merge costs is one pass over the appended nodes, clips and channels plus one resolve of
every character in the scene; the arrays are appended to, never rebuilt. `unloadModel` gives
back `skinData`, `morphData` and the cloth sources **only when the removed model owns the
tail** of each — the rule the vertex and index arrays already follow, and for the same reason:
a live instance carries `skinOffset` and `morphOffset`, so reclaiming a run out of the middle
would renumber the arrays under it. The rig is never shrunk at all. Its nodes and clips
accumulate across imports, which is a bounded, per-import cost paid in host memory and no
part of what an instance indexes.

### Which rig a bare node index belongs to

`ParticleEmitter::node` and `AudioSourceDesc::node` are indices into the merged rig and carry
no rig of their own — the glTF `extras` schemas have nowhere to put one — so with two rigs in a
scene the index alone is ambiguous. `SceneAnimator::characterForNode` resolves it, and
`Engine::poseFor` is the one place both attachment sweeps ask.

The map is rebuilt at the top of every `update`, in two passes, and the order of the passes is
the design. **Skins first**: a node a skin lists as a joint belongs to a character on that skin.
**Clips second, and only for what the skins did not claim**: a node no skin owns is a *rigid*
animated node — a drawbridge, a clock tower, a lift — and the character that moves it is the one
playing the clip that names it. Running the clip pass first would let a clip that happens to
mention a joint take it from its own skin.

**First claim wins, and that is what keeps N copies of one rig agreeing.**
`spawnExtraCharacters` gives every copy the same skin, so a joint really is shared between them
and any answer but a stable one puts an attachment on a different copy each frame. A node
nothing animates is nobody's; `poseFor` hands those character 0, which is the narrow case the
old sweep was right about.

Rebuilt rather than kept in step by the calls that could invalidate it: `create`, `destroy` and
`merge` can all move a node's owner, and one pass over the joints per frame is cheaper than three
dirty flags that have to agree.

### Blending and state machines

`blendPose(dst, src, t)` moves one pose toward another — lerp for translation, scale and
weights, slerp for rotation. A cross-fade is that applied to the outgoing and incoming
clips **both still advancing**, so a character interrupted mid-run keeps running while it
blends into the stop.

The machine is states, transitions and parameters, with `kAnyState` for "from anywhere"
and triggers a transition consumes when it fires. **Table order is priority**, which is a
rule a reader can apply by eye. The demo builds one by *name* against whatever clips the
scene has, several spellings each, because Mixamo, Khronos and Blender name them
differently and the alternative is one machine per exporter.

**The machine belongs to the character, not to the animator**, and so does the root-motion
node. It read the other way until C23 — one machine and one root joint for everybody — which
is invisible at one rig and a hard limit at two: a guard could not have its own clips, and a
rig whose pelvis is not called `Hips` silently got no root-motion hold because the name was
resolved once and applied to everything.

`setStateMachine(machine)` still installs one for every character and is what a one-rig scene
calls; `setStateMachine(id, machine)` gives one character its own. The animator-wide call also
keeps a *template*, so a character created after it starts on the same entry state rather than
on nothing — which is why "install the machine, then load the rig" and the reverse both work.
`setRootNode` has the same pair, and the per-character one is guarded against being handed the
node it already holds, because setting it restarts the measurement and a game resolving its
root joint every frame would otherwise report a delta of zero for ever.

What this does **not** yet split is the rig: `findNode` searches one rig per animator, so a
per-character root node named by string is only half an answer until each character has its
own rig. That seam is left visible rather than papered over.

### The engine writes the parameters (G15)

`scene::LocomotionDriver` maps a `PhysicsCharacterId` to an `AnimatorId` and writes what that
pairing implies — `speed`, `airborne` and a `jump` trigger — from `Engine::simulate`, after
`PhysicsWorld::step`. The pairing is derived rather than declared: a `CharacterVirtual` is a
capsule with no rig and an animator character is a pose with no collider, and what joins them
is a skinned mesh the scene bound to the collider's node, whose `InstanceTable::characterOf`
says which pose deforms it. So a scene with a second rig in it needs no game code at all.
`Engine::locomotion()` is the door for the cases the engine cannot see — a rig a game spawned
itself — and for a rig that spells its parameters differently.

**The engine names no player, and `Engine::authoredCharacters()` is what it says instead
(G17).** It is the collider walk's output — every `Character` collider a loaded file declared,
each with the node the engine attached it to — and the engine does nothing further with it.
Which of them anybody is driving is the game's answer to give: an RTS drives none, a party game
drives four, a possession game changes its mind mid-run.

What that replaced was a `playerCharacterIndex` latched first-wins from the same walk, plus a
`setPlayerCharacter` to override it (G16). **Nothing in the engine ever read it.** It was
written in four places, compared against itself twice to find the node, and handed out through
two accessors that only a game called — so the singular was not a constraint the engine needed,
it was a promise the API made and could not keep: three of four players unnameable, and a scene
with no player character silently promoting whichever NPC the loader walked first.

The tables underneath were plural before this and are why the change is small: `PhysicsWorld`
holds characters by handle with a free list, `LocomotionDriver` updates every live pair, and
`SceneAnimator::characterForNode` resolves an attachment against the rig that actually animates
it. A game holds its players in whatever shape it needs; `game/demo/DemoWorld.h` holds a
`std::vector<Player>` of controller, node and rig, and fills one.

This was eighty lines in a game before it, and it worked for exactly one rig. **The parameter
names, the normalising divisor and the set of triggers all belong to the rig**, so a second
rig meant a second copy of the loop with different constants in it. The names stay strings and
stay the rig's — an engine that hard-coded `"speed"` would decide what a machine may contain,
which is the thing the per-character split above exists to stop — and a name a machine does
not have is skipped rather than refused.

**And they are held per pair, which took a second row to become true** (D19). The paragraph
above was the header's argument for a driver that kept one `Parameters` for all of its pairs,
so a human and a horse from two exporters could not both animate: naming the second rig's
parameters un-named the first's, and the first stopped blending on the call that started the
second with nothing anywhere able to say so — `findParameter` answers `kAnyState` for a name a
machine does not have, and that answer is deliberately silent. `pair(controller, rig, names)`
is the three-argument form; `setParameters(rig, names)` renames one pair afterwards.

The driver-wide `setParameters` still exists and does **both halves**, which is what
`SceneAnimator::setStateMachine` does and for the same reason: it moves every existing pair and
becomes the template for later ones, so a one-rig scene stays a one-call scene and the answer
does not depend on whether a game pairs before or after it names its parameters. Re-pairing a
rig replaces the controller and keeps the vocabulary — a rig handed a new body on a respawn
must not quietly move back onto the defaults.

**The divisor is the one that had to move somewhere honest.** The demo wrote `speed / 4.0`,
which asserts that the machine's thresholds sit at 4 m/s: a fact about the *rig's* thresholds
and the *collider's* `moveSpeed`, two things the engine holds and the game had to guess
consistently with. It guessed wrong — the showcase collider tops out at 3.2 — so the parameter
could never exceed 0.8 and the top fifth of every blend was unreachable, with nothing anywhere
able to say so. `speed / characterMoveSpeed` makes the assertion checkable and makes a rig
authored against a different top speed work without editing a game.

**After the step and not before it.** Every value the driver writes comes back out of the
solver, so it has to read a solver that has looked at the world: before the first step a
`CharacterVirtual`'s ground state is the one it was constructed with, which reads as *in the
air*, and a driver reading it makes every character fall and land in the first three steps of
every run with nobody touching a key. Running after the step removes the moment rather than
guarding it, and the animator picks the values up on the next step — one step of latency,
uniformly.

Six states — idle, walk, run, jump, fall, land — over three parameters, and three things
about that table are decisions rather than shape:

- **The airborne parameter is spelled `airborne`, not `grounded`.** Every parameter starts
  at zero, so a machine nobody is driving has to read as *standing on something*; spelled
  the other way round, a scene whose characters run off the fixed step rather than off a
  controller falls through the floor of its own state machine on the first frame.
- **`fall` is entered by three enumerated transitions and not by a wildcard.** `kAnyState`
  would also hold one step after a launch and cut the jump clip short, so the wildcard is
  spent on the one thing that genuinely interrupts everything: the jump trigger.
- **`findState` returns `kAnyState` for a name the rig does not have**, which is the same
  value `from` uses for the wildcard. A missing state written straight into a transition is
  therefore a *wildcard*, not a no-op — so transitions between two named states go through
  a helper that refuses the sentinel, and the wildcard is written out by hand.

Which clip a state takes is not free either: `jumping up` is preferred over `jump`, because
on a Mixamo rig the second is the whole leap including its own landing and would play that
landing a second and a half before `land` does.

**No blend tree**, deliberately: interrupting a fade drops the clip being faded out rather
than keeping a third playback, and the alternative is an unbounded stack of them.

**`advance` and `crossedEvents` are also the sprite flipbook's**, through an overload taking
a duration and an event list rather than an `AnimationClip` — see
[Sprites](#sheets-are-a-rectangle-on-a-sprite-and-the-vocabulary-is-the-animators). One
event model serves a skeleton and a flipbook, so a game that plays a footstep off one and a
spark off the other learns one vocabulary rather than two.

### Root motion, and the node that carries it

A locomotion clip authored with the feet planted moves the rig forward. Whether that motion
should reach the character is the *game's* answer, so `SceneAnimator::setRootNode(node)` is
opt-in: naming a node makes `update` record its per-step translation delta and then **hold the
node at its bind translation**, so the pose animates in place and the delta is the caller's to
apply — to the controller via `setCharacterInput`, or nowhere. Doing one without the other is
the bug the single call exists to avoid; leaving the node moving *and* reporting the delta
moves the character twice.

**Naming it is by string, and that is not a convenience.** `AnimationRig::nodeNames` keeps what
the file called each node and `findNode` turns one into an index, because a rig's joints belong
to the file and a game cannot know their numbering. Without it the opt-in was unreachable:
`setRootNode` shipped with six unit tests naming a synthetic rig's node 0 and no caller in the
tree, and the showcase character consequently travelled at the sum of its controller's speed
and its clip's, snapping back to nothing whenever the machine blended to a clip that stands
still. **The names are in the baked scene as well as the document** — a cache that dropped them
would leave the feature working from a glTF and silently off from the fast path, which is the
normal path.

The node is not always the obvious one. Mixamo rigs carry a `mixamorig:Root` that never moves
and put the travel on `Hips`, so "the skeleton's first joint" names the one node with nothing
on it. The demo asserts the outcome rather than the wiring: `scripts/locomotion.sh` reads a
`drift` figure off every arm — how far the pose carried the root, worst over the run — and
requires it under 2 cm. Unheld, `walk-run-jump` measures 3.17 m.

### Determinism

Animation time is `frame * fixedStep`, later `update(step)` accumulated — a state machine
has history, so an absolute time would have to replay every transition to reach it, and N
identical additions land where N identical multiplications did. Frame N is the same image
on every run.

### Skinned acceleration structures

One BLAS per deformed instance with `ALLOW_UPDATE` + `PREFER_FAST_BUILD`, refitted from
the buffer the deformation dispatch just wrote, under a TLAS **rebuilt** every frame over
`1 + N` instances. A TLAS is rebuilt rather than refitted because a refit degrades under
exactly what a character walking across a room produces. **0.169 ms for six structures.**

**The refit costs five times what the deformation does** — 0.123 ms against 0.024 for one
character — so it runs only while ray tracing is on. The first version refitted
unconditionally, arguing that a structure refitted only while tracing is one frame stale
the moment somebody turns tracing on. Measured, that buys one frame of staleness on a
debug keypress for 0.12 ms every frame of a feature that is off by default, and it lost.
**The cost argument only decides anything after it has a number.**

**A BLAS build has no signed `vertexOffset`.** The draw path maps a deformed instance onto
the scene's absolute indices for free — that is what an indirect command's signed field is
for — and a BLAS build has only a `firstVertex` that is added. Indices are rewritten once
at load into a second buffer, which is why `GltfScene` retains its index array, and only
for a scene that deforms.

---

## Particles

`engine/scene/ParticleSystem.{h,cpp}` and six shaders. One dispatch per pool slot integrates,
collides and lights; a second writes the frame's births.

**The pool grows, and only `Engine` may grow it.** `ParticleSystem::grow` resizes `deathTime`
at the tail — a live slot is one whose death time is still in the future, so everything in
flight keeps its slot — and `Renderer::resizeParticlePool` reallocates the GPU buffers and
copies the old pool forward. Doing one without the other emits into storage the device does
not have, which is why the header used to say flatly that the pool is not resized and a game
had to state `particleBudget`. `Engine::growParticles` runs after each step, is the only caller
of either half, and grows to `wantedCapacity()` — the requirement summed over the **live**
emitters, so a retired burst does not hold the pool at its peak. Only `particlePool` is copied:
`particle_simulate.comp` rewrites every sort key each frame, and the spawn and emitter buffers
are filled per frame from the CPU (C40).

**There is not one atomic in the subsystem**, and that is the whole design. A GPU dead
list with `atomicAdd` is one dispatch smaller and hands two particles born on the same
frame different slots between runs, which is a different image. Slot allocation is
therefore on the CPU. That is only affordable because **a particle dies of age and nothing
else**, which is what makes depth collision bounce rather than kill.

**A particle carries its birth time, not its age.** An age advanced by `+= dt` on the
device and a death predicted by addition on the host drift apart by an ULP and disagree
about which frame a particle dies on. The consequence is a slot reused while its occupant
is still drawing, which presents as a sorting artefact and is not one.

### The sort

A bitonic network over the whole pool — a **fixed** comparison network, so the result is
bit-identical run to run and a particle scene can join the golden set, which a radix
sort's scatter could not promise. Passes with `j < 256` collapse into one shared-memory
dispatch, turning a 2,048-key sort from 66 dispatches into 15. Dead slots carry the
largest key there is and land after every live one, so the draw takes the first
`aliveCount` entries and needs no compaction. **0.026 ms.**

**The sort is 6% of the pass; fill rate is the other 94%.** The expensive thing about
blended particles is that they are blended. The next work in this area — a low-resolution
target, soft particles, a size cap — is all about overdraw.

**One blend state is what makes one global sort possible.** Premultiplied
`src + dst * (1 - src.a)` with a zero alpha reduces exactly to `src + dst`, so an emissive
particle is additive for free. Two blend states would be two draws, and two draws could
not share a sort.

### The sheet is the effect

**A soft radial disc cannot make fire, and no amount of tuning fixes that.** The demo's
braziers spent eleven rounds getting smaller and denser on the theory that enough discs
blend into a volume. They do not: a disc has no interior, so however many are stacked, the
ones on the *boundary* are still discs. The symptom is fire that reads as coloured bubbles,
and it is a property of the mask rather than of the numbers.

So an emitter names a **flipbook** — a grid of frames with turbulent internal detail, walked
over the particle's life from a start cell its own seed picks. One particle is then a tongue
of flame rather than a dot contributing to one, which is why the count went the opposite way
to intuition: 12,000 discs became 1,000 sheet particles, and the pool fell from 8,192 to
1,024. The procedural disc stays for what it is genuinely right for — a spark is a point of
light.

Three things travel with it, all off by default so every emitter authored before them
renders identically:

- **Phase randomisation.** Random start frame, random billboard angle, random spin rate. A
  field of identically-oriented sprites reads as one repeated stamp however good the sprite.
- **Playback decoupled from lifetime** (`flipbookLoops`). One loop per lifetime is one loop
  per *short* lifetime — sixteen frames across a 0.6 s particle is 26 fps of churn, which
  reads as a fire in a hurry.
- **Erosion.** The sprite's own alpha is the noise field, so a particle dissolves along the
  detail it already has instead of dimming as one shape.

**Two traps, both found by getting them wrong.** The sheets wrap in *time*
— `flame_sheet.png` and `smoke_sheet.png` are authored that way — because a particle that starts on frame 11 crosses the seam
mid-life and a discontinuity there pops for every particle at a different moment. And a
particle appears at `sizeStart` in a single frame, so a `sizeStart` larger than `sizeEnd`
makes every birth a full-size pop at a random offset — with a small population that is a
flame whose base visibly jumps. Flame particles are born small and grow.

**The sheets are white with the shape in alpha.** All colour comes from
`colorStart`/`colorEnd`, so a blue or green flame is four numbers rather than a second
texture.

### Lighting

**Per particle, not per fragment.** A blended billboard is the most overdrawn thing a
renderer draws, so shading it per fragment multiplies the light loop by the overdraw *and*
the pixel count; shading it once in the simulate dispatch costs O(particles x lights) —
three orders of magnitude smaller than O(pixels x lights).

### Depth collision

Reads the single-sample depth the decal pass already binds and reconstructs the surface
normal from the depth gradient. **It sees the screen and nothing else**, which is stated
rather than discovered: a depth buffer is not a collision world.

---

## Sprites

`engine/scene/SpriteTable.{h,cpp}` — layers, sprites, sheets, handles and the order they
draw in.
**Hosted**, so all of it runs under ASan; the pass it feeds is
[rendering.md](rendering.md#sprites).

```cpp
e.camera().projectionMode = scene::Camera::Projection::Orthographic;
e.camera().orthoHeight = 180.0f;                  // world units of visible height
background = e.sprites().createLayer({.order = -10});
actors     = e.sprites().createLayer({.order = 0});
hero = e.sprites().create(actors, {.image = heroImage, .uv = {0, 0, 16, 16},
                                   .size = {16, 16}, .pivot = {0.5f, 1.0f}});
e.sprites().setPosition(hero, at);
```

`SpriteId` and `SpriteLayerId` are both `core::Handle<Tag>` under C1's rules — two distinct
types, because `destroy(layer)` and `destroy(sprite)` are different verbs on different
arrays and a shared `uint32_t` would let one be passed where the other belongs. Destroying
a layer destroys its sprites, and it does so *through* `destroy` so each one's generation
moves rather than being torn out silently.

### Sprites are their own dense array, not scene-tree nodes

A sprite is a quad with a tint and a UV rect. It has no material, no mesh, no bounds worth
culling and nothing for `InstanceTable::dynamicCount()` to say about it, so making every
sprite an instance would put ten thousand blended entries through a G-buffer built for
surfaces a flat quad does not have. That is also the line between this and P6's lit
sprite, which *is* an instance and pays for it deliberately.

### The sort runs when the order changes, not when a sprite moves

**The design the ten-thousand-sprite budget rests on.** `SpriteTable` keeps its
`GpuSprite` array dense and already in draw order, so a frame that changed anything is one
`memcpy` and no gather, and a frame that changed nothing is neither. A layer is a sort key
and a *position is not part of it*, so:

- `setPosition` is two float writes into the buffer the draw will read. No re-sort.
- `create`, `destroy` and `setLayerOrder` mark the order dirty; `prepare()` re-sorts once,
  before the next frame.
- `destroy` swap-removes and lets the pending sort repair the order, rather than paying
  an O(n) erase to preserve an ordering that is about to be recomputed.
- Image slots are re-resolved only when `ImageTable::revision()` moves, the same
  reconciliation `Renderer::syncImages` does. A destroyed image drops its sprites to the
  fallback slot — the font atlas — rather than to whatever takes the slot next.

In the steady state `prepare()` is two comparisons. The sort itself is `std::sort` over an
index permutation, keyed on `(layerOrder, creationSequence)` packed into one `uint64_t`
with the sign bit flipped so a background at `order = -10` draws *first*. The sequence
number is unique per sprite, so the key is already a total order and stability is a
property of the key rather than of the algorithm — which is what makes the frame
reproducible run to run.

**Sorting is on the CPU, and that is a deliberate stop.** The bitonic sort next door would
be the second occurrence of a pattern, not the third. The trigger to move it is a
measurement, and the measurement now exists: ten thousand sprites cost 0.037 ms of CPU
*including* the upload, and none of it is the sort.

### The upload, and the one counter that gates it

`revision()` is bumped by every mutation of `draws()` and by nothing else, and the renderer
holds one per frame in flight — the same shape [the instance table](#the-upload) has, and
the fourth place the pattern appears. The consequence is stated on the table rather than
discovered in the pass: **a static screen of sprites uploads once and then costs nothing**,
where before it paid 64 bytes a sprite a frame forever. The pass's half of it, including
why it is the whole array or none of it, is in
[rendering.md](rendering.md#the-upload-is-once-per-revision-per-frame-slot-not-once-per-frame).

Two rules keep it honest, because a revision that misses a mutation is a sprite that quietly
stops updating on one frame slot in three — and a sprite that stopped moving looks exactly
like a sprite that was told not to.

- **The eight per-sprite setters cannot forget it.** `at(SpriteId)` is the only way to reach
  a writable `GpuSprite`, and it bumps on the way in, after refusing a stale handle. That is
  structural rather than remembered: a ninth setter gets it by construction, and a game
  calling a setter with a dead handle forces no copy.
- **`applyFrame` bumps inside its guard, not outside it.** P5 writes a sheet's rectangle only
  when the *cell* moves; bumping on every step would upload the whole array to write a
  rectangle already in the buffer, and the four-fifths-of-no-work property would be gone.

`create`, `destroy`, `sort` and the image reconcile in `prepare` are the other four writers
and bump where they write. `shutdown` bumps rather than resetting, so a table re-`init`ed
into a renderer still holding the old per-slot numbers cannot match one. Sheet bookkeeping —
`createSheet`, `addClip`, `destroySheet` — and playback control — `stop`, `setPlaying`,
`setSpeed` — touch no byte the pass reads and deliberately do not bump.

### What a sprite is, on the GPU

Sixty-four bytes, four `vec4`s, and every field is there to avoid computing something
twice. The tint is packed RGBA8 exactly as `OverlayVertex`'s colour is — a tint is a colour
a person picked, and opaque white round-trips exactly, which is what the readback check
depends on. The rotation is stored as a cosine and a sine, written once per *change*
rather than recomputed six times per sprite per frame in the vertex shader.

A flip mirrors the **UV rect**, not the geometry: the quad stays where the pivot put it and
the image inside it turns round, which is what every 2D tool means by `flipX` and what
keeps a character's feet in the same place when it faces the other way.

### Sheets are a rectangle on a sprite, and the vocabulary is the animator's

```cpp
sheet = e.sprites().createSheet({.frame = {32, 32}, .columns = 8, .count = 24});
run   = e.sprites().addClip(sheet, {.name = "run", .first = 8, .count = 6, .fps = 12,
                                    .events = {{0.25f, "left"}, {0.75f, "right"}}});
e.sprites().play(hero, sheet, run);
e.sprites().setUv(tile, e.sprites().frameUv(sheet, 17));   // a cell, with no playback
```

An animated sprite is a sprite whose `uvRect` is written once per **frame change**. There is
no `AnimatedSprite`, no second pass and no second shader — `GpuSprite` already carries the
rectangle, because a cell of an atlas and a frame of a flipbook are the same rectangle asked
for twice. `SpriteSheetId` is a third `core::Handle<Tag>` under C1's rules; destroying a
sheet stops every playback reading it and leaves each sprite on the cell it was showing,
which is a smaller surprise than a rectangle that starts naming whatever takes the slot next.

**The animation vocabulary is C7's, reused rather than restated.** `LoopMode`,
`ClipPlayback`, `AnimationEvent` and `advance`/`crossedEvents` come out of `Animation.h` and
mean here exactly what they mean for a skeleton: a paused playback holds, a `Loop` clip
wraps, every crossing in the step fires and each fires at most once. `advance` and
`crossedEvents` grew an overload taking a **duration and an event list** rather than an
`AnimationClip`, because a flipbook has no samplers and no channels — handing them a
synthetic clip with two empty vectors would be a lie about what it is, and copying the wrap
arithmetic into `SpriteTable` would be the anti-pattern the Rule of Threes names outright.

What P5 adds is the one thing skeletal animation has no equivalent of: a frame index.
`frameAt` is `first + min(floor(time * fps), count - 1)`, and the `min` is not a defensive
clamp — `time * fps` reaches exactly `count` at the duration, which is where a `ClampToEnd`
playback sits for as long as it is held. `frameUv` is the slicing: column `n % columns`, row
`n / columns`, plus an origin for a margin and a spacing for gutters. Both are public and
neither needs a playback, so a tilemap is `setUv(t, frameUv(sheet, cell))` and nothing else.

Timing runs off the fixed step — `Engine::simulate` calls `SpriteTable::update`, beside
`SceneAnimator::update` — so a paused game has paused sprites and a time scale slows them,
for free, because it is the same accumulator (C4). Events are a list read after the update
(`firedEvents()`), never a callback, and it is cleared by every update including one that
fires nothing.

**Deliberately not here**: per-frame hold times, ping-pong, and arbitrary frame lists — a
clip is a contiguous run at one rate, and `SpriteClip` grows a `frames` vector when an
authored sheet in the tree needs a held cell. No sheet *file* format, because the two worth
reading disagree about everything and neither is in the asset tree. And no state machine:
`AnimationStateMachine` is already generic over clip indices and would drive these clips
unchanged, which is the argument for not writing a second one.

---

## Physics

`engine/scene/Physics.{h,cpp}`, Jolt v5.6.0 submoduled. Both files are **hosted**, so the
solver runs under ASan and TSan.

### Jolt's build defaults

Three had to go, and two are traps rather than preferences:

- `OVERRIDE_CXX_FLAGS` defaults ON and rewrites `CMAKE_CXX_FLAGS_*` for the **whole
  project**, which would have fought `build.sh`'s sanitizer flags.
- Jolt 5.6's GPU hair backends are ON by default, compile a dozen HLSL shaders through a
  `glslc` this project does not use, and fail before a source file is touched. `JPH_USE_VK`
  would also have Jolt open a **second Vulkan device** beside `VulkanContext`.
- `CPP_RTTI_ENABLED` must be ON to match the rest of the project, or the one derived type's
  typeinfo references a base's that was never emitted.

`CROSS_PLATFORM_DETERMINISTIC` is deliberately **off** — see
[limitations.md](limitations.md).

### The world grows, because Jolt's cannot

`PhysicsSystem::Init` takes `inMaxBodies` and has no resize path: `BodyManager::Init` reserves
the body array and allocates the active-body lists outright, so a `Body*` held by a solver job
across a step can never be invalidated. That is Jolt's decision and a sound one, but it is not
a number a game can know, so C40 put the growth in `PhysicsWorld` instead of in `configure()`.

`grow()` reads every live body's state back through a `BodyLockRead`, captures each cloth's
`SoftBodySharedSettings` plus its per-particle positions and velocities, and captures each
character's pose — then replaces the whole `Impl`, rebuilds the system at double the capacity,
and re-creates all three against it. **A `BodyId` survives**: an id is an index and a
generation into `PhysicsWorld`'s own vectors, and only the raw Jolt id inside each slot changes.
A `CharacterVirtual` cannot survive, because it holds the `PhysicsSystem*` it queries — so it is
rebuilt and given `RefreshContacts`, which is what stops a growth reporting every character as
airborne for a step. Growth happens from the create verbs, between steps, in the same window
`reclaim()` uses and for the same reason.

The interpolation snapshots are **four arrays, not two**. They were one pair laid out bodies
first and characters after, which meant creating a body shifted every character's slot along:
`characterTransform` ran off the end and answered identity until the next `snapshot()` repaired
the layout — a character at the origin for one frame because something else spawned. That was
survivable while runtime body creation was rare; growth makes it ordinary, so a slot's index no
longer depends on how many of the other kind exist.

**`DEBUG_RENDERER_IN_DEBUG_AND_RELEASE` and `PROFILER_IN_DEBUG_AND_RELEASE` are adjacent
options that both default ON, and they get opposite answers.** The debug renderer is wanted
in Release: a collision shape you can only see in a Debug build is one you cannot check
against a frame time. The profiler is wanted nowhere — nothing in this tree calls
`JPH_PROFILE_START`, so all 193 of Jolt's scoped measurements find a null `ProfileThread`
and cost a thread-local load and a branch around a stack sample that is never written,
collecting nothing anything can read. It was on by inheritance rather than by argument
until it was measured: turning it off cuts the `simulate` zone on `physics.gltf` from
0.0175 to 0.0157 ms in Release, ~10% of a zone that is ~2% of that frame, and moves no GPU
zone at all. `PROFILER_IN_DISTRIBUTION` is forced off beside it because it would put the
define back on its own.

### The fixed clock

`FixedClock` belongs to the engine, not to the physics world: animation, particles
and audio step on it too, and a scene with no collider still has one. `--realtime` and
`--locked` select what `dt` it is fed and nothing else differs — fed exactly `step` (`locked`) it
lands on exactly zero in float, so one step runs, `alpha()` is exactly zero, and every
frame is a function of the frame index.

**`realtime` is the default, and `locked` shipped as the default for one release too
long.** Locked feeds one step per *frame*, so with vsync off the demo renders at several
hundred FPS and the whole simulation runs at roughly ten times real time — visible as a
character sprinting through an idle clip, and as physics that settles impossibly fast. It
was the default because eleven golden cases and every per-pass measurement need frame 60
to be the same frame 60, which was a reasonable trade only while `main.cpp` was both the
engine and the game. It is not one now: `scripts/golden.sh` and `scripts/baseline.py` pass
`--locked` and pin what they depend on, exactly as every golden case already names its own
scene rather than inheriting the game's. The clock has had no JSON key since S1 for the
same reason: it is a developer control, and a benchmark harness is not "the person running
the program". **A tool that needs determinism asks for it;
the engine ships what a game wants.**

**Characters are driven by `stepCount()`, not `frameCount()`.** The two differ by one on
the first pass: `frameCount()` counts frames *already drawn*, so it is zero while the first
step runs, and `consume()` has already counted the step by the time the loop body executes.
A clip driven by the wrong one is offset by a single step — invisible in motion, and it
moves every golden image.

### Colliders and bodies

A collider on a node that carries a mesh drives that node's instances and sets
`kInstanceDynamic`. **That is the whole coupling** — no component, no registry, no scene
graph. A flat list built once at startup.

**One walk, in two halves.** `Engine::createColliderBodies` turns a range of colliders into
bodies and characters and binds any sound authored on the same node; `Engine::bindDrivenNodes`
makes a scene node per driven body afterwards. Both `initPhysics` (for a `--scene` document)
and `Engine::addModel` (for every import) call the pair, and the split is exactly where they
differ: `initPhysics` runs `initCloth()` between them, because a cloth has to be in the world
it collides with, and an import has none. The order is load-bearing in one direction —
**bodies, then `PhysicsWorld::finalize`, then the binding** — since `finalize` takes the
world's snapshot, and a rest transform read before it is the identity, which puts every driven
mesh at twice its distance from the origin. The two callers also disagree about ownership:
`initPhysics` walks the whole table and so rebuilds `sourceBody` and `authored`, while an
import extends them.

Two things the shape path has to get right and would hide: a hull or mesh built from the
node's geometry needs its indices rebased off the shared buffer, and **a Jolt body has no
scale at all** — the node's goes into the shape, and a shape that refuses it (a capsule, a
sphere under non-uniform scale) is corrected and says so.

The instance is attached to a scene node with `inverse(bodyAtRest) * placementAtRest` as
its offset rather than having the body's transform written straight into it, which carries
the scale and carries a mesh authored at an offset from its collider for free. That offset
is a matrix on the attachment rather than a child node's local transform, and the reason is
arithmetic: a local transform is translation, rotation and scale, so going through one
would put every driven mesh through a decomposition, which is exact in mathematics and not
in floats.

**Binding is by the nearest ancestor collider, not the placing node.** A collider sits on
the node an author can see; a rig's skinned meshes hang several levels below it. Matching
the placing node is correct for every collider in a simple test scene and finds nothing on
a rig — the character controller drove nothing at all, and the report said `0 driven
instances`.

### Moving a body, and confining it to a plane

Four calls, and until P7 there were none: a body could be created, destroyed and asked
about, and nothing in the engine's surface could make one move.

| Call | Applies to | What it does |
|---|---|---|
| `addImpulse(id, kg·m/s)` | dynamic | Accumulates, at the centre of mass. Wakes the body. |
| `setLinearVelocity(id, m/s)` | dynamic, kinematic | Replaces. Clamped to Jolt's maximum rather than asserted against it. |
| `linearVelocity(id)` | any | Read from the solver, not from an interpolation snapshot — there is no half-step velocity. |
| `setBodyTransform(id, m)` | dynamic, kinematic | Teleports, and writes **both** snapshots. |

**`setBodyTransform` was widened rather than joined by a `setTransform` beside it.** G3 added
it for kinematic bodies only, because its one caller was a scene node pushing its attachment
downward and a dynamic body's transform is the solver's to report. The solver owns how a body
*moves*; a respawn, a level reset and a portal are not movement, they are the same operation
on the same handle, and a second verb would have differed from the first only in which motion
type it refused. Static is still refused with a reason — a static body's place in the broad
phase was decided by `finalize()`.

**A teleport keeps the body's velocity**, which is the one thing about the widening a caller
has to know. Zeroing it would make a portal impossible and a respawn convenient;
`setLinearVelocity(id, {})` is the other call, and needing it is a large part of why the two
landed together.

A non-dynamic body handed an impulse is **refused with a reason**. Jolt's own `AddImpulse`
checks `IsDynamic()` and returns silently, which is a crate that does not move and a log that
says nothing.

**2D is a constrained 3D body, not a second solver.** `ColliderDesc::freedom` is
`ColliderFreedom::All` or `ColliderFreedom::Plane2D` — X and Y translation, Z rotation — and
it becomes Jolt's `EAllowedDOFs`. That is the solver's own constraint rather than a
correction applied after each step: the disallowed rows and columns of the inverse mass and
inverse inertia are zeroed, so a confined body is never solved off its plane in the first
place. The unit suite asserts the plane coordinate is *exactly* unchanged over 300 steps of
gravity, floor contacts and impulses pushed along the forbidden axis, with a control arm that
drifts under identical treatment — an implementation that clamped the position each step
would pass a tolerance and fail that equality.

The plane is chosen to agree with everything else here that already has one: gravity is -Y,
an orthographic camera looks down -Z, and a sprite's layer is its depth. There is one plane
and no switch for a second; a game that wants another rotates its world.

**Navigation used to disagree with it, and navigation is the side that moved** (D18). A
navmesh's solver is Y-up — the slope filter measures against +Y and the funnel reads a
portal's left and right off a winding that only holds in a right-handed basis — so the only
navmesh a game could bake was XZ, and a 2D game's bodies and its navmesh lived in
perpendicular planes with neither subsystem able to say anything was wrong. `Plane2D` keeps
its plane because three other conventions already agree with it; `NavBuildParams::up` is what
lets the bake take a world in any other, rotating into the solver's frame and back out. The
rotation is a rotation and not an axis swap for the funnel's sake, and +Y performs no
arithmetic at all, so a 3D scene's navmesh is bit-for-bit what it was.
`PathFollower::up` is the other half: a follower dropping the wrong axis measures its progress
along the one axis the agent is not travelling on, and never reaches a waypoint.

#### The funnel emits a point wherever the path runs through a portal endpoint

A straight path across an open plane came back with a **collinear** extra waypoint at its
midpoint — a diagonal across square cells passes exactly through a portal endpoint at every
corner, and a zero signed area reads as the sight lines having crossed. Smoothing could not remove
it: the segment in question runs *along* the shared edges it is asking about, and `raycastNav`
used to find no exit edge for a ray leaving a triangle through a vertex — reporting blocked for
a line lying entirely on the mesh, which is why the same world straightened in Debug and did not
in Release.

**That degeneracy is resolved by simulation of simplicity, not by an epsilon.** The walk is run
as the walk of the ray displaced infinitesimally off its own line, with the displacement
direction computed once from the start triangle and held for every step, so a vertex with zero
signed area counts as being on that side. An absolute epsilon would be a different tolerance at
every world scale, and one loose enough to catch the tie also picks edges the ray genuinely
misses — which sends the walk into the wrong triangle and answers *clear* across a hole. A
vertex fan is unnecessary: under a consistent nudge every crossing after the first is strict.

**Which way to nudge is the whole of it.** Nudging always to the same side means a start triangle
lying wholly on the ray's left is never entered by the perturbed ray — and along a tile boundary
exactly one of the two triangles sharing it is that triangle. The rule is "left unless some
vertex is strictly right, or none is strictly left", which nudges *into* the start triangle. A
triangle with a vertex strictly on each side is entered either way, so no non-degenerate case
moves. The guard on the sign is `RaycastSeesAcrossAnOpenFloor`, whose diagonal runs along the
cell diagonals and is the same degeneracy; inverting the rule turns it red.

Only one of the exit-edge search's two comparisons ties in this case. The other,
`triArea2(a, b, to) >= 0.0f`, stays exact: a tie there means `to` is on the edge's line, which
either makes the separation test reject the edge anyway or has already been answered by the
containment test. Relaxing both — the epsilon reading — is what breaks the walk.

`findPathNav` therefore ends with a **corners-only pass**: drop any waypoint whose perpendicular
distance from the segment joining its kept neighbour to its successor is under 1e-4 m. It is
measured in 3D, not in the ground plane, so a ramp's crest — collinear seen from above, a corner
seen from the side — survives. Removing a point that lies on the segment moves the polyline by
at most the tolerance, far under any clearance `agentRadius` bought, so this is a shortening
that cannot cross a wall. It also collapsed a genuine detour: a path that backtracked 5.657 m
where the straight line is 4.243 m.

Fixing the ray removed about a third of what the pass was cleaning up and **did not make it
dead**, which was measured rather than assumed: over 6852 paths on 400 pseudorandom worlds — 4 to
12 cells square, open planes and scattered-wall mazes, XZ and XY, radius 0 and 0.3, endpoints on
cell centres, edge midpoints and grid corners — 274 paths still lose a waypoint to it, against
400 before. The unit suite alone drops to **zero**, so the suite by itself would have licensed
deleting it.

**Handedness is watched by a U, not by an L.** The check that a portal's left and right are not
swapped has to be a corridor with **two** turns: a single-turn L reconstructs the same waypoints
from a mirrored funnel during smoothing, so swapping `l` and `r` in the portal construction left
the entire suite green. `AUShapedCorridorInXYTurnsInsideBothOfItsCorners` is the test that
fails — and it is the only one that does — giving five waypoints on the outer wall and 17.56 m
walked against 16.045 m correct.

`freedom` is authored as `"all"` or `"plane2d"` in the collider extras, read by the same
`readEnum` every other spelling in that schema goes through. A static body ignores it — Jolt
gives one no motion properties to hold it — so nothing refuses the combination.

### Contacts

`PhysicsWorld::contacts()` returns what the last `step()` found, as a span of `Contact`
records — the two bodies, where they met, the normal out of `a` toward `b`, and the impulse.
A game reads it after stepping; it is cleared at the top of the next step.

**It is a recorded list rather than a callback, and that is the whole design.** Jolt invokes
a `ContactListener` from inside the solver, on its own threads, while the body lock is held —
so anything a game did there would run at a moment when it may not create a body, destroy
one, or touch a system that does. Recording during the step and draining after it costs a
few flops and a `push_back` per contact, and hands the game a list at a point where every
engine call is legal again.

**A contact naming a destroyed body is the case worth stating.** It cannot happen, and not
by luck: the handle's generation moves the moment `destroy` is called, and the slot only
reaches the free list at `reclaim()`, which runs at the top of the *next* step — the same
place the contact list is cleared. So every handle in the list is still resolvable for
exactly as long as the list exists. This is C1's deferred reclaim paying for something it
was not built for.

The pair is canonicalised — lower slot first — and the list ordered, so a contact between
the same two bodies is the same record whichever order the solver happened to report it in.
That is what lets a game key a cooldown on a pair without discovering the key has two
spellings.

### One-shot sounds

`AudioEngine::playAt(desc, position)` starts a sound at a point and forgets it: `loop` is
forced off and `autoplay` on, and `update()` retires the voice when it finishes. Without it,
every sound a contact wanted to fire was a source the game had to create, own and destroy —
which is bookkeeping for something whose entire lifetime is a quarter of a second, and it is
why the contact stream and this landed together rather than separately.

Voices retired this way come out of the same budget as any other, so a game that fires one
per contact in a pile-up is bounded by the same limit rather than by a new one.

### Cloth

`engine/scene/Cloth.{h,cpp}` and `PhysicsWorld::createCloth`. A mesh whose **name** begins
`FABRIC_`, carrying a per-vertex `_PIN_WEIGHT` float attribute — 1 pinned, 0 free — becomes
a Jolt soft body at load. Its solved vertices land in the buffer `skinning.comp` writes, so
it draws, casts and reflects through the passes that already read that buffer, with no new
pass and no new binding rule.

**A name prefix and a vertex attribute, and it is the one authoring convention here that is
not an `extras` key.** The four schemas in the table above are per-node dictionaries and a
pin weight is per vertex; `extras` has nowhere to put one. So cloth authors through the
geometry instead, the loader's only name-driven dispatch, and there is deliberately no
`substrate_cloth` key beside it — carrying both is how two vocabularies start disagreeing.
Both strings and the `>= 0.999` pin threshold are spelled once in `Cloth.h` and once in
`scripts/check_pins.py`, which validates an exported file from the other side of the
exporter, and `ClothTests.cpp` asserts the two spellings against each other.

**The solve runs inside `PhysicsWorld::step`, because it is not a separate solve.** A soft
body is added to the same `PhysicsSystem` every rigid body is in, so `PhysicsSystem::Update`
steps it and it collides with everything `Collider.h` can author for nothing. That inherits
the determinism argument above unchanged — one job system, a fixed step, bodies created in
the order the scene declared — and it is why cloth is not a bake: §9 forbids a run writing
what a later run reads, and a baked cloth could only be a canned animation that no longer
collides with anything.

Three solver settings, all Jolt's own and all taken rather than defaulted: 10 iterations,
`mLinearDamping` 2.0, and **bend constraints disabled**. The third is the one worth stating,
because the obvious reading of "stiff everywhere" is a bend compliance of zero and that turns
a sheet into a rigid plate — over-constrained, unsatisfiable by a Gauss-Seidel solver, and it
twitched two millimetres a step forever. Long-range attachment constraints (`GeodesicDistance`)
cap how far a vertex may get from its nearest pin along the fabric, which is the envelope
`tests/ClothTests.cpp` asserts, enforced by the solver rather than after it.

**A glTF mesh is not a simulation mesh.** Vertices are welded by quantised position before
the body is built, because an exporter duplicates a vertex wherever a UV seam or a smoothing
split needs two normals at one place — and a soft body built from those has two unconnected
particles at one point, so the fabric tears along every seam on the first step. The weld is a
grid rather than a radius so the answer cannot depend on which vertex was seen first, and the
welded numbering is first-sight order, so it is a function of the file and not of a hash
table.

Positions come back on the CPU once per **frame**, not per step: a frame runs zero to four
steps and only the last pose is drawn, and the normal recompute is the expensive half.
Normals are area-weighted from the solved positions; tangents are re-orthogonalised against
the **rest** tangent rather than the previous frame's, because Gram-Schmidt folded over its
own output makes the shading a function of the frame rate.

Stated restrictions, all in [limitations.md](limitations.md): the node transform is baked in
at load and ignored thereafter, so a `FABRIC_` mesh cannot be moved by animating its parent;
cloth inherits the deformed instance's infinite bounds, so no frustum culling and no LOD; it
is not appendable; and there is no self-collision, wind or tearing.


## The decision layer

`engine/ai/Planner.h` (C24). A world state of named boolean properties, actions carrying
prerequisites, effects and a cost, and an A\* over states that returns the cheapest ordered
sequence reaching a goal.

**Why it does not choose clips, and this is the mistake the design exists to prevent.**
`AnimationStateMachine` decides what pose; its parameters are continuous and `speed` at 0.42
— most of the way from walk to run — is a value a planner has no way to express. The planner
decides what to *do*; its state is boolean and its output is a sequence. They meet at an
intent: the planner decides "walk to the pot", the character controller pursues it, and the
machine blends whatever gait that produces. A planner asked instead to choose a clip per
frame would be a search run sixty times a second over a question with no prerequisites and no
sequence, and it would cost the cross-fades to do it.

**A planner rather than a third state machine**, and the distinction is the whole reason the
layer exists. A machine needs every route spelled out: to reach `attack` from `unarmed`
somebody authors `unarmed -> draw -> attack`, and authors it again for every state `draw`
might be entered from. A planner is given `attack` and derives the route, so an action added
later is reachable from everything that satisfies it without a transition being edited. That
is worth having in proportion to how many actions exist, which is the argument for it in an
engine meant for a substantial game rather than for the demo.

### A state is two words

```cpp
struct WorldState { uint64_t known; uint64_t value; };
```

Sixty-four properties, and **two masks rather than one**: "the door is shut" and "I have no
opinion about the door" are different claims, and a single bitmask cannot tell them apart. An
action's prerequisites are the second kind almost everywhere — most actions care about two
properties out of forty — and so is a goal. `satisfies` is four instructions and `after` is
three, so the search never allocates a state and comparison is not a loop.

That shape is also the answer to the two things the row expected to change on the way in.
There is **no `shared_ptr` per action and no `std::function` per action body**: an `Action` is
a name, two states and a cost, because what the planner needs is the *contract* and running
the thing is the caller's job at the far end of a plan. And there is **no state builder with
a getter and setter lambda per field** — that pattern exists to work around the absence of
reflection, and a flat name table indexed by bit is what a codebase that prefers a table to a
builder does instead.

### Re-planned on event, and what that costs

`Agent` holds the goal, the plan and a cursor. `advance` walks the cursor past steps the world
already satisfies — however they came to be satisfied, which is what lets a plan survive
somebody else opening the door — and searches only when the plan ran out or the current step's
prerequisites went away.

**The order of those two is load-bearing.** An effect is precisely a thing that makes its own
action's prerequisites false, so judging the plan's validity before moving the cursor re-plans
on every step of every *successful* plan: `draw` requires `unarmed`, drawing makes the
character armed, and the cursor still points at `draw`.

Measured on the demo, release, one goal handed over and followed to completion:

| | ms |
|---|---|
| `advance` on a frame that did not re-plan | 0.00027 |
| the frame that searched | 0.00739 |

The search allocates — a node vector, a visited map, an open list — and that is the one place
this layer departs from "no allocation per frame". It is defensible because it is not per
frame: 7.4 microseconds, once, on the frame a goal changed or a plan broke, against a 2 ms
frame. A caller that re-planned every frame would be misusing it, and the measurement is here
so that claim is a number rather than an assertion.

### What the demo shows

`G` hands the character a goal — a carried torch — and nothing is pressed after it. The
planner derives `walk to the torch > pick up the torch`, which nobody authored: `walk` has no
prerequisites and does not know it is the first step of anything, and it is in the plan
because `pick up` needs what it produces. The plan goes in the log, which is what makes this
evidence rather than a library with a test suite.

## The scene tree

`engine/scene/Scene.h`. Nodes with a parent, a local TRS, a cached world transform and one
flat attachment record — an instance, a body or character, a sound, a light index, an
emitter index. It is what a game says *this follows that* with, and what the engine now
uses to say it too.

**It replaced four hand-written loops.** A body drove an instance through
`Engine::DrivenInstance`, a body drove a sound through `DrivenSource`, an animated node
drove an emitter, and another drove a sound — four ways of saying the same thing, none of
which a game could reach. One node says it once, and a light or an emitter on a node now
follows it for free.

### Components (C42)

A node also carries **components of any type**, including types the engine has never heard
of:

```cpp
struct Health { int current = 100; };
scene.add<Health>(fighter, {100});
if (Health* h = scene.get<Health>(fighter)) h->current -= 10;
scene.each<Health>([](NodeId, Health& h) { /* ... */ });
```

`Attachments` could not be this. It is six typed handle fields, so a node holds exactly one
instance, one body and one sound, and nothing a game defines — which meant a game's own
per-node data lived in a parallel array with a `NodeId` in it. `game/battle_arena`'s `Fighter`
was that pattern written out, and is now a component on the node it describes.

**The engine's own six are deliberately not components.** The per-frame sweep reads `attached`
for every node in the scene; a hash lookup per node per kind is a real cost on the hottest walk
in `scene/`, bought for API symmetry alone. They keep their dense fields and their `attach*`
verbs. What the store is for is everything else, which includes every type a game defines.

Storage is one `unordered_map<slot, T>` per type, held type-erased in a vector indexed by a
per-type id. **No base class and no virtual**: `engine/` defines exactly three base classes and
this is not a fourth, so the erasure is a `shared_ptr<void>` — which already carries the right
deleter — plus one function pointer for the one operation that has to run without knowing `T`.
Components are erased with their node and with its whole subtree, before the slot reaches the
free list: a component left behind would be handed to whatever is created into that slot next,
which is the aliasing generations exist to stop one container along.

One value per type per node; a second `add` replaces. A node wanting two of something holds a
component with a container in it, or two child nodes. `each` walks a hash map and says so —
anything whose output depends on order sorts what it collects.

**Slots never move**, because a `NodeId` is a slot plus a generation, so a stale handle is
detectable rather than a silent alias. That rules out keeping the array in
parent-before-child order, so a separate `order` array holds a topological ordering,
rebuilt only on a structural change — create, destroy, reparent — and walked linearly by
the per-frame sweep. The tree is threaded through `firstChild`/`nextSibling`/`prevSibling`
indices rather than searched: a scene of ten thousand nodes makes "find my children" a
ten-thousand-entry scan, and the sort asks it once per node.

**Which way a transform flows** is the one thing worth knowing before using it:

| Attachment | Direction | Why |
|---|---|---|
| instance, light, sound, emitter | down, from the node | The node is where the thing is |
| dynamic body, character | up, into the node | The solver owns it; the node reports it |
| kinematic body | down | Kinematic means "moved by something else", and this is that |

A node a body drives takes the solver's transform **exactly**, with no decomposition on the
way in — that is what keeps a physics scene rendering the image it rendered before there
was a tree. Its local position, rotation and scale are not written back, because deriving
them would be that round trip.

Every node's world transform is recomputed each frame; only a *dirty* one pushes. A write
is an upload — `InstanceTable::revision` is what the renderer compares per frame-in-flight
buffer — so a static scene must touch nothing.

`setParent` keeps the world transform, which is what makes picking an object up not
teleport it; `setParentKeepLocal` does the opposite, for an attach-to-socket. The keep-world
form decomposes `inverse(newParentWorld) * world` into TRS, which is exact for any chain
whose scales are uniform and lossy for the shear a non-uniform parent scale introduces.
Stated rather than hidden.

**It is not an ECS and holds no gameplay fields.** A registry adopted later sits beside it
holding a `NodeId` as a component — the same argument the instance table already makes
about why entt cannot own it.

### Reading the tree from outside

G3 built every query a node's *owner* needs and none a *reader* does. `order()` hands out
slots and a slot cannot be passed back to any call on the class, so nothing outside `Scene`
could answer "which nodes are there" at all — a panel, a listing or a console had no way in.

Four calls close that, and none of them adds a structure: `idAt(slot)` is the conversion
back to a handle, spelled the way `InstanceTable::idAt` already is; `firstRoot()`,
`firstChild(id)` and `nextSibling(id)` expose the sibling list `resort` was already walking.
A depth-first listing — a child directly under its parent, which is the only order a
hierarchy reads in — therefore costs one visit per node. `order()` stays breadth-first and
stays the sweep's: it lists every root before any child, which is right for the sweep and
unreadable as a tree.

**`idAt` on a dead slot yields an invalid handle, not the generation the slot is holding.**
Handing out the latter would compare equal to the handle the *next* `create` in that slot
issues, which is the aliasing generations exist to stop — reached through the one call a
listing makes on every node it prints.

`structureRevision()` is the counter beside them, and the name is the specification: it
moves on create, destroy, reparent and clear, and **not** when a node moves. A caption is a
name and an attachment record, so a counter that ticked with a transform would rebuild every
string every frame of an animated scene. It starts at 1, so a holder's `0` means "never
built" — the same promise `InstanceTable::revision` makes.

### Turning a node toward a heading

`Scene::turnToward(node, direction, rate, dt, floor, forwardOffset)` (C30) — the shortest-arc
slew about +Y, at most `rate * dt` of it a step. It reads the yaw out of the node it is about
to write, so **the tree is the state and there is deliberately nowhere else to keep it**: the
version this replaced kept a `facingYaw` in the demo and wrote it to several nodes, which made
the angle and the tree two copies of one fact — and the copy `locomotion.sh` asserts against is
the tree's.

Three things it is not. It is not keyed on a character: a vehicle facing its velocity, a
turret facing a target, a boat, an aircraft and an agent on a navmesh all want the identical
turn, and the demo's version was the third caller and the first that could not have it. It is
not a per-node property the sweep applies — *when* a thing turns and *what toward* is a game's
decision, and a tree that turned nodes by itself would own it. And it does not normalise
`direction`, because the length is the speed: `floor` is what separates a heading from
rounding, below which the node keeps the yaw it had rather than jittering about noise.

`forwardOffset` is where the model's own forward sits relative to +Z and is subtracted, so a
rig authored looking down +X passes `pi/2` instead of a sign being flipped somewhere in the
caller. The angle comes back folded into (-π, π]: the rotation is the same either way, but a
turn *through* the seam is exactly what produces an unbounded angle in a caller that keeps one.

### The character controller

`CharacterVirtual`, not a rigid body — a capsule pushed by a solver slides down slopes,
tips over and cannot step up a stair. Authored as a fourth `motion` value rather than
through a schema of its own, because a character is placed, sized, named and budgeted
exactly like every other collider.

Where the scene also has a skin, the controller's horizontal speed drives the animation
state machine's `speed` parameter instead of a frame-index triangle wave — a state machine
driven by where the character actually is. `characterOnGround` drives the second parameter
the same way, with one caveat a caller has to know: **a `CharacterVirtual` has no ground
state until it has been swept**, and `Game::fixedUpdate` runs ahead of `simulate`, so on
step zero it answers *in the air*. A game driving locomotion from it starts at step one.

**Speed and heading are one measurement, and both are relative to the ground.** A character
carried by a kinematic platform is not walking, so `characterSpeed` takes the ground's own
velocity out before reporting — a rider who pressed nothing reads as still and stays in
`idle`. `characterVelocity` is that same quantity before it was collapsed to a length, and it
exists because the obvious source for a *direction* is wrong in exactly the same way:
differencing `characterTransform` across a frame is the platform's travel, so a game turning
a mesh to face where its character is going turns it to face where the floor is going. The
demo did, and its character pirouetted with the sliding platform for as long as anyone stood
on it.

**Both come out of the swept displacement, and that is not the same as Jolt's velocity.**
`CharacterVirtual::Update` slides the shape through the world and leaves `mLinearVelocity`
exactly as the caller set it, so an accessor built on `GetLinearVelocity` reports the request
back — a character flat against a wall reads as running at `moveSpeed`, and the locomotion
machine plays a full-speed run on the spot for as long as the key is held. It read that way
until `bug-a-blocked-character-reports-the-speed-it-asked-for`, and no arm anywhere pressed a
character into anything, so nothing could have said so. Taking the position before the sweep
and differencing it afterwards is what makes the three cases the surface promises true at
once: a wall takes away the component into it, a ramp is climbed at the speed the ramp
allows, and a stair arrives with whatever `WalkStairs` actually moved. The displacement is
**not** fed back into the motion model's ramp — that integrator's state is the request, so a
character leaning on a wall still has its speed the moment the wall stops being in the way.

**`setCharacterInput` takes a request, and the vector's length is part of the request.** The
controller multiplies it by the character's `moveSpeed` without normalising, so a
half-length direction is half the speed — which is what makes an analogue stick analogue,
and what makes a walk state reachable from a keyboard at all through a modifier that scales
the vector rather than a second speed constant.

**The ground is a body and a normal, not only a yes** (C30). `characterGround` says whether
there is something under the character; `characterGroundBody` says *what*, and
`characterGroundNormal` says which way it faces. A moving platform, a conveyor and an enemy's
head are one answer to the first and three to the second, and a slope lean, a ski and a wall
run all start at the third. Both are falsy-or-up in the air rather than stale: Jolt keeps the
last ground it found, and a normal from a face the character has left is worse than no normal.

**`setCharacterTransform` is the character's `setBodyTransform`** (C29) — a respawn, a
checkpoint, a portal, a level transition and a loaded save, none of which the surface could
express while a character was placed once from its `ColliderDesc` and thereafter only walked.
It teleports: the velocity is zeroed, both interpolation snapshots are written, and the
contacts are refreshed at the new position rather than left to the next sweep. That last part
is the only one that is not obvious, and it is the one with teeth. `step()` reads the ground
state **before** it sweeps, so a character teleported off a floor reports standing on it for
one more step — a whole coyote window, and enough to spend a jump on ground that is a hundred
metres away. The two-armed test that pins it fails on that arm alone with the refresh removed.

#### The motion model

**The horizontal velocity ramps toward the request rather than being assigned it** (C20).
Until it did, the request's magnitude was the *only* thing between a character standing
still and one at top speed — so `characterSpeed` read 0 or `moveSpeed` and nothing else, and
a locomotion machine with a walk band between the two had a state nothing could enter. It
shipped that way and no check in the tree could have said so.

Four `ColliderDesc` rows carry it, and each replaces a constant that used to be compiled in:

| Row | Default | What it decides |
|---|---|---|
| `acceleration` | 10 m/s² | Rate toward a *faster* request |
| `deceleration` | 40 m/s² | Rate toward a slower one, including a stop |
| `airControl` | 0.35 | The fraction of both that applies while not standing |
| `stepHeight` | 0.35 m | Jolt's `mWalkStairsStepUp`, with the step-down at 1.25× it |

Three things about the shape of it:

- **Which rate applies is decided by speed, not by whether the request is zero.** Turning
  around at full tilt is a deceleration through the turn and an acceleration out of it, and
  comparing the request's length against the current one gets that without a special case.
- **The ramp is relative to what the character is standing on**, so it is a ramp against the
  moving platform rather than against the world.
- **A large enough pair reproduces the assignment exactly.** That is the property that makes
  this a *number* rather than a behaviour change forced on every game: the comment the old
  code carried said "a game wanting momentum instead changes this line", and a game changing
  a line in `engine/` is the thing the engine/game split exists to prevent. A game wanting
  the old feel authors `acceleration: 1e6` and gets it, which the unit suite pins.

`airControl` below one is what makes a jump carry the speed it launched with. Above the
ground the same two rates apply scaled by it, so a character steers in the air and does not
stop dead in it.

**`stepHeight` was parsed, documented and read by nothing at all** until this row. The step
handed every character a default `ExtendedUpdateSettings`, whose `mWalkStairsStepUp` of 0.4 m
and `mStickToFloorStepDown` of −0.5 m are absolute metres sitting two hundred lines from an
`mSupportingVolume` the same header *does* scale to the capsule's radius. So the authorable
row was dead and the live one was wrong for any character not roughly human-sized. The pair's
ratio is what matters and is kept: the step-down has to reach further than the step-up, or a
character that walked up one stair hovers off the next.

#### A jump that forgives

Two windows, **both counted in fixed steps and never in frames or seconds**:

| Row | Default | What it holds |
|---|---|---|
| `jumpBufferSteps` | 10 (⅙ s) | A press that arrived before the character could act on it |
| `coyoteSteps` | 6 (⅒ s) | The launch, for that many steps after the ground went away |

**Neither is a feel preference.** Without a buffer, whether a press reaches the solver at all
is a function of the frame rate: a fixed-step solver sampled by a variable-rate game drops
the presses that land in the gaps. `setCharacterInput` latching the edge was half of that
fix; the step cleared `c.jump` unconditionally afterwards, so a press one step early was
still eaten. The window is the other half.

The units are the other half again. A window in *seconds* has to be divided by the step to be
used, and G12 already measured that sixty additions of `1.0f/60.0f` land just under a second —
so a window compared against an accumulator would be off by a step at random. These are
integers and this loop is the only thing that advances them.

The buffer is the longer of the two on purpose: it only ever **delays** a jump the player
asked for, where the coyote window **grants** one the world did not offer. At `moveSpeed`
3.2 m/s, ⅒ s of coyote is 32 cm of overhang — under half a stride, so it cannot be used to
cross a gap.

**The window a launch used is spent, and refilled only by standing again.** Without that, a
jump key held down across a ledge buys a second launch out of the air, which is a double jump
and C20 declined one. The ground state lags the launch by a sweep, so the step *after* a jump
still reports standing; `Character::launched` is what stops that step refilling what the jump
just spent, and it is the same flag `characterJumped` reports.

**A game cannot derive whether a jump happened and must not try.** The demo used to fire its
jump animation on `pressed(jump) && characterOnGround(...)` — the controller's own decision,
re-derived from outside, and correct only for as long as the two could not disagree. With a
window in the way they do: a press inside the coyote window launches with no ground under it,
and a press inside the buffer launches a step or two after the frame it arrived on.
`characterJumped` reports what the solver did, one step after it did it.

#### Ground too steep to stand on

`characterGround` answers `InAir`, `OnGround` or `Sliding`, and the third is why it exists.
A face past the collider's `maxSlopeAngle` is not ground and is not mid-air either; reported
as the first a character could walk up a cliff, and reported as the second — which is what a
bool did — a game plays a fall clip for something visibly in contact with a surface, and a
jump is refused for a reason nothing can see.

`Sliding` also covers Jolt's `NotSupported`: touching a body that cannot hold the character
up. It is the same answer to the same question, and splitting the two would put a fourth
value in every caller's switch to say something no game acts on differently.

**The solver's behaviour on a steep face did not change and did not need to.** Steep ground
takes the airborne branch, so gravity accumulates and the character slides down it, with
`airControl` deciding how much it can steer while it does. `characterOnGround` still means
*standing* and still refuses the jump. What this row added is that the case is now visible.

#### The camera is data; the controller is a game's opt-in

`scene::Camera` is a pose, a projection and `frameBounds`, plus **three empty-defaulted
virtuals** — `activate`, `deactivate`, `update` (G18). It used to be one specific camera, its own
first line reading *"Orbit camera with WASD fly controls"*, installed unconditionally: every game
got nine `Camera.*` actions including W, A, S and D, listed in the rebind menu of a game with no
flycam, and the pointer grab was wired to `Camera::orbitAction()` for a camera the game might not
be using.

**The base doubles as the null camera.** "Looks at the scene and takes no input" is
simultaneously its definition and what a null object has to be, so there is no `NullCamera`, and
`Engine::camera()` can never be null — `drawFrame`, the audio listener, `--camera` reproduction
and `frameBounds` gain no branch. `Engine::setCamera(Camera*)` is the only door and is
deactivate-then-activate, so the pair cannot be mis-sequenced; `nullptr` installs the engine's own
base instance. The pointer is **non-owning and never deleted**.

**Bindings arrive on activation rather than construction**, which is what lets a game hold every
camera it will use and pay input surface only for the active one — two held cameras declaring
`Camera.Forward` from their constructors would be a conflict `InputMap::conflicts()` would be
right to report. `deactivate` **retires** rather than clears (C36), because clearing leaves a row
a player can still see and rebind for a camera that is not running.

`scene::FlyCamera` in `engine/scene/CameraControllers.h` keeps the behaviour unchanged, and stays
in `engine/` because it is the only one of the four camera kinds that already exists as debugged
engine code: `./run.sh` with no game opens Sponza and flying is how you look at it, and `--camera`
reproduction assumes somebody flew somewhere first. The objection — that the engine has WASD
opinions again — does not survive declare-on-activate. **The defect was unconditional
installation, not existence.**

**Three settings rows moved with it, and that was the discovery.** `moveSpeedScale`,
`orbitSensitivity` and `zoomStep` are the *controller's*, so with only a `Camera&` in hand the
engine cannot apply them — `FlyCamera::applySettings(settingsTable())` is why a game's opt-in is
two lines rather than one. `camera.fovDegrees` stays the engine's. `moveSpeed` left the base
entirely: it was private and read only by the old `update`, and the flycam now derives
`max(distance, 0.25) * moveSpeedScale`, so speed tracks the dolly. On Sponza that is 7.44 m/s
against the old ~4.66, and nothing in the golden, readback or locomotion suites measures it.

**Framing had to move to make this behaviour-neutral.** The engine framed the camera during
`Engine::init`, but a game installs its camera in `Game::init` and `setCamera` copies no pose — so
a demo flycam would have started at focus 0, distance 5, and every golden would have changed.
Framing, fov and `--camera` are now `Engine::applyCameraConfig()`, run immediately before
`applyBindings()`. `Game::init` therefore cannot read a framed camera.

**Four controllers, and each has working defaults rather than a hook** (C37).
`FirstPersonCamera` sits the eye at the focus and grabs the pointer for as long as it is the
camera — one line in `activate`, one in `deactivate`, and the case a declarative "this action
grabs while held" flag could not have expressed, which is why C36 made grabbing a verb. It
declares `Camera.Look` and **no movement actions at all**: in a first-person game the *character*
moves under the solver, and a camera that also walked would fight the controller and need its own
collision. `ThirdPersonCamera` holds a `scene::NodeId` and **reads its world transform itself
during `update`**, because `update` runs before `Game::frameUpdate` so a position pushed in by the
game would always be one frame stale — and the ordering is not the thing to change.
`IsometricCamera` is the first real consumer of `Projection::Orthographic`, driving `orthoHeight`
from scroll rather than `distance` (the thing a controller written for perspective gets wrong),
with pitch fixed at 35.264° — the angle where a unit cube's three visible faces project to equal
areas — and yaw re-derived with `lround(yaw / (π/2))` at the moment of a press so the snap is
exact across the seam.

**No spring arm, no collision, and that is stated rather than discovered.** Pulling a
third-person camera in when a wall comes between it and its target needs physics queries and a
policy for when there is nowhere to go. `IsometricCamera` likewise does not replace
`pixelPerfectCamera`: that is a pixel-exact 2D setup, one world unit per texel, and conflating
the two produces a camera bad at both.

**One control scheme is live at a time, and the demo had to make that true.** The premise that a
game installing `ThirdPersonCamera` never activates the flycam's WASD alongside the player's is
false on its own — toggling to the flycam puts `Camera.Forward` on W while `Player.Forward` is
also there. The demo stops driving its character while flying, and *that* is what let
`PlayerActions::declare` finally lose the five-key rebinding dance it carried for four stages.

**A view holds a camera, not a copy of one.** Giving the base a vtable makes
`entry.camera = someFlyCamera;` compile as base copy-assignment and silently drop the derived
half — a camera that does nothing, with no diagnostic. `ViewTable::Entry` therefore carries a
by-value slot *and* a non-owning `installed`, with `active()` choosing; `camera(ViewId)` keeps its
exact signature. The default is **per entry rather than one shared instance**, because writing
`focus` through one uninstalled view's handle would otherwise move every other view — and it
fixes a live bug, since `camera(id)` used to return `&entries[s].camera` and a later `create()`
can reallocate the vector out from under it. **The engine calls neither `activate` nor `update` on
a view camera**: there is one `InputMap`, and a game that wants a view camera to read input
updates it in `frameUpdate` with whatever map it likes.

#### Where the movement basis comes from, and which way the coupling runs

**A third-person character walks where the camera points, and that is a game's arithmetic
rather than an engine capability** (G13). Every piece of it is already public — `Camera`'s
pose, the node the game holds for its player, `setCharacterInput` — so the demo joins them up in about six
lines and the engine keeps the free-fly orbit `./run.sh` with no game uses to look at Sponza.
What the engine contributes is one ordering guarantee and one property of the scene tree.

**The basis is `Camera::forward()`, flattened, and there is exactly one expression of it.**
The demo used to rebuild a heading from `camera.yaw` as `(sin y, 0, -cos y)` while the camera
built `(cos p·sin y, sin p, cos p·cos y)`, and the two agree only where `cos y` is zero.
`frameBounds` picks yaw = π/2 when X is the longer horizontal axis, which the showcase scene
is, so W walked the character *toward* the camera at every other yaw and nothing noticed. The
rule this leaves behind is the general one: **derive the basis from the camera, never restate
it**, and take screen-right from the same cross product `Camera::update` takes rather than
writing the perpendicular out.

**The camera's own update runs before `Game::frameUpdate`, and that is why the two can be
coupled at all.** A game resolving "forward" gets this frame's yaw rather than the last one's,
and a game writing `focus` writes it after `Camera::update` has read it — which turns a drag
into an orbit *about the character* with no mode flag and nothing to switch off.

**The rig writes `focus` and never `yaw`.** That is the whole answer to the chase problem a
follow camera invites: a camera that also aimed itself from the character's heading would be
two integrators feeding each other, and the usual fix is to damp one of them enough that the
drift is slow. Here the coupling is one-way by construction — yaw comes from the mouse and
from nothing else, so the basis does not depend on the motion it produces. The focus is
snapped rather than lagged, because `frameUpdate` is handed a wall-clock delta even under a
locked clock and a smoothed focus would make the drawn frame a function of how fast the
machine ran; the smoothness a lag reaches for is already there in `characterTransform`'s
interpolation between fixed steps.

#### Facing is the game's rotation, on a node the solver does not own

`characterTransform` returns `CharacterVirtual::GetRotation()` and nothing in the engine ever
calls `SetRotation`, so a character keeps whatever heading its rig was authored with and
strafes everywhere. Turning it is a rotation the *game* composes into the tree, and the node
it goes on is the one thing here that is not obvious:

- **Not the node the character drives.** The sweep writes the solver's matrix into that node's
  world transform verbatim and `continue`s, so a local rotation set on it is discarded every
  step and silently does nothing.
- **Its child.** The loader already makes one per mesh a body drives, carrying the rest
  transform as an `instanceOffset` matrix. A child's TRS is composed onto a parent whose world
  transform is already final, so the heading is a rotation about the character's own origin
  and **no matrix is decomposed anywhere** — which is the property `instanceOffset` exists to
  protect and the reason a facing must not round-trip through one.

A `setCharacterFacing` on `PhysicsWorld` would be the other shape and is the wrong one here: a
setter is a capability, it belongs to whichever row wants the solver to know a heading, and
adding it for a mesh that needs turning blurs the line between what the solver owns and what
the game does.

The angle is slewed toward where the character **actually went** — the per-step displacement
the solver produced — rather than toward what it was asked for. The two differ through every
ramp the motion model adds and through anything the character slid along, and facing the
request is the same mistake as animating a jump off the keypress.

### Both interpolation snapshots, and why they are one array

`previous` and `current` are one `PhysicsState` array each, laid out **bodies first and
characters after them**, so one pair serves both and the interpolation is written once.
Every accessor bounds-checks the current snapshot and then indexes both, and each pair is
kept the same length by `snapshot()`. A slot that has just appeared starts equal to `current`
— one step without interpolation, on the step it was created, which is the only honest answer
for an object that had no earlier state.

Bodies and characters have **a pair each** rather than sharing one. They shared one until C40,
laid out bodies first and characters after, so a body created after `finalize()` did not merely
append — it shifted every character's slot along, and `characterTransform` read off the end and
answered identity until the next `snapshot()` repaired the layout. A game building its own
props in `Game::init` saw its character at the origin for exactly one step. Splitting the
arrays makes a slot's index independent of how many of the other kind exist, which is what the
growing world needs: creating a body at runtime is now ordinary rather than rare.

### Debug draw

One `LINE_LIST` pipeline, **no depth test on purpose**: a collision shape is inside the
mesh it describes, so a depth-tested wireframe of a box collider on a box is either hidden
or z-fighting. The line data reaches the renderer as `renderer.debugLines`, a plain vertex
vector the application fills, so `gfx/` gains no dependency on physics. **0.0094 ms CPU
and 0.0041 ms GPU** for ~1,900 lines.

---

## Audio

`engine/scene/Audio.{h,cpp}` and `AudioSource.{h,cpp}`, miniaudio 0.11.25 compiled through
one translation unit exactly as VMA and stb are.

**Audio cannot change a frame, and that is checked rather than asserted**: frame 60 of the
audio test scene is byte-identical with a playback device open and three sources playing
as it is under `--no-audio`.

**Voices are held by pointer and that is load-bearing:** a `ma_sound` registers into the
node graph *by address*, so a vector that reallocated would leave the graph pointing at
freed memory and crash inside the mixer on the frame a scene declared one more sound than
the last.

**`voiceBudget` is the one budget with no allocation behind it** (C40). The voice list already
grows, so what the number bounds is *mixing cost* — a property of the machine rather than of
the game. It therefore doubles on demand rather than refusing, up to `AudioEngine::kMaxVoices`
(1024); at the ceiling `stealVoice()` retires the quietest one-shot, preferring a one-shot over
a placed source and never taking a loop, because losing a footstep is momentary and losing a
loop is a hole in the mix that never fills back in. The only refusal left is a mixer whose
every voice is looping, which is a real condition rather than a number somebody guessed.

### The device-less mode

`backend: "null"` is miniaudio's `noDevice`, where the caller pulls the frames and
**everything else on the path is identical**. It bought three things: a unit suite that
asserts on real samples with no sound card, a resource manager with **zero** job threads
(so the suite is clean under TSan), and a fallback for a machine whose device will not
open that is not silence behind a different code path.

`--audio-null` is therefore not `--no-audio`, and `--help` says so.

### Streaming

One hosted free function, `audioShouldStream(load, seconds, threshold)`, holds the whole
rule. Streaming costs **0.96 us per voice per step**; decoding costs **375 KiB and ~0.25
ms of load per second of audio**.

**The honest reading is not "so decode everything".** A microsecond per voice is 0.006% of
a frame, so steady-state CPU is not what decides this. Memory is, growing without bound —
an 8.9-minute bed costs 196 MiB decoded, three times the whole default budget — and so is
*when* a sound can start. Short assets are fired on events and long ones are started once,
and **5 seconds is where those two populations separate**.

Sizes are stated in **f32 at the mix rate, not the file's**: a 16-bit WAV costs twice its
file size in memory.

**The budget is charged per asset, not per source.** miniaudio caches a decoded buffer
*per path*, so eight voices on one file cost one buffer. Charging each source separately
reports an order of magnitude more memory than the process holds — 169.9 MiB against an
actual growth of a twenty-first of that — and would refuse a scene with forty footsteps on
one file the memory it was never going to allocate. Peak RSS is what settles it, and it
agrees to within half a percent.

### Spatialisation, buses and occlusion

The listener is the camera by default, set once per **frame** rather than per step because it
is driven by input; world up rather than the camera's own, since rolling the view should not
roll the room. Source transforms are read at **alpha 1** rather than the frame's
interpolated alpha — interpolation exists so a *drawn* thing lands between two steps, and
nothing here is drawn.

#### More than one pair of ears

`AudioConfig::listeners` asks for up to four (C28), and `setListener(..., index)` places each.
Two is split screen; one that is not the camera is a first-person game putting the ears at the
character's head rather than at a camera that has moved for a cutscene, or a top-down game
whose camera hovers tens of metres up and would otherwise hear everything distant and unpanned.

**`GameSetup::audio.listenerFollowsCamera` is a switch rather than an ordering change.** The engine
writes listener 0 in `beginFrame`, *before* `Game::frameUpdate`, so a game writing its own
would have it overwritten before it was heard — and moving the engine's write after the game's
would overwrite the game's instead. Neither order is right for both, so a game says which of
the two owns the ears and the other one does not write.

**A spatial source is heard from the nearest listener, not the sum of them**, and that choice
had to be implemented rather than configured. `ma_engine_node_config_init` zeroes its config,
and zero is a *valid* listener index rather than the `MA_LISTENER_INDEX_CLOSEST` sentinel, so
miniaudio pins every sound to listener 0 for its whole life;
`ma_sound_set_pinned_listener_index` cannot undo it, because it refuses any index at or past
the listener count and the sentinel is 255, and `ma_sound_config` has no field for it. Measured
before the fix: a source one metre from listener 1 and thirty from listener 0 mixed at exactly
the thirty-metre level, so a second pair of ears was created, positioned, drawn and never heard
from. `AudioEngine::update` now picks the nearest listener per spatial voice and pins it, and
skips the loop entirely at one listener. Nearest rather than summed because summing makes a
room louder as players are added and doubles a sound both can hear — a source's loudness would
stop being a property of the scene.

**Occlusion is the same question asked once per listener.** The filter is one biquad on one
voice and there is no per-listener version of it, so a source is treated as occluded only when
*every* listener is behind something: muffling a sound the second player can see plainly is the
worse of the two mistakes, and it is the one a sweep reading listener 0 would make on every
frame of a split screen. The debug draw shows a line per listener per source for the same
reason — one line would draw an answer the sweep did not take.

A bus is a `ma_sound_group` with a name and a gain. What makes it more than a multiply is
`duckedBy`, naming another bus whose playing voices pull this one down over a stated
attack and release — **asymmetric on purpose and asymmetric in every mixing desk ever
built**. `duckAmount` defaults to 1.0, which is no ducking at all: a mixer that quietly
attenuated a bus because something else made a noise is help nobody asked for.

Occlusion is `PhysicsWorld::segmentBlocked` returning a **boolean** — the engine's first
*query* of the physics world rather than an instruction to it, and deliberately the
narrowest one that answers the question, because everything past the boolean is the first
half of an acoustic material system nobody asked for. `Engine` casts and calls
`setOccluded`; `AudioEngine` owns the filter and the slew, which keeps Jolt off
`Audio.cpp`'s include path.

The filter is inserted **at load or never** — re-plumbing a running graph on the frame a
wall first came between a source and the listener would be re-plumbing it on exactly the
frame that must not glitch. One slewed 0-to-1 state drives both a cutoff (20 kHz to 700
Hz) and a gain, because a wall passes low frequencies and occlusion that was only a volume
change sounds like a volume knob.

**The ray ignores the source's own body rather than trimming its ends.** A source
usually sits on the node that carries its collider, so a ray that is not told to skip that
body starts inside it and reports the source as permanently occluded by the object it is
bolted to. A fixed margin cannot answer this: the margin is a constant and the collider is
whatever size the scene made it, so anything larger swallows it. `sourceBody` therefore
names a body to ignore, and it includes **static** bodies — the case hardest to notice,
precisely because nothing about it ever moves.

**Cost:** three sources, the whole per-step audio block is **0.0017 ms** with a device,
because the mix runs on the driver's thread. Under `--audio-null` it is 0.0433 ms, since
that arm is doing the mixing too.

### Tapping the mix

`engine/core/AudioTap.{h,cpp}` — a lock-free single-producer ring, fed from miniaudio's
`ma_engine_config::onProcess`, the callback fired at the end of every
`ma_engine_read_pcm_frames`. One hook, so there is **no second mixing path**: the samples
recorded are the samples that reached the device, taken from the same call, and it works
identically with a device and without one. The game stays audible while it records.

The producer is the audio thread, which is filling a device period; anything it does that
can block is a glitch you can hear. So `write` takes no lock, allocates nothing and never
waits — and because there is exactly one writer and one reader, two atomics are enough.
`./test.sh tsan` is what says so, which is why the ring is in the hosted sources.

**The newest frames are dropped and counted**, which is the opposite of what a live
monitor would do. It feeds a file: a file wants the audio in order with any gap *stated*,
so `dropped()` is how many frames never made it in and the recorder replaces exactly that
many with silence. See [tooling.md](tooling.md#recording-a-session).

---

## Naming an asset

`engine/core/Resources.{h,cpp}` (hosted). **Not `engine/gfx/Resources.h`**, which is GPU
buffers, images and `Uploader`. One is memory on a device, the other a file on a disk;
they share a basename and nothing else, and rooted includes keep
`#include "core/Resources.h"` and `#include "gfx/Resources.h"` unambiguous.

There are two asset trees, and before this every path naming something in one of them
spelled out which tree and how deep — `game/demo/assets/character.gltf` in `substrate.json`,
`engine/assets/Sponza/glTF/Sponza.gltf` compiled into `Config`. A `res:/` name says what
the asset is and lets the lookup say where it lives:

```cpp
Resources("res:/character.gltf")           // -> <abs>/game/demo/assets/character.gltf
Resources("res:/Sponza/glTF/Sponza.gltf")  // -> <abs>/engine/assets/Sponza/glTF/Sponza.gltf
```

**The game's tree is searched first**, so a game can ship its own version of an engine
asset under the same name and get it while every other game still gets the engine's. That
is the rule `readShaderBinary` already follows for GLSL, and it is the same rule
deliberately: two trees with the game in front is now how this engine answers "where does
this come from".

Three properties are worth stating because each was a decision:

- **A path without the scheme is passed through** as an ordinary filesystem path. Every
  string that worked before this existed still works, which is what let the call sites move
  over one at a time and why `scripts/golden.sh` still names its four scenes directly —
  keeping the golden set a valid check on the change that introduced this.
- **A glTF's own `uri` entries are untouched.** Buffers and images are relative to the
  document, which is what the format specifies and what fastgltf and
  `GltfScene::decodeImage` already do. The result is absolute, so resolving the document
  anchors everything inside it — the `.bin`, all 69 images, the `.ktx2` beside each one,
  and a sound named in `extras` — for free.
- **An empty name stays empty**, and that is load-bearing rather than tidiness:
  `render.debugFont` defaults to `""` meaning "use the embedded bitmap font", and
  `Font::init` decides that by asking whether the path is empty. Absolute-ising `""` into
  the working directory would have it read a directory as a TTF. The unit suite pins it.

A miss logs the name and both roots and still returns a path, because which roots were
searched is the one thing a caller's "cannot open" cannot tell you, and the caller's own
error handling is better at the rest.

**The roots are absolute, baked at build time**, exactly as `SUBSTRATE_SHADER_DIR` is —
and for the reason the scheme exists at all. A root resolved against the working directory
would put *where* straight back, as "the directory you happened to be standing in", which
is the one thing a name is supposed to remove. So `res:/` resolves from any working
directory, the way shaders already did. A path *without* the scheme stays relative to the
working directory, because that is what a path means, and it is why `scripts/golden.sh`
naming its scenes directly still behaves exactly as it did.

The tradeoff is the one the shader paths already took: a source tree moved after the build
loses its assets. Better than a binary that works from one directory only.

**What this does not do:** it resolves names, and it owns nothing. No cache, no handle, no
lifetime — one path and one bool per instance. It is not the `ResourceManager`
[principles.md](principles.md) refuses, because that refusal is about owning `GpuImage`
lifetimes behind handles. And it covers assets, not `substrate.json`: the config file is
still found relative to the working directory, with `--config` for the rest.

---

## Input

`engine/core/Input.{h,cpp}` (hosted) and `InputGlfw.cpp` (not).

Actions carry a float in `[0,1]` and `held` is derived at 0.5, so `Camera.Forward` is
`W Pad.LeftY-` rather than two paths that happen to move the same camera — a stick at a
third of its travel moves at a third of the speed, and a key reads back as exactly 1.

**Resolved once per frame in `beginFrame`, never on demand**: "did this action fire" has
to be the same answer everywhere in the frame. 37 actions, and no key code appears in a
`case` label anywhere in the tree.

`Camera` and `BindingMenu` declare their own actions with their own defaults, because the
thing that consumes an action is what should name it.

**The press edge needs a flag; the release edge must not have one.** A key tapped and
released between two polls is invisible to a level test, and at 250 fps against a 1000 Hz
keyboard that gap is real. The symmetric release flag is not merely unnecessary but wrong:
releasing one of two keys bound to the same action, while the other is still down, is not
a release of the action.

Gamepad support is **bindings rather than a code path**: all sixteen joystick slots are
polled, each keeping its own `GamepadState`, so one pad, four and none are the same path.
Triggers are rescaled from GLFW's resting-at--1 convention so "not pressed" is zero on
every axis.

**An action resolves against a player, not against a device.** A `PlayerDevices` is a
keyboard flag and a bitmask of pad indices, and `InputMap` resolves every action once per
player into its own state — `value(id, player)`, `held`, `pressed`, `released`, all
defaulting to player 0. There is one player by default holding the keyboard and `kAllPads`,
which is the single-player case and the reason no existing caller names a player. Two
players are `setPlayerCount(2)` and two `setPlayerDevices` calls; nothing else in the map
changes, because the binding table is shared and only the state vector is per player.

The unit is the player rather than the pad index because a player is what owns a character:
one player can hold a pad and the keyboard at once, a pad can be handed to a different
player without rebinding anything, and a game that asked "which pad pressed this" would
have to re-answer it after every hot-plug. **The deadzone applies per pad before the pads
combine**, not after — an idle pad's stick drift biases a player holding more than one
otherwise, and the merge hides it because the largest magnitude wins.

**Only actions that differ from their declared default are serialised**, so a default that
moves in a later build still reaches anyone who never rebound it. A save re-parses the
file and swaps one object, so keys the reader never looked at survive.

**An action is retired, never erased, and an `ActionId` is why.** An id is a bare index into
`actions`, so removing a row would shift every id above it and silently repoint the `ActionId`
members a game is holding — a failure with no symptom until a keypress does the wrong thing.
`retire(id)` flips a `live` flag and **keeps `bindings` and `defaults`**. Since `declare` is
idempotent by name, re-declaring a dead row revives it with the same id and whatever the player
had rebound it to, so a control scheme that comes and goes does not cost anyone their bindings.

The consumers do not all answer the same way, and two of them are traps:

| Consumer | A dead row |
|---|---|
| `value` / `held` / `pressed` / `released` | zero, false; `retire` zeroes the per-player state so the frame it dies in already reads gone |
| `find(name)` | not found — nothing that reacts to input can see a dead row |
| `findDeclared(name)` | **found**; `applyBindings` only, so a config's rebind reaches a row retired before it ran |

**A rebind for a row nothing has declared yet is parked, not dropped.** Declare-on-activate means
a game holding three cameras and installing one has two thirds of its camera bindings undeclared
when `applyBindings` runs, every time — so `applyBindings` keeps what it could not resolve, and
`declare` takes and **erases** a matching parked row when the action first appears. Three
properties make that safe: the row is consumed rather than kept, so reviving a retired action does
not replay the config file over an edit the player made in the meantime; `applyBindings` *replaces*
the parked set rather than appending, so it is bounded by one config file and cannot accumulate;
and `saveBindings` walks the action table, so a parked row is never written. `Config binds unknown
action` now fires at **teardown**, because "nothing has declared it yet" and "no game will ever
declare it" are the same state at startup and only distinguishable at exit.

What it does not fix: `saveBindings` still writes only rows that exist, so a player who rebinds
the flycam, plays a whole session on the follow camera and rebinds something else before quitting
still loses the flycam edit. Writing parked rows back out would preserve it at the cost of a typo
living in the config forever — a separate decision, deliberately not taken.
| `declare(name, defaults)` | revives it, same id, bindings intact |
| `conflicts()`, `BindingMenu`'s listing, `Engine::applyBindings`' listing | skipped |
| `saveBindings` | **written** |
| `actionCount()` | still the table size |

`saveBindings` writes a retired row because a player who rebinds the fly camera, switches to a
third-person scheme and quits must not lose the edit for being inactive when the file was
written. And `actionCount()` is the table *size* because three loops use it as a bound —
`saveBindings`, `Engine::applyBindings` and `BindingMenu::rebuild` — so a live count would make
them skip real rows rather than dead ones.

**There is one cursor, so grabbing it is process state.** `core::input::mouseGrab()`,
`mouseRelease()` and `mouseGrabbed()` are free functions over a file-local boolean, the shape
`Logger` and `Profiler` already use — a static member of `InputMap` would say the state belongs
to a map, and a second map would be talking about the same physical pointer. It replaces asking
the camera which action is a drag, which was a hook for one behaviour.

`Input.cpp` is hosted and pulls in neither Vulkan nor a window, so the call records a *desire*
and `Engine`'s frame applies the GLFW mode change; the effective grab is
`desired && !uiOpen && hasFocus`, which is what keeps a panel opening mid-drag from leaving the
pointer captured — the desire survives and nothing has to re-assert it. One boolean, last writer
wins, no refcounting. It has a reset entry point because the unit suite links the hosted sources
and would otherwise carry a grab between tests. A declarative alternative — an action flagged
"grabs the pointer while held" — only expresses *while this action is held*, and a first-person
camera wants the pointer for as long as it is the camera, with no button down at all.

**A run can state its own input.** `input::Script` is a list of `(frame, action, down, pad)` fed
into the map through the same `onKey` / `onMouseButton` / `setGamepad` calls the window layer
uses, addressed by frame index and never by elapsed time. It is a third *source* for
`InputMap` rather than a mechanism beside it, and every consequence of that is deliberate:
text mode still suppresses a scripted key, the deadzone still applies to a scripted stick, a
scripted press and release inside one frame still reads as a tap, and a rebind still moves
what a script fires. That last one is the point — the thing this exists to drive is the
binding table, and a feed that set `pressed` directly would be a test of the feed.

It lives in the engine rather than in the tests because what an end-to-end regression drives
is a game binary, and a feed the engine did not ship could not reach one. `--input-script` is
the flag; [tooling.md](tooling.md) carries the syntax and what the golden suite does and does
not say about it.

`input::TextInput` holds a byte offset that always sits on a UTF-8 boundary — backspace
deletes a whole codepoint, never one byte of three. **Repeat is split on purpose:** the
character callback already repeats at the system rate, so held letters repeat for free and
honour the user's own settings; nothing repeats a backspace, so this does. `GLFW_REPEAT`
is dropped entirely.

**A generated table can be wrong in a way a hand-written one cannot.** `SUBSTRATE_KEY_LIST`
names its parameter `SUB_ENTRY` rather than `X` because the list contains an entry for the
letter X: the preprocessor substituted the parameter into its own argument and compiled a
`Key::X` that was actually named after the macro. It built clean. The `static_assert` per
row against the GLFW macro is why the third column exists.

---

## User interface

`engine/ui/Ui.{h,cpp}` — 742 lines, against the 1774 of the retained-mode canvas it was sized
from.

**Immediate mode**, and it is a consequence of the simplicity rules rather than a style
borrowed from elsewhere. A widget is a function call that both draws and answers —
`if (ui.button("Screenshot")) ...` — and the whole of what persists between frames is
three integers plus a scroll offset per panel. A retained widget tree is the single most
reliable way a codebase acquires both interfaces and virtual dispatch; immediate mode
deletes the tree rather than abstracting over it. **The call stack is the hierarchy**, for
as long as the frame takes.

The stated cost: frame N+1's layout can depend on frame N's content. Two places pay it —
a panel's scrollbar and a list's extent — and both say so.

### Rects for four texels

**There is no `ui_rect` pipeline.** `overlay.frag` already multiplied a vertex colour by
an R8 atlas, so a quad whose texcoords sit on a texel with coverage 1 *is* a solid
rectangle. `Font::reserveWhiteBlock` appends four rows to the atlas and publishes the
centre texel.

That buys more than the pipeline it saved: a button's background and its label are
consecutive vertices in one buffer, so they composite in submission order, where two
pipelines would need a bind per widget or two passes — and two passes break the moment one
panel overlaps another.

**An image is the same quad with a different slot.** `ui.image(id, ...)` takes a
`gfx::ImageId`, and the one thing it does before laying the rectangle out is turn that
handle into the slot a vertex carries — which is where a destroyed or never-issued handle
is refused, and it degrades to the font atlas rather than to whatever reused the slot. The
`DrawList` beneath it still takes a bare slot, because it is the vertex builder and a slot
is what a vertex holds.

Clipping is the one thing the shared pipeline did need: a `DrawCommand` per clip
rectangle, one `vkCmdSetScissor` each, and a clip stack that **intersects and never
replaces**, so a widget cannot escape its panel by asking for a bigger rectangle.

**Vertex colours are converted to linear in the shader**, and the colour space is the
thing to get right here. The swapchain is `_SRGB`, so the hardware encodes linear to sRGB
on write; passing authored sRGB colours straight through applies that encode to values that
were never linear, and a `0x17` panel grey reaches the screen as `0x55`. White-on-dark text
hides this almost entirely, which is what makes the conversion easy to omit — a black drop
shadow arriving as mid grey is the tell.

The conversion is in the shader rather than on the CPU: the vertex colour is `R8G8B8A8`, so
converting before packing would quantise the *linear* value into eight bits, which has
almost no resolution in exactly the dark greys a panel is made of. A second consequence
worth knowing: blending against an `_SRGB` attachment happens in **linear** space, so an
alpha value is perceptually far less opaque than its number reads, and the theme's values
are measured rather than guessed.

### Layout, widgets and routing

**Flow, not constraints.** Widgets stack downward, `beginRow(n)` places the next n across
the content width, and every container knows its own width from its parent — a rule one
paragraph long, which is what lets a widget be written without knowing what contains it. A
constraint solver buys alignment across containers that do not know about each other and
costs a solver, a dependency graph and a relayout pass. Everything goes through one
function, `allocate`.

A row left part-filled still takes its height — without that, a `beginRow(3)` holding two
widgets has the next thing drawn on top of them — which presents as a rendering artefact
and is entirely a layout one.

Behaviours that are decisions rather than drawing:

- A **button fires on release inside itself**, because pressing and dragging away is how a
  user changes their mind.
- A **slider reads the pointer's absolute position** rather than an accumulated delta,
  because a drag that leaves the track and returns has to land where the pointer is.
- A **text field writes through** rather than committing on Enter, because an inspector
  that only applied a value when the user remembered to press Return is one where half the
  edits silently do nothing. Escape restores, clicking away commits.
- **Every widget is reachable from the keyboard**, including the list, which scrolls to
  follow its own selection.

Identity is the caption hashed with the enclosing frame's id, so the same caption in two
panels is two widgets and the same caption in the same place is the same widget between
frames — which is what makes focus and a drag survive at all.

`InputMap::setPointerMode` is the half worth recording: without it, any world drag sharing a
button with `Ui.Click` means dragging a slider spins the world behind it. `Camera.Orbit` was
that case until it moved to `Mouse.Middle` — left and right are a game's to spend, since a
pointer over a 3D world is how a game selects and orders — and a game putting its own action
on `Mouse.Left` is the same case again. It lives in `InputMap` rather than in the camera
because the camera is neither the only consumer nor the last.

### DPI

**One multiplier, applied in one place.** `Context::begin` scales every theme distance
once and every widget reads the scaled copy, never the source. `--set ui.scale=2` renders
exactly what a HiDPI display would, which is what makes the path testable on an ordinary
monitor.

Three things had to follow it and the third would have been missed: the row height is the
**larger** of the theme's and the font's, so a theme written for a 16 px font does not
clip a 32 px one; the panel is **clamped to the window**, because sized in logical units
it is 1120 px tall at 200% against a 900 px window; and **the font itself**, which a TTF
re-bakes and the embedded bitmap cannot. It is a bitmap sampled NEAREST, so **integer**
magnification is exact. A fractional scale is deliberately not offered: a blurry bitmap
font would be worse than a small one.

**Cost:** a panel of about forty widgets is **0.0072 ms CPU** and **0.0031 ms GPU**, about
0.2% of a 3.5 ms frame. Closed it costs nothing at all.

### The inspector

`engine/ui/Inspector.{h,cpp}`. Lists live instances, shows identity, flags, world bounds and
triangle count for the selected one, and edits its position.

**There is no property registry, no reflection, no `describe<T>()` and no field table.**
`drawInstanceInspector` writes out what an instance has, one widget per property. A
property system is a schema plus a type-erased setter plus a name-to-offset map — three
abstractions to save writing `ui.slider("X", p.x, ...)`. The second inspectable thing will
be a second function.

**The second one arrived and it is a second function.** `drawNodeInspector` inspects the
scene tree — hierarchy, local TRS, and the attachment record — in the same file, and the
prediction above is what it was written against rather than a coincidence. The trigger for
revisiting the refusal is still the *third* thing, and what the two share is worth knowing
because it is less than it looks: a caption cache keyed on a revision, and a selection index
that clamps. Neither is a schema.

Three decisions:

- **Selection is a slot, not an `InstanceId`.** A selection is a thing on screen the user
  is pointing at, and it has to survive the object under it being destroyed and the slot
  reused — which is exactly what a generation counter exists to stop being valid across.
- **Position edits write the translation column directly.** Exact: rotation, scale and
  shear are untouched, so dragging back to the start restores the matrix bit for bit.
  Decompose-edit-recompose is lossy for any matrix not built as translate\*rotate\*scale,
  and lossy *again* on every frame of a drag. Rotation and scale are read-only.
- **`kInstanceVisible` prints as "gpu-side"**, not as a flag, because the CPU-side bit is
  always clear and a `-` beside `live: yes` would read as "nothing is on screen".

Every write goes through `setTransform`, so the world bounds the cull dispatch tests are
refreshed because the mutation went through the call that refreshes them.

### The node inspector

`drawNodeInspector`, in the same file. A depth-first listing of the scene tree, then
identity, hierarchy, local transform, world position and the attachment record for the
selected node. The caption carries the indent and one letter per attachment — `M` mesh, `B`
body, `C` character, `S` sound, `L` light, `E` emitter — because "which of these forty nodes
has the light on it" is the question the panel is opened to answer.

Where it differs from the instance panel is the interesting part, and every difference has a
reason of its own:

- **Selection is a row of the listing**, not a slot and not a `NodeId`. One step further
  than the instance panel's slot, and for the same argument taken to its end: an id goes
  stale when the node is destroyed, a slot survives that but not a *reparent* — which
  reorders the listing without destroying anything — and the row survives both.
- **Position and scale are editable and rotation is not.** A node stores TRS *as* TRS, so
  writing one back is exact and the instance panel's decomposition problem does not exist
  here. Rotation is held back for a different reason: three sliders over a quaternion's
  components produce an unnormalised quaternion between any two frames of a drag, and Euler
  angles would be a second representation to keep in step with the first.
- **A `driven` row**, because `Scene` takes a solver's matrix verbatim and never writes the
  local TRS back — so for a node with a body or a character the sliders move a number
  nothing reads. That is a fact about the node and it gets a row, the same call the
  instance panel's `visible: gpu-side` makes.

The listing is rebuilt on `Scene::structureRevision`, so an animated scene costs no strings.
The demo splits its inspector column between the two panels: what the table holds and what
the tree holds are two views of the same object, and a reader compares them.
