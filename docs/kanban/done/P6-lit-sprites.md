---
id: P6
title: Lit sprites
arc: P
size: M
verification: golden-11, readback, tests-hosted, validation, trace
---

# P6 — Lit sprites

A sprite that goes through the shading path: a quad in the world, drawn by the G-buffer pass
under a material and a `gfx::ShaderVariant`, lit and shadowed like every other surface. The
2.5D half of the arc, and the half that gives up bit-exactness on purpose

## Scope

**The card above was a stub** — one sentence naming three nouns, with no statement of the
surface, no exclusions, and a `verification` line naming `golden-12`, a count that has not
existed since `no-ibl` was retired. This section was written before any code, and it is the
part of the row worth more than the code, because the row exists to resolve a conflict
between two guarantees rather than to add a feature.

### The tension this row has to resolve, stated first

P4 draws sprites **after the tonemap**, into the virtual-resolution target, with no depth
test. That is deliberate and its argument is on P4's card: downstream of the tonemap is the
only place in this frame where a texel reaches the swapchain unaltered, and that is what
makes `scripts/readback.sh` a *proof* rather than a regression check. P4's header states the
cost in its own words — no occlusion by 3D geometry, no bloom, no SSR, no fog.

**A lit sprite cannot have both properties, and no amount of design makes it possible.**
Lighting a sprite means exposure, a BRDF, shadow attenuation, ambient occlusion, fog and a
tonemap curve are applied to it. Every one of those is a correction to the value in the file,
which is precisely what "a texel authored is a texel presented" forbids. So this row is not
free to pick both; it is only free to be honest about which it picked and where the boundary
runs.

### Decision 1 — two paths, not a mode

**Lit sprites are a second path. `scene::SpriteTable` and `Renderer::recordSprites` are not
touched by this row at all.**

Three reasons, in the order they decide it:

1. **The difference is not a flag; it is a different point in the frame.** The unlit pass
   runs after the tonemap, into the virtual target, with no depth test and one blend state.
   A lit sprite draws into the G-buffer, before lighting, with depth test *and* depth write,
   into four attachments. A `bool lit` on `SpriteDesc` would be a field that silently moves a
   draw between two passes with different attachments, different blend states and different
   sort orders. That is not a mode; it is two subsystems wearing one struct.
2. **A guarantee with a runtime condition on it is not a guarantee.** P4's header says a
   sprite is bit-exact. If `lit` were a field, that sentence would become "bit-exact if `lit`
   is false", and the readback would be checking a configuration rather than the engine. The
   thing the arc exists to protect would have become conditional on a bool.
3. **`SpriteTable` is hosted and holds no Vulkan, and a lit sprite's residency is three
   things it deliberately knows nothing about** — a geometry range, a material index and an
   instance slot. Teaching it those would put the scene, the uploader and the instance table
   into the one file in this subsystem that links no device.

They are two passes, each honest about what it gives up, and the two headers now state each
other's trade rather than only their own.

### Decision 2 — no new pass, no new pipeline, and G5's `ShaderVariant` is the mechanism

A lit sprite is **a quad with a material**, and G5 already built exactly the machinery for
"a surface the standard glTF material cannot describe". So this row adds:

- **no `Renderer::recordLitSprites`**, because there is nothing for it to do that
  `recordGBuffer` does not already do;
- **no pipeline**, because `Renderer::variantPipeline` creates one lazily for the variant the
  material names, out of the layout the engine already wrote;
- **no `LitSpriteTable`, no base class, and no second renderer.**

The consequence is worth stating plainly because it is what makes the row cheap: **a lit
sprite inherits every pass that already runs.** Culling, the depth pre-pass, shadow cascades,
the punctual atlas, SSAO, SSR, fog, velocity, TAA, bloom and the tonemap all treat it as
geometry, because it *is* geometry. A new pass would have been a second thing to keep in step
with MSAA changes, hot reload, debug views and the G-buffer contract.

**Why a variant rather than reusing `gbuffer.frag` unchanged.** Two things a lit sprite needs
that the standard material path cannot express:

- its texture lives in `gfx::ImageTable` (P1), not in the scene's glTF texture array, because
  the authoring path for a sprite is `e.images().load("res:/hero.png")` and not "wrap the
  sheet in a glTF";
- its UV rect is in **texels** and the divide by the image's size has to happen in the
  fragment shader against `textureSize`, for the reason P4 already found and wrote down —
  `ImageTable` holds no `VkImage`, so nothing CPU-side knows the file's dimensions.

Both are four lines of GLSL. Neither is expressible as a material field. That is the exact
shape G5's header describes, so the variant mechanism is used as designed rather than
extended.

### Decision 3 — the image array the scene's pipelines can reach

This is the one renderer change in the row, and it is the Rule of Threes arriving on
schedule. `gfx::ImageTable` has two callers today: the overlay (C5, promoted by P1) and the
sprite pass (P4). **The G-buffer is the third**, so `overlaySetLayout` is added to the
`gbuffer` and `shadow` pipeline layouts as **set 2**, and the set is bound beside the frame
set and the scene set at the three record sites that bind them.

It also closes a gap G5 left open and did not notice: **a game could supply its own GLSL and
had no way to give that GLSL its own texture.** Set 1 is the scene's array, which holds only
what a glTF brought. After this row a variant can sample anything `e.images()` loaded.

The alternative was to put sprite images into `GltfScene`'s own array through the public
`acquireTextureSlot` / `bindTexture` pair. It is refused for a stated reason rather than on
taste: **that free list has no generation**, which
[arcs.md](../arcs.md#the-defect-c1-left-behind-and-why-it-matters-here) records as the one
lifetime C1 did not reach and the exact silent-alias bug `Handle<Tag>` exists to prevent.
Building the 2D content path on the one lifetime model the engine knows is broken would be
the worst available answer.

### Decision 4 — a lit sprite is `MASK`, not `BLEND`, and that overturns the arc's own note

[arcs.md](../arcs.md) says of this row:

> "A lit sprite is blended; `InstanceTable.cpp:166` excludes blended instances from
> `dynamicCount()`, so a lit sprite writes no velocity and therefore gets no TAA motion
> correction."

**That is true of a blended lit sprite and a lit sprite is a cutout.** Pixel art has a hard
alpha edge; ALPHA_MODE `MASK` is what the format calls that, and it is what every 2.5D
impostor, billboard and character card in every engine uses. Choosing `MASK` costs one
`discard` and buys back everything the note gave up: the sprite goes through the *deferred*
path, so it writes depth, writes velocity, gets TAA motion correction, occludes and is
occluded by 3D geometry, receives SSAO, is reflected by SSR, is fogged, and cuts its own
silhouette out of the shadow map through `sprite_lit_shadow.frag`.

Blended lit sprites — a glow, a soft particle card — are **deferred**, with the trigger
stated below. That is also why the forward pipeline layout is left alone in this row.

### What the surface is

- **`scene::quadMesh(const scene::QuadDesc&) -> scene::MeshData`** — the geometry, as one
  `createMesh` call wants it. Hosted, in `SUBSTRATE_HOSTED_SOURCES`, so the arithmetic that
  actually goes wrong — corners from a size and a pivot, the UV corners, the flip, the
  winding, the tangent frame — is provable without a device, exactly as `SpriteTable`'s is.
- **`scene::LitSpriteDesc`** — image, texel UV rect, size, pivot, world position, rotation,
  tint, cutoff, roughness, metallic, emissive, flip, dynamic. The same vocabulary
  `SpriteDesc` uses, in the same units, so the two paths are not two dialects.
- **`Engine::createLitSprite(const scene::LitSpriteDesc&) -> GltfScene::ModelId`** — creates
  the material and the quad and returns the id `removeModel` already frees. No new lifetime
  model, which is what the P6-vs-C1 non-overlap in [arcs.md](../arcs.md) asks for.
- **`Engine::setLitSpriteUv(ModelId, const glm::vec4& uv)`** — rewrites the material's texel
  rect. The hook P5's sheets need: `frameUv(sheet, n)` goes straight into it.
- **`engine/shaders/sprite_lit.frag`** and **`sprite_lit_shadow.frag`**, and the variant the
  engine registers lazily for them.
- **`GpuMaterial::gameImage`**, which is the word previously called
  `occlusionTextureUnused` — a slot in `gfx::ImageTable` rather than in the scene's array.
  Renamed rather than added, so the struct's size and layout do not move.

### What it deliberately excludes, and why

- **No blended lit sprite.** Above. Trigger: a game with a soft additive card — at which
  point `overlaySetLayout` joins the forward layout too and the variant names a
  `forwardFragment`.
- **No shared quad primitive across sprites.** Each lit sprite is its own `createMesh`: four
  vertices and six indices, 216 bytes. One primitive with N instances would be cheaper and
  needs an "instance this primitive again" call the engine does not have. `GpuInstance::meta.y`
  is already a per-instance material index, so the mechanism is there when a caller needs it.
  Trigger: a stated count that makes 216 bytes a sprite matter.
- **No engine-owned animation of lit sprites.** `setLitSpriteUv` is the whole hook; driving it
  from a `SpriteTable` playback would mean a lit sprite that is also a `SpriteId`, which is
  the two-subsystems-in-one-struct mistake Decision 1 refuses.
- **No sorting, no layers.** A lit sprite is depth-tested. That *is* the sort, and it is the
  reason to use this path rather than P4's.
- **No per-lit-sprite material reuse.** One `createLitSprite` is one material, and material
  slots are never reclaimed (`unloadModel` says why). A scene's material headroom is 64.
  Trigger: a game that creates and destroys lit sprites during play, which wants either a
  material free list or a `material` field on the desc naming one the caller already owns.

## Verification

Everything below must pass before this may enter `done/`:

- `scripts/golden.sh check release` — **eleven** cases, byte-identical. (The card said
  `golden-12`, a count retired with `no-ibl`; P1's, P2's and P4's carried the same stale
  number.) **Necessary and load-bearing rather than a formality here**: this row changes the
  `gbuffer` and `shadow` pipeline layouts, which every one of the eleven draws through. A
  moved pixel is a defect in a layout change that must move none.
- `scripts/readback.sh release` — **nine of nine still bit-identical.** This is the strongest
  single check available to the row and it is a check on what the row did *not* do: P4's and
  P5's unlit bit-exactness must survive untouched. A row that made lit sprites possible by
  weakening the unlit path would fail here and nowhere else.
- **The lit path's own image check, and the arc owes a stated exception for it.** The P arc is
  *defined* by "a texel authored is a texel presented, checked by reading back the presented
  image and comparing it against the source file". **That check is definitionally unavailable
  to this row** — the whole point of the row is that the value is shaded — so P6 is the row
  where the rule needs an exception with a reason rather than a quiet omission.

  Re-snapping is not the answer either: `docs/architecture/tooling.md` forbids
  `scripts/golden.sh snap`, and a golden case for a lit sprite would be a picture somebody
  accepted, which is the standard the arc exists not to use.

  **What replaces it is coverage.** Lighting changes a pixel's *value*; it cannot change which
  pixels the sprite covers. The silhouette of an alpha-cutout sprite is decided entirely by
  the source file's alpha, the cutoff, the pivot, the texel rect, the quad, the projection and
  the viewport transform — which is every place a half-texel is lost, and it is computable
  from the source file rather than snapped. So:

  ```bash
  scripts/readback.sh release       # ten cases; the tenth is the lit silhouette
  ```

  `--readback-lit-sprite` runs the identical command line twice, once without the sprite and
  once with it, and asserts two properties:

  1. **Outside the expected silhouette, the two frames are bit-identical.** Zero tolerance,
     zero pixels allowed. A sprite one texel too wide, one texel offset, mirrored, rotated,
     or drawing the wrong cell puts a differing pixel outside the mask.
  2. **The bounding box of the differing set is exactly the expected silhouette's bounding
     box.** Property 1 alone is satisfied by a sprite that drew nothing; this is what refuses
     that, and it also refuses a sprite that came out too small or in the wrong place.

  The expected mask is the source PNG's alpha at or above the cutoff, expanded by the
  presentation scale and placed at the stated offset — computed from the input, with nothing
  to re-snap when it fails.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation. `quadMesh` is
  Vulkan-free by construction and its arithmetic is the half of a lit sprite that lands it in
  the wrong place.
- Zero validation errors with layers on. A set added to two pipeline layouts is exactly the
  kind of change that renders correctly and reports a bound-set mismatch, so this is a real
  check here rather than a ritual.
- Per-pass GPU cost from `scripts/baseline.py` trace medians, several runs per arm — never
  the `GPU @` log line. The claim to discharge is that **a scene with no lit sprites pays
  nothing for the row**, which is what an added descriptor set to two layouts has to prove.

## Reference

[architecture/rendering.md](../../architecture/rendering.md).

## Outcome

**The card was a stub** — one sentence, three nouns and a `verification` line naming a case
count that has not existed since `no-ibl` was retired. The Scope above was written before any
code, and writing it changed the row three times: it produced the two-paths decision, it
produced the `MASK`-not-`BLEND` finding that overturned the arc's own note about this row, and
it produced a verification the card as written would not have had. The exclusion list is worth
more than the feature.

### The argument this row was written against was overtaken before it started

[arcs.md](../arcs.md) argued:

> "P6 needs one unit quad in the geometry buffer and no more. That is cheaper than waiting for
> G4's `createMesh`, and it is worth stating that the row was checked against that dependency
> rather than assuming it."

**The argument for not waiting became moot because the thing it declined to wait for arrived
first.** G4 landed `GltfScene::createMesh(MeshData)` and `createEmpty`, so the buffers a quad
sub-allocates from exist even for a game that names no scene — and the quad is now one
`createMesh` call rather than a special case in the geometry buffer. The row got smaller for
exactly the reason the argument said it would not need to.

**A second prediction of that document was overturned by doing the work**, and this one
mattered more:

> "A lit sprite is blended; `InstanceTable.cpp:166` excludes blended instances from
> `dynamicCount()`, so a lit sprite writes no velocity and therefore gets no TAA motion
> correction."

A lit sprite is a **cutout**, not a blend. ALPHA_MODE `MASK` is what a hard alpha edge is
called and what every 2.5D impostor uses; it costs one `discard` and buys back everything that
sentence gave up — depth, velocity, TAA motion correction, occlusion both ways, SSAO, SSR, fog
and a silhouette cut out of the shadow map. The note was true of a design nobody had to choose.

### The design decision, which is the row

**Two paths, not a mode.** P4's `SpriteTable` and `recordSprites` are untouched; a lit sprite
is an `InstanceTable` entry over a quad. The reasoning is in Decision 1 above and the short
form is that the difference is not a flag but a different point in the frame — different
attachments, different blend state, different sort order — and that a guarantee conditional on
a `bool` is not a guarantee.

**No pass and no pipeline, and G5's `ShaderVariant` is the mechanism**, which the card was
asked to evaluate rather than assume. It is used, and the reason is that it is the case G5's
own header describes: a lit sprite needs two things the standard material path cannot express
— an image from `gfx::ImageTable` rather than from the scene's glTF array, and a UV rect in
texels divided by `textureSize` — and both are four lines of GLSL. A new pipeline would have
been a second thing to keep in step with MSAA changes, hot reload, debug views and the
G-buffer contract, in exchange for nothing. **The consequence is that a lit sprite inherits
every pass that already runs**, because it is geometry.

### What landed

- `scene::quadMesh(QuadDesc)` and `scene::LitSpriteDesc` in `engine/scene/LitSprite.{h,cpp}`,
  hosted.
- `Engine::createLitSprite`, `Engine::setLitSpriteUv`, `Engine::litSpriteShader`. A lit sprite
  is a `GltfScene::ModelId`, freed by `removeModel`, so the row adds **no lifetime model** —
  which is what the P6-vs-C1 rule asks.
- `engine/shaders/sprite_lit.frag` and `sprite_lit_shadow.frag`, and one variant the engine
  registers lazily. Without the second one every lit sprite would cast a solid rectangle,
  which is the failure `shadow.frag`'s own header records for Sponza's foliage.
- `GpuMaterial::gameImage`, which is the word that was `occlusionTextureUnused`. Renamed
  rather than added, so no offset in the struct moved.
- `overlaySetLayout` as **set 2** of the `gbuffer` and `shadow` pipeline layouts — the one
  renderer change, and the Rule of Threes arriving on schedule: `ImageTable`'s callers were
  the overlay and the sprite pass, and the G-buffer is the third. It also closes a gap G5 left
  open without noticing, that a game variant had its own GLSL and no way to give it a texture.
- `gfx::compareSilhouette`, `--readback-lit-sprite`, `--readback-lit-cutoff`,
  `--readback-background`, `engine/assets/cutout.png`, and the tenth case in
  `scripts/readback.sh`.

**What was refused and why**: putting sprite images into `GltfScene`'s array through the
public `acquireTextureSlot`/`bindTexture`. That free list has no generation — the one lifetime
C1 did not convert — and building the 2D content path on the engine's one known-broken
lifetime model would be the worst available answer. P6 was the row that could have been
`limitations.md`'s "second caller of `acquireTextureSlot`" and deliberately was not.

### The verification the lit path gets, and why it cannot be the readback

**The P arc is defined by "a texel authored is a texel presented", and this row cannot
satisfy it.** Exposure, a BRDF, shadow attenuation, ambient occlusion, fog and a tonemap curve
are six corrections applied to the value in the file, and applying them is the entire reason a
game would draw a sprite this way. Re-snapping is not the alternative either: `golden.sh snap`
is forbidden and a snapped reference is the standard this arc exists not to use. So the rule
gets a stated exception rather than a quiet omission, recorded in
[tooling.md](../../architecture/tooling.md#the-lit-exception).

**The claim moves from the value to the coverage**, which is the largest property lighting
leaves alone. Two runs of one command line differing in one number — `--readback-lit-cutoff 2`
is above every alpha there is, so every fragment discards while the material, the instance, the
indirect command and the pipeline stay identical, which is a stronger control than omitting the
sprite. Then two assertions, both computed from the source file:

1. Outside the expected silhouette the two frames are **bit-identical**, zero tolerance.
2. The bounding box of the differing set is **exactly** the silhouette's.

Property 1 alone passes a sprite that drew nothing; property 2 is what refuses that, and a
negative control confirms it — the measured frame compared against *itself* reports `changed
box [0,0)-[0,0), expected [6,9)-[150,135)` and exits 1.

The source is an **L**, asymmetric in both axes so a mirror, a transpose and a quarter turn
each move the box differently, with a concave notch that is a large transparent region *inside*
the box — which is the half of the check a filled silhouette could not make. Alpha is 0 or 255
and nothing between, so the cutoff is a threshold rather than a question about a soft edge.
The case runs `--no-bloom --no-ssao --no-ssr`: those passes legitimately change pixels outside
a silhouette and property 1 is right to fail on them.

### The defect the verification caught, and it was found by the layer rather than by an image

**Adding one set to two pipeline layouts made four `vkCreatePipelineLayout` calls illegal, and
nothing but the validation layer would have said so.** `maxPerStageDescriptorSampledImages` is
a sum over *every set in a pipeline layout*, not over one set — and P1 declared the game image
array at the whole device limit, which was free for as long as that array was bound alone in
the overlay's layout. The moment it joined the G-buffer's, the sum came to the limit plus the
scene's own 134 textures and the device refused all four layouts, in eight messages per
pipeline.

**A set that claims the entire budget cannot be bound beside anything, which makes the budget
useless rather than generous.** `createDescriptorLayouts` now subtracts a stated 4,096 for what
the sets beside it may declare — chiefly `GltfScene`'s array, whose count is not known until a
scene loads. Nothing in the tree is within two orders of magnitude of that reserve; the trigger
to make it a real calculation is four thousand textures in one scene.

Worth recording because of what it says about the check: the golden suite was byte-identical
*with the bug present*, because a refused pipeline layout still yields a handle the driver
happily draws with on this device. Eleven of eleven and nine of nine both passed. Only the
layer knew.

### Verification

- `scripts/golden.sh check release` — **11 of 11 match**, across a change to the `gbuffer` and
  `shadow` pipeline layouts, a renamed field in the shared material struct and a reduced image
  ceiling. Every one of the eleven draws through those layouts.
- `scripts/readback.sh release` — **9 of 9 bit-identical**, plus the lit silhouette exact and
  the resize soak clean. The nine are the row's strongest check and they are a check on what it
  did *not* do: P4's and P5's unlit bit-exactness survived a row that changed the layouts every
  scene pass binds.
  - `lit-sprite` — 0 pixels differ outside the mask; changed box `[6,9)-[150,135)` is exactly
    the 8,424 covered texels of `res:/cutout.png` at 3x.
- `./test.sh debug` — **754 of 754**, including **8 new `LitSpriteTests` cases**.
  `./test.sh asan` — **754 of 754**.
- Validation, layers on, debug build: 120 frames of the lit path at 960x540 with a 320x180
  virtual resolution — **zero errors and zero warnings**, after the ceiling fix. Before it,
  eight errors per pipeline layout.
- `scripts/baseline.py --zones`, release, 1x MSAA, medians over 6 runs of 600 frames, two arms
  differing only in the cutoff:

  | | discarded | drawn |
  |---|---|---|
  | `GBuffer` | 0.041 ms | 0.042 ms |
  | `Lighting` | 0.033 ms | 0.036 ms |
  | `Frame` | 0.458 ms | 0.462 ms |
  | CPU busy | 0.139 ms | 0.139 ms |

  One lit sprite covering 8,424 virtual texels is **+0.004 ms of `Frame`**. The engine-defaults
  arm (Sponza, no lit sprite) is `Lighting` 1.067 ms and `Frame` 2.087 ms; a scene with no lit
  sprites pays nothing structurally, because the row adds no pass and no draw — only one more
  set in descriptor-set binds that were already being made.

### Deferred, with triggers

All five are in [limitations.md](../../architecture/limitations.md): a blended lit sprite (a
soft additive card); a shared quad primitive across lit sprites (a count that makes 216 bytes a
sprite matter); reclaiming a lit sprite's material slot (a game that spawns and retires them
during play); a normal map on a lit sprite (a game wanting per-sprite normals); and
`SpriteTable` driving a lit sprite's playback (the third game that writes the one line).

### Found and left alone

**`Renderer::stats.particles` still has the staleness P4 reported one row ago** — assigned only
where a draw happens, so a scene whose last particle died reports the previous frame's count.
Still one line, still in a pass this row did not touch, and now on its second sighting. The
third is the one to fix.

**`ShaderVariant::forwardFragment` is now the odd one out.** The G-buffer and shadow layouts
can reach the game's image array and the forward layout cannot, because a lit sprite is a
cutout and this row had no caller for the third. That asymmetry is deliberate and is exactly
what the blended-lit-sprite trigger buys off; it is written here so the next reader does not
find it and read it as an omission.
