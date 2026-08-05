# Rendering

A deferred PBR renderer with a multisampled G-buffer, per-sample lighting, and an optional
temporal path. Everything below lives in `engine/gfx/Renderer.{h,cpp}` unless stated.

---

## The frame, in order

Each entry is a `recordX(cmd, slot)` method. There is no render graph: the order is the
order they are called in, and the barriers are written where they belong.

**The frame is scene-wide work, then one chain per view, then the window's own passes.**
Rows 1–6 run once however many views there are — the instance upload, the cull that fills
every list, the skinning, the acceleration refit and both shadow maps are properties of the
scene, and re-running them per view is the cost `Renderer::View` exists to avoid. Rows 7–18
are `recordViewChain`, recorded once per registered view into the **same** targets, each
ending in a tonemap into that view's own destination. Rows 19–23 belong to the view that
presents.

| # | Pass | What it does | Conditional on |
|---|---|---|---|
| 1 | `updateInstances` + `recordInstanceUpload` | Stage and copy the instance table, joints, morph weights, previous transforms, and the command lists | — |
| 2 | `recordCull` | One dispatch per cull view (the camera, the sun, up to 24 atlas layers) writing per-view command lists. **Before the shadow passes, because they draw out of lists 1..** | `culling` |
| 3 | `recordSkinning` | Compute skinning and morph-target blending into a second vertex buffer, and **copy the frame's solved cloth into the same one** | scene has a rig or a `FABRIC_` mesh |
| 4 | `AsRefit` | Refit BLAS for deformed instances, rebuild the TLAS | RT on, and dynamic geometry |
| 5 | `recordShadows` | The sun's map, one 4096-square D32 layer | — (still clears when off) |
| 6 | `recordPunctualShadows` | Point and spot shadows into a D32 atlas | `punctualShadows` |
| 6b | `recordCull` (`CullViews::Camera`) | List 0 again, from this chain's camera. **Not recorded at all in a one-view frame** | a second view exists |
| 7 | `recordGBuffer` | Albedo, normal, ORM, emissive, depth — multisampled, with a depth resolve on store | — |
| 8 | `recordDecals` | Fullscreen per decal, alpha-blended into `gAlbedo` before anything reads it | scene has decals |
| 9 | `recordSsao` | `ssao.comp` then `ssao_blur.comp`, full resolution, depth only | `ssao` |
| 10 | `recordVelocity` | Motion correction for dynamic instances, single-sample | `taa` |
| 11 | `recordLighting` | Per-sample deferred shading into the single-sample HDR target | — |
| 12 | `recordForward` | Alpha-blended geometry, sorted back to front. Binds the opaque depth at **set 4** as well as testing against it | scene has blended draws |
| 13 | `recordParticles` | Simulate, sort, draw | scene has emitters |
| 14 | `recordSsr` | Screen-space or ray-traced reflections, additively composited | `ssr` |
| 15 | `recordFog` | Volumetric fog, premultiplied-over composite | `fog` |
| 16 | `recordBloom` | Threshold, downsample chain, upsample chain | `bloom` |
| 17 | `recordTaa` | Reproject and blend against history | `taa` |
| 18 | `recordTonemap` | Bloom composite, exposure, tonemap, into `composeImage` — the view's own destination, the presentation target, or the swapchain | — |
| 19 | `recordSprites` | Every layer's sprites, sorted on the CPU, one instanced draw | a game created any |
| 20 | `recordDebugLines` | Physics and audio wireframes, no depth test | lines present |
| 21 | `recordOverlay` | HUD text, binding menu, UI panels | overlay or UI present, **and** inside the virtual target |
| 22 | `recordPresent` | Blit the virtual target into the swapchain at an integer scale, letterboxed | the scale is not an identity |
| 23 | `recordOverlay` | The same pass again, at the window's size | overlay or UI present, **and** outside the virtual target |

### More than one view

```cpp
mirror = e.views().create(e.images(), {480, 270});   // in init, at a quarter of 1080p
e.views().camera(mirror)->focus = wallPoint;    // in update, every frame
mirrorArt = e.views().image(mirror);            // an ImageId like any other
```

`gfx::ViewTable` is the lifetime and `Renderer` is the residency — the same split
`ImageTable` draws, and the reason `views()` sits on `Engine` beside `images()` rather than
behind `renderer()`. The table holds a camera and a generation per slot and no Vulkan at
all, which is what makes create, destroy, slot reuse and a refused stale handle testable
with no device; `tests/ViewTableTests.cpp` is those nine cases.

**A view's destination is an ordinary image slot.** `ViewTable::create` adopts one through
`ImageTable::adopt`, so what one view drew is nameable by the same `ImageId` a PNG is —
one kind of texture handle rather than two — and a sprite, a UI quad or a material samples
it with no new path. The renderer neither loads nor frees an adopted slot; it writes the
destination into the descriptor array and `overlayBorrowed` is what stops teardown freeing
an image it does not own.

**A target set per view, at the extent that view asked for** (C38). `create(images, extent)`
takes the size the result will be sampled at, and `{0, 0}` follows the presenting view — which is
what a resize then moves it with. `kMaxViews` is 4, so a game holds three beside the presenting
one. Views still run **serially**, each to completion before the next with one full memory
barrier between; what changed is that each has its own eighteen targets rather than borrowing the
primary's.

**Per-view extent is the feature, and the memory is why.** One full-extent target set is
**224.5 MiB** at 1600x900 and 4x MSAA — measured two ways that agree: 540.2 MiB steady state at
1600x900 against 371.8 at 800x450 gives 168.4 MiB for three quarters of the resolution-dependent
allocation, and three extra full-extent views measured 1230.3 MiB against 540.2, or 230 MiB each.
Four full-extent views is ~898 MiB of render targets. Four at quarter extent is ~56 MiB each.

Time scales with it, and the card's prediction was exact:

| views | extent | `Lighting` | `Frame` |
|---|---|---|---|
| 1 | full | 1.891 | **3.317** |
| 2 | full | 1.885 | 6.838 |
| 4 | full | 1.923 | **13.985** |
| 4 | quarter | 0.617 | **6.945** |

The card predicted four full views would "land near 14 ms, which is the whole frame budget", and
they land at 13.985. **Four quarter-size views cost about what two full-size ones do**, not what
one does — the honest number, and still the difference between shippable and not. `Lighting` at
quarter extent is a median over all four views' instances, which is why it falls rather than
holding.

Two things a secondary view still shares, both deliberate:

- **TAA is off for a non-primary view.** The history is one image pair, and a shared history
  smears each view into the other. `View::taaActive` is what the passes read, so the user's
  `taa` switch decides only the presenting view.
- **Every view ranks its own lights; only the atlas assignment is the primary's.** `updateLights`
  culls and ranks against the matrix and position it is handed, so a view looking elsewhere shades
  what it can see. What it does not do for a secondary is assign atlas layers —
  `recordPunctualShadows` renders one assignment before any secondary chain runs, so a second one
  would describe layers the atlas does not hold and the view would sample the wrong light's depth.
  A secondary looks its lights up by source index instead: one the primary also ranked keeps its
  layer, one only this view ranked gets `params.w = -1` and illuminates without occluding. See
  [limitations.md](limitations.md#a-light-only-a-secondary-view-can-see-illuminates-but-does-not-occlude)
  for why that is the same degradation an atlas overflow already produces, and what a per-view
  atlas would cost.

A one-view frame is unchanged, which is what the golden set checks: 13 of 13 byte-identical, and
`Frame` 3.317 against 3.202 published with `Lighting` 1.891 against 1.850.

**Ordering decisions worth knowing:**

- Decals run *before* SSAO, lighting and reflections, so all three see the decal. A decal
  is part of the surface description, not a correction applied to the image.
- A decal's footprint is the projector's square, or `Decal::round` for the disc inscribed in
  it. A field rather than a texture with a circle in it, because the texture a decal samples
  comes out of the *scene's* bindless array — it is a glTF texture, so a game with a mark it
  did not author into a document has nowhere to put one. Paired with
  `GltfScene::fallbackTextureSlot()`, that is a tinted disc with no asset at all.
- SSAO runs before lighting because the AO it produces is an input to the ambient term.
- Bloom is composited **before** exposure and the tonemap, which is where lens scatter
  physically belongs.
- Debug lines draw after the tonemap so a wireframe is not exposed, curved and bloomed
  with the scene — and before the overlay so text stays on top.
- **Unlit sprites draw after the tonemap for the same reason and one stronger one.** They
  are display-referred art: the texel in the file is the one the artist chose, so exposure,
  a curve and a temporal resolve are three corrections applied to a value that needs none.
  Downstream of the tonemap is also the only place in this frame where a texel reaches the
  swapchain unaltered — see [Sprites](#sprites).
- The overlay appears twice in the table and runs **once**. Which of the two rows fires is
  `uiInsideVirtual`, and no other pass has the choice: a wireframe is world-space geometry
  and always draws with the world, while text and widgets are screen-space by construction.

---

## The projection

`scene::Camera::projection()` builds one of two matrices — `Projection::Perspective` or
`Projection::Orthographic` — and **both are reverse-Z and both are hand-built**. Near maps
to depth 1 and the far plane to 0 under either. The perspective one is additionally
*infinite*: it has no far plane at all, which is what puts float precision where the depth
buffer's hyperbola needs it and keeps Sponza's ~3700-unit extent stable. The orthographic
one takes its box from `orthoHeight`, `nearPlane` and `orthoFar`, and `frameBounds()` sizes
all three from the scene alongside the numbers it already derived.

**Neither is a `glm::` call, and the orthographic one is where that matters.** `glm::ortho`
is forward-Z. Substituting it inverts every `depth > FAR_DEPTH` test in the shaders, fights
a depth buffer cleared to 0, and fights the `GREATER` compare ops — three failures that
present as three unrelated bugs.

**Almost nothing else in the renderer knows which matrix it got**, and that is a property
of how those parts were already written rather than of work done to add the second mode:

| | Why it is projection-agnostic |
|---|---|
| `worldFromDepth` | Goes through `invViewProj`, so SSAO, lighting, decals and fog reconstruct correctly under either |
| GPU culling | `cull.comp` transforms eight corners and tests the six clip-space inequalities rather than extracting planes |
| TAA jitter | A clip-space translation post-multiplied onto the finished view-projection, not a perturbed projection |
| The sun's shadow | Its box is fitted to the scene bounds and never took a camera |

Two things do know which matrix they got.

**`viewDistance()` in `frame.glsl`** turns a depth sample back into a distance along the
view axis, for the SSR march, the fog march and particle collision. It was three copies of
`pc.nearPlane / depth` — the expression that exactly one matrix satisfies — and is now one
expression over four coefficients that `Camera::depthLinear()` reads off the projection's z
and w rows:

```
distance = (depth * p[3][3] - p[3][2]) / (depth * p[2][3] - p[2][2])
```

Four coefficients rather than two, and that is arithmetic rather than taste: a perspective
depth inverts to a multiple of `1/depth` and a parallel one to a combination of `1` and
`depth`, so spanning both needs three basis functions and no two-number expression is exact
for either family plus the other. Under the perspective matrix two of the four are exactly
zero and one is exactly -1, so the expression folds to `near / depth` **to the bit** — which
is what let the second projection land with the golden set byte-identical. Reading them off
the matrix rather than rebuilding them from near and far is what stops a projection and its
inverse from drifting apart.

**The skybox ray**, `viewRay()` in `lighting_body.glsl`, is the one genuine branch in the
renderer, on `flags.w`. A parallel projection has no eye point for rays to fan out from, so
every ray is `cameraForward`. Unprojecting the far plane and subtracting the camera position
— which is correct and cheap under perspective — produces a perspective fan instead, and the
sky then draws a horizon across a frame whose geometry has no field of view to see one.

---

## Render targets

Named `GpuImage` members, created in `createRenderTargets()`. Not a manager.

**They belong to a `Renderer::View`, not to the `Renderer`.** A View is everything a frame
draws into at one extent, plus the storage views, descriptor sets and derived extents that
name those images — `createRenderTargets(View&, VkExtent2D)` fills one and
`destroyRenderTargets(View&)` is its exact inverse. The Renderer holds one, and every pass
records through it.

| In a `View` | Format | Samples | Notes |
|---|---|---|---|
| `gAlbedo` | RGBA8 | MSAA | Base colour |
| `gNormal` | **RG16F** | MSAA | Octahedral. SFLOAT because only SFLOAT is on Vulkan's mandatory colour-attachment list |
| `gOrm` | RGBA8 | MSAA | Occlusion, roughness, metallic |
| `gEmissive` | R11G11B10 | MSAA | Can exceed 1.0 and feed bloom, at the same 4 bytes as RGBA8. Stays MSAA: resolving it *adds* an image rather than removing one — see [limitations.md](limitations.md#resolving-the-emissive-attachment-cannot-save-what-it-looks-like-it-saves) |
| `gDepth` | D32 | MSAA | Reverse-Z under either projection; see [The projection](#the-projection) |
| `gDepthResolved` | D32 | 1x | Produced by a depth resolve as the G-buffer stores. Empty at 1x |
| `hdrTarget` | RGBA16F | 1x | The resolve has already happened by this point |
| `velocityTarget` | RG16F | 1x | TAA motion correction |
| `ssaoRaw` / `ssaoBlurred` | R8 | 1x | Two images, because the blur reads a neighbourhood |
| `bloomChain` | RGBA16F | 1x | Mipped, kept in `GENERAL` for the whole chain |
| `depthPyramid` | R32F | 1x | Min-reduced, power-of-two, up to `kDepthPyramidMips` levels |
| `ssrTarget` | RGBA16F | 1x | |
| `fogTarget` | RGBA16F | 1x | Premultiplied colour plus opacity |
| `taaHistory[2]` | RGBA16F | 1x | Ping-ponged, because the resolve reads a *reprojected* texel. History is **per view** |
| `presentTarget` | swapchain's | 1x | Only where the presentation blit is real; see [Presentation](#presentation-virtual-resolution-and-integer-scale) |

| On the `Renderer` | Format | Samples | Notes |
|---|---|---|---|
| `shadowMap` | D32 | 1x | **One** 4096-square layer, 64 MiB. Not cascaded |
| `punctualShadowMap` | D32 array | 1x | `kMaxShadowLayers` = **24** layers, 1024 square. A spot takes one, a point six |
| `envCube` / `irradianceCube` / `prefilteredCube` / `brdfLut` | | | Baked once at startup |

**The split is what makes a View a boundary rather than a rename.** The IBL bake and both
shadow maps are properties of the *scene*, and a second view shares every one of them — a
View that swallowed the punctual atlas would re-render up to 24 layers per view for nothing.
Pipelines, layouts and samplers stay on the Renderer for the same kind of reason: they are
resolution-independent, so a second view at a second extent reuses them unchanged.

`forwardDepth` / `forwardDepthView` point at whichever of `gDepth` and `gDepthResolved`
applies, chosen in `createRenderTargets()` so the record path has no sample-count branch.

**Every target in the first table is sized by the view's own `renderExtent`**, which every
`renderArea`, viewport, dispatch round-up and inverse-texel push constant reads, so a view's
resolution is decided once and everything derived from it agrees by construction. It equals
the window's extent unless a game named a virtual resolution.

**`destroyRenderTargets` is the authoritative list of what a View owns**, and it must stay
identical to the declaration — nothing else in the file enumerates the set, and a compiler
cannot check it. C31 found the one member that had already drifted: `hizSet`, the cull
dispatch's sampled view of the depth pyramid, was allocated by `createRenderTargets` and
released by nothing.

**The resolve mode is queried, not assumed.** A float depth attachment may only resolve
with `SAMPLE_ZERO` or `AVERAGE` depending on the device, and asserting the wrong one is a
validation error rather than a wrong image.

---

## Presentation: virtual resolution and integer scale

The engine renders into a target of size `V` and presents it at the largest integer `S`
that fits the window, centred, with the leftover as black bars.

```cpp
void MyGame::configure(GameSetup& setup, core::settings::Settings&) {
    setup.virtualResolution = {320, 180};   // omit for native
    setup.uiInsideVirtual   = true;         // the HUD scales with the world
    setup.pixelExact        = true;         // no TAA, no jitter, no curve, nearest sampling
}
```

**There is not a virtual-resolution path and a native path.** There is one path, and native
is its degenerate case: `V = the window extent` makes `S` exactly 1, the source rectangle
the whole target and the destination the whole window. `identityPresent` answers true on
precisely that, `presentTarget` is never created, and `recordPresent` records nothing — so
the tonemap draws straight into the swapchain image exactly as it did before this existed.
Writing it as two modes would have been writing the same arithmetic twice and then
discovering the two disagreed about rounding, which is the only interesting thing here.

### The arithmetic is in a file that links no Vulkan

`gfx::Presentation` — `presentLayout(virtualW, virtualH, windowW, windowH)` returning a
`PresentLayout` of integers, and `identityPresent`. It holds no Vulkan for the same reason
`ImageTable` does: what goes wrong in a presentation step is not a Vulkan call, it is an
off-by-one in a scale, a bar one pixel wider on the left than the right, or a window one
pixel too small silently becoming a 0.996x resample. All three are arithmetic, and
`Presentation.cpp` reaches no device, so the unit suite links it and proves them under
every sanitizer. `Renderer` turns the answer into one `VkImageBlit2` and nothing else.

Every field of `PresentLayout` is an integer, and that is the guarantee rather than an
implementation detail: a `float` anywhere in it would be a fractional scale waiting to
happen.

### What happens when the window is not an integer multiple

Which is nearly always, and it is the decision the row exists to make deliberate:

| Case | What happens | Why not the alternative |
|---|---|---|
| Window is an exact multiple | Presented at that scale, filling the window | — |
| Window is **larger** but not a multiple — 1000x600 holding 320x180 | Scale floors to 3, presents 960x540, **20 columns and 30 rows of bars** | Fitting exactly is a 3.125x resample. A nearest blit at 3.125x doubles every 33rd column of texels and the pattern *moves when the window is dragged* — the artefact this row removes, reintroduced by the step meant to remove it |
| Leftover is **odd** — a 3-pixel remainder | 1 pixel left, 2 right. Truncating division, so it leans right and down | Somebody has to take it, and stating which side is what makes a letterboxed capture reproducible rather than a coin toss |
| Window is **smaller** than `V` even at 1x | Scale clamps at 1 and the source rectangle **crops**, centred on the target | There is no half scale that preserves a texel. Downscaling would be the one place in this path where a texel authored is not a texel presented, so a game in a too-small window sees the middle of its world at the size it was drawn |
| Any extent is zero | The layout has a zero extent and the caller records nothing | A minimised window and a one-pixel swapchain are both things a window manager hands over mid-resize, and `vkCmdBlitImage` rejects a zero extent |

The scale is a function of the window, so it changes under the user's hands: it is
recomputed with the render targets, which is every resize.

### Why the presentation step is a blit and not a fullscreen draw

A correctness decision rather than a convenience one. `presentTarget` carries the
swapchain's own `_SRGB` format, so `vkCmdBlitImage2` between two images of identical format
at `VK_FILTER_NEAREST` moves bytes: no sampling, no filter weights, and **no colour-space
arithmetic**. A fullscreen quad sampling an `_SRGB` view would decode every texel to linear
and let the attachment encode it back, and an 8-bit sRGB round trip through a filter unit is
exactly the off-by-one this is here to make impossible.

The bars are a `vkCmdClearColorImage` over the whole swapchain image before the blit, not
four rectangles around the destination: one command that cannot leave a seam, against four
`VkClearRect` computations that are empty in the common case and wrong in exactly the case
they exist for. It also means a resize that shrinks the presented rectangle cannot leave the
previous frame's pixels in the gap. Skipped where the destination covers the window.

### `pixelExact` is one switch because it is one decision

Four things in the frame destroy pixel-exactness independently — the TAA jitter, the TAA
resolve, the tonemap curve, and a linear tap on the overlay's image array — and a game that
turned off three of them has not made a decision, it has made a bug that presents as a
slightly soft sprite. So `GameSetup::present.pixelExact` does all four: `render.taa` false (which
takes the jitter with it, since the jitter exists to feed TAA and is applied nowhere else),
the tonemap to `clamp`, and `VK_FILTER_NEAREST` on the overlay's images instead of the
linear filter C5 chose for icons drawn at whatever height a layout gives them. Linear is
right for that caller and wrong for every texel this arc cares about, which is what makes it
a switch rather than a correction.

The two settings are written as `Source::Game`, so `--dump-settings` names the game in the
source column and `--taa` on the command line still outranks them — which is what makes
"is TAA what is softening this?" a question one run answers. That last part is a written-out
comparison against `Source::Cli` rather than the ordering that carries precedence everywhere
else, and it has to be: both rows must follow `bindRenderer`, so this is the one write a
game owns that cannot be made before the flags are applied.

### What the UI sees

`framebufferWidth()` / `framebufferHeight()` return the extent of the surface a caller draws
onto, which is the *virtual* target where `uiInsideVirtual` holds. Handing back the window
there would put every widget off the right-hand edge at 6x. `windowExtent()` is the other
number, and only the presentation step and the capture path want it.

The cursor is mapped the other way by `Renderer::uiFromWindow`, through the scale and the
letterbox offset, so a hit test is correct without a game doing arithmetic. It is the
identity in every other case, which is why `Engine::ui()` calls it unconditionally.

---

## Sprites

`Renderer::recordSprites` — a method, not a class, and not a second renderer. The CPU half
is `scene::SpriteTable`; see [systems.md](systems.md#sprites) for layers, handles and the
sort. This section is the pass.

**It is `particle.vert` with the billboard taken out.** Six vertices from a
`const vec2 kCorners[6]` indexed by `gl_VertexIndex`, per-instance data from an SSBO through
`gl_InstanceIndex`, a bindless texture index carried `flat`, `PremultipliedOver`, no vertex
buffer and no vertex attributes. What a sprite adds is rotation, a pivot, a flip and a UV
rect; what it drops is the camera-facing basis, because a sprite sits on the z = 0 plane and
the camera is P3's orthographic one.

### Where it draws, and what that costs

**After the tonemap, into the virtual target.** Two things follow, and both are decisions
rather than consequences:

- A sprite is not exposed, curved or temporally resolved, so the texel in the file is the
  texel in the swapchain. That is the P arc's whole standard and it is what
  `scripts/readback.sh`'s `sprite` case proves in texels — and its `sheet-cell1` and
  `sheet-cell2` cases prove for a *cell* of a sheet, since P5 added no pass and no shader
  here: an animated sprite is a sprite whose UV rect is rewritten when the frame changes.
- A sprite is therefore **not occluded by 3D geometry, not bloomed, not reflected by SSR
  and not fogged.** In a scene that is sprites, none of those has a meaning. In a scene
  that is not, the row for it is P6's lit sprite, which goes through the G-buffer and the
  lighting pass and by construction carries no pixel-exact guarantee. The two are opposite
  trades and that is why they are two passes rather than one with a flag.

Always into the *virtual* target at `renderExtent`, the way the debug lines are: a sprite is
world-space content and scales with the world. Only the overlay gets `uiInsideVirtual`'s
choice, because only the overlay is screen-space by construction.

There is no depth attachment and no depth test. The CPU sort **is** the order; a blended
surface cannot depth-write without occluding what is behind it, so a depth test would buy
only the interaction with 3D geometry this pass has already declined.

### One draw, not one per layer

One blend state makes one global sort possible, and one global sort makes one draw
possible — which is the argument `particle.frag` already carries, arriving at the same
answer one subsystem along. So a layer is purely a sort key: `SpriteTable` emits its array
in layer order then creation order, and `vkCmdDraw(cmd, 6, count, 0, 0)` draws all of it.

### The UV rect is in texels, and the division happens in the fragment shader

Normalised coordinates in a public API are where half-texel errors come from, and an atlas
an artist authored is measured in pixels in the tool that made it. So `SpriteDesc::uv` is
`{x, y, width, height}` in texels, and `sprite.frag` divides by `textureSize(images[i], 0)`.

That location is forced rather than chosen. `ImageTable` holds no `VkImage` — that is P1's
split — so nothing on the CPU side of the boundary knows how many texels a file turned out
to have. Asking the shader is also the answer that cannot drift: an atlas re-exported at a
different size needs no number in the game changed, and there is no CPU-side copy of an
image dimension to keep in step. A zero width or height means *the whole image* and is
resolved in the same place, for the same reason.

At 1:1 the fragment centre of destination texel `k` lands on `rectMin + k + 0.5`, which is
the centre of source texel `k`, so a nearest tap returns that texel and nothing else. What
puts a nearest sampler there is `pixelExact` — the overlay's image array is the sprites'
image array, and that switch is what chooses its filter.

### The upload is once per revision per frame slot, not once per frame

`SpriteTable::revision()` is the fourth counter of its shape — `InstanceTable`'s, the
scene's `materialRevision()` and `ImageTable`'s are the other three — and `recordSprites`
compares it against what each frame in flight last received. A screen of sprites that did
not change costs **no copy at all**; one where anything changed costs the whole array.

**Whole-array or nothing, and that is the decision rather than a shortcut.** A dirty
*range* would have to be a range per frame in flight, since each slot last uploaded at a
different revision and what each needs is the union of every range since — and it would buy
a scattered set of small writes into write-combined memory where there was one linear one.
`InstanceTable` declined the same trade for the same reason. Skipping the copy entirely is
what no range can beat, and it is what a static screen now does.

Every path that can invalidate a buffer without the table's counter moving zeroes that
slot's copy instead: `ensureSpriteCapacity` (the allocation is destroyed), `setSprites` (a
new table is a new revision space) and `destroyFrameResources`. On the table's side the
eight per-sprite setters all reach their entry through `SpriteTable::at`, which bumps on the
way in, so a ninth cannot be written that forgets; `create`, `destroy`, the sort, the sheet
frame in `applyFrame` and the image reconcile in `prepare` write `gpu` directly and bump
where they do. **`applyFrame` bumps inside its own guard**, so a flipbook at 12 fps on a
60 Hz step still uploads on the one step in five that changes cell and not the other four.

### What it costs

`--sprites N` is a run mode, so the number is reproducible from a shell rather than from a
game somebody has to write, and `--sprites-move` is the second arm: it nudges every sprite
by a fraction of a texel every frame, so the table's revision moves every frame and the pass
pays its whole copy. Both arms draw the same overdraw. 1600x900, 1x MSAA, release, medians
over four runs of 900 frames per arm, `Renderer::record/Sprites` from `--zones`:

| | 1,000 | 10,000 | 50,000 |
|---|---|---|---|
| `Renderer::record/Sprites`, moving | 0.005 ms | **0.036 ms** | **0.176 ms** |
| `Renderer::record/Sprites`, static | 0.001 ms | **0.002 ms** | **0.002 ms** |
| `Renderer::record`, moving | 0.065 ms | 0.097 ms | 0.237 ms |
| `Renderer::record`, static | 0.060 ms | 0.061 ms | 0.062 ms |
| `Sprites` (GPU) | 0.007 ms | 0.049 ms | 0.278 ms |
| `Lighting` | 0.034 ms | 0.034 ms | 0.035 ms |
| `Frame` | 0.319 ms | 0.365 ms | 0.599 ms |

The moving arm is what every sprite cost before the gate existed, to a thousandth of a
millisecond, and it is the whole of the GPU-side story unchanged: `Sprites` and `Frame` are
the same in both arms, because the copy is CPU work feeding a draw whose cost is overdraw.
What moves is `Renderer::record`, and by a lot — at ten thousand the copy was **a third** of
everything the renderer records in this scene, and at fifty thousand **three quarters** of
it. The 640 KB figure is unchanged and is what a *changing* screen pays; a static one pays
nothing, which is O(changed) where it used to be O(live).

`Lighting` not moving across any of the six arms is the check that the pass really is
downstream of it, and the CPU figure is still **not a sort**: a position is not part of the
sort key, so moving every sprite every frame re-sorts nothing.

---

## Lit sprites

The other half of the sentence above, and **it is not a pass.** A lit sprite is a quad with
a material, drawn by `recordGBuffer` and the shadow passes like every other surface, so
there is no `recordLitSprites`, no lit-sprite pipeline and no lit-sprite table. The whole of
it is `scene::quadMesh` (hosted), `engine/shaders/sprite_lit.frag`, `sprite_lit_shadow.frag`
and one `gfx::ShaderVariant` the engine registers on the first `Engine::createLitSprite`.

`e.createLitSprite(desc)` returns a `GltfScene::ModelId` and `e.removeModel(id)` frees it —
the same allocator, the same range coalescing, no new lifetime model.

### It exists because the two guarantees are mutually exclusive

An unlit sprite is bit-identical to its file and is occluded by nothing. A lit sprite is
occluded, shadowed, ambient-occluded, reflected, fogged, temporally resolved and tonemapped,
and is therefore bit-identical to nothing. **No design gives one draw both**, so they are
two paths, each stating the other's trade rather than only its own. A `bool lit` on
`SpriteDesc` would have moved a draw between two passes with different attachments,
different blend states and different sort orders — and would have made the readback's
guarantee conditional on a field, which is not a guarantee.

### A cutout, not a blend, and that is what buys the rest

ALPHA_MODE `MASK`. A blended lit sprite would be drawn forward, after lighting, and
`InstanceTable::dynamicCount()` excludes blended instances — so it would write no velocity
and get no TAA motion correction. A hard alpha edge is what pixel art has anyway; one
`discard` in `sprite_lit.frag` buys depth, velocity, occlusion, SSAO, SSR, fog and a
silhouette cut out of the shadow map. Blended lit sprites are not implemented; see
[limitations.md](limitations.md).

### The image array is bound into the scene's pipelines, which is the one renderer change

`gfx::ImageTable` had two callers — the overlay (C5, promoted by P1) and the sprite pass
(P4). The G-buffer is the third, so `overlaySetLayout` is **set 2** of the `gbuffer` and
`shadow` pipeline layouts and `overlaySet` is bound beside the frame set and the scene set.
`GpuMaterial::gameImage` is a slot in it — the word that used to be
`occlusionTextureUnused`, renamed rather than added so no offset moved.

Two consequences beyond sprites:

- **A game shader variant can now sample a game's own texture.** G5 gave a game its own GLSL
  and no way to give that GLSL an image; set 1 holds only what a glTF brought.
- **The array's ceiling now leaves a reserve.** `maxPerStageDescriptorSampledImages` is a sum
  over *every set in a pipeline layout*, so an array declaring the whole device limit cannot
  be bound beside anything. It was free while the array was bound alone and became four
  `vkCreatePipelineLayout` failures the moment it was not. `createDescriptorLayouts`
  subtracts 4096 for what the sets beside it may declare — chiefly `GltfScene`'s own texture
  array, whose count is not known until a scene loads. The ceiling is now what the array may
  grow *to* rather than what its layout declares, which is a separate repair below.

The alternative was to put sprite images into `GltfScene`'s array through
`acquireTextureSlot`/`bindTexture`. It is refused because that free list has **no
generation**: it is the one lifetime C1 did not convert, and building the 2D content path on
the engine's one known-broken lifetime model would be the worst available answer.

### The UV rect is on the material, not in the geometry

`sprite_lit.frag` reads `material.params` as `{x, y, width, height}` in texels and divides by
`textureSize`, exactly as `sprite.frag` does and forced by the same thing: `ImageTable` holds
no `VkImage`, so nothing CPU-side knows the file's dimensions. The quad's own UVs are the
0..1 corner.

That is what makes an animated lit sprite one call —
`e.setLitSpriteUv(id, e.sprites().frameUv(sheet, cell))`, driving it off P5's slicing — rather
than new geometry every frame. Its cost is that a lit sprite showing its own frame needs its
own material, and material slots are never reclaimed.

### What it costs

Both arms are the readback case's own camera and scene, 960x540, 1x MSAA, release, medians
over 6 runs of 600 frames. They differ in one number: `--readback-lit-cutoff 2` puts the
cutoff above every alpha in the file, so the sprite's fragments all discard while the
material, the instance, the indirect command and the pipeline stay identical.

| | discarded | drawn |
|---|---|---|
| `GBuffer` | 0.041 ms | 0.042 ms |
| `Lighting` | 0.033 ms | 0.036 ms |
| `Frame` | 0.458 ms | 0.462 ms |
| CPU busy | 0.139 ms | 0.139 ms |

One sprite covering 8,424 virtual texels is **+0.004 ms of `Frame`**. A scene with no lit
sprites pays nothing at all: the row adds no pass and no draw, only one more set in
descriptor-set binds that were already being made, and the golden suite is byte-identical
across the change.

---

## The deformed vertex buffer has three producers, and one of them is a copy

`skinnedVertices` holds one `Vertex` per vertex of every deformed *instance*, and
`Renderer::skinDestBase` says where each instance's range starts. Two producers write it on
the GPU: skinning and morph-target blending, both dispatches of `skinning.comp`. The third
is cloth (C19), and it is a **`vkCmdCopyBuffer` from a host-visible staging buffer** — the
solve is Jolt's and runs on the CPU inside the fixed step, so the vertices exist in system
memory and have to get across.

Three things make that a third producer rather than a second buffer:

- **The ranges are disjoint by construction.** `skinDestBase` hands every deformed instance
  its own, so the copy and the dispatches never touch the same bytes and need no barrier
  between them — only the one after, which now names `COMPUTE_SHADER | COPY` as its source
  scope instead of compute alone.
- **`drawSceneIndirect` needs no change at all.** It switches the bound vertex buffer on a
  command's *position* in the list — static half, then deformed half — not on any per-instance
  flag, so a cloth command that lands in the deformed half draws out of the right buffer for
  free. Every geometry pass, both shadow paths and the dynamic BLAS refit inherit it the same
  way.
- **One bit says which producer.** `kInstanceCloth` is OR'd into `kInstanceDeformed`, so a
  cloth instance gets its `skinDestBase` range, its infinite culling box and its skipped LOD
  chain from code that never mentions cloth. The one site that tests the bit by name is the
  command build, which must *not* record a `SkinBatch` for it: there is no dispatch to run.

The staging buffer is one per frame in flight, host-visible and mapped, sized from the
scene's cloth vertex count — the pattern debug lines, sprites and particle spawns all use,
and deliberately not `gfx::Uploader`, whose own header says every submit blocks and it is
"fine for load time, never for the frame loop". A scene with no `FABRIC_` mesh allocates
none of it.

**The allocation gate moved.** Before cloth, `skinnedVertices` existed only when the scene
had a rig, because a rig was the only thing that deformed. A scene whose only moving geometry
is a curtain has no rig, so the test is now "does anything deform" and the animator is allowed
to be null all the way through the buffer's creation.


## MSAA and the deferred problem

MSAA rasterizes coverage at N sample points but runs the fragment shader once per pixel.
Deferred shading moves lighting to a fullscreen pass, by which time a pixel is N samples
that may straddle a geometric edge — and lighting is a **nonlinear** function of surface
attributes, so `average(light(x)) != light(average(x))`.

Resolving the G-buffer before lighting averages normals into directions no surface faces
and depths where no geometry exists, producing halos exactly along the edges MSAA was
meant to fix. Substrate therefore **keeps the G-buffer multisampled and shades every
sample**, averaging the resulting radiance.

That scales almost linearly — 1.99x / 3.84x / 8.18x measured at 2/4/8 samples — which is
what `ENABLE_EDGE_MSAA` exists to claw back.

### Edge-detect hybrid shading

`samplesAgree()` shades once where every sample holds the same fragment. **A branch, not a
stencil pass**: stencil needs D32S8, eight bytes per sample where depth is now four, which
would spend most of what octahedral normals saved.

The classifier compares albedo and normal and deliberately **not** depth, which is
interpolated per sample and would flag the whole screen. Measured **-8.5% at 4x and -24.9%
at 8x** on the lighting pass — against a ~28% prediction, because a densely tessellated
scene genuinely does have two fragments in most pixels. The `edges` debug view shows why.

**Precomputing the classification into a mask was built, measured and reverted.** The obvious
saving is that `samplesAgree` costs `2(SAMPLE_COUNT - 1)` multisample fetches — six at 4x — on
every pixel including the four in five that then shade once, so evaluating it once into an R8
image should leave the lighting pass reading a single texel. It does, and the mask is bit-exact:
`--debug-view edges` was byte-identical across the change, and all thirteen golden cases held.
It is also **slower**. Nine runs per arm: `Lighting` moved 2.799 → 2.789 ms, a 0.010 ms change
against a run-to-run spread of 0.017, while the new pass cost **0.057 ms** and `Frame` rose
5.121 → 5.164 (+0.84%).

The six fetches are not merely not-slower where they are, they are **nearly free** — they read
`gAlbedo` and `gNormal`, which the shader is about to read anyway, so the lines are resident and
the latency hides under shading. Given their own pass they lose that residency and pay a render
pass, two barriers and 1.4 MB of write-then-read on top, for a 6x cost.

Two things this establishes for anyone tempted again. **`Frame` is the number that decides it,
not `Lighting`** — any implementation necessarily moves work out of `Lighting` into a new zone,
so a `Lighting`-only comparison shows a win of 0.010 ms while the frame gets 0.043 ms longer.
And the mask would not live purely under `render.edgeMsaa`: with `--no-edge-msaa` the
`debugView == 7` branch still calls `samplesAgree`, so a mask needs a gate of
`edgeMsaa || debugView == Edges` — one more piece of state than the row that already exists.

### TAA, and why it is not simply better

TAA distributes samples over *time*, so it antialiases shading, specular, shadow-map
stair-stepping and SSAO noise at roughly flat cost — **0.098 ms at every sample count**.
1x + TAA runs ~2.5 ms with edge quality comparable to 8x MSAA at 4.95 ms.

It is **off by default**, and that is the point rather than an oversight: it makes the
frame a function of the last several rather than of this one, which is the determinism the
golden-image suite rests on. It converges to a bit-exact period-8 cycle, so a golden image
at a fixed frame still works.

If adopted, the target is **TAA + 2x MSAA** rather than TAA alone: MSAA holds silhouettes
stable in exactly the disocclusion cases where TAA's history is rejected.

Half the shading-aliasing gap is closed deterministically and for free by
`ENABLE_GSAA` — Kaplanyan roughness clamping from normal-map variance, folded into the
roughness the G-buffer already stores, costing two derivatives and no extra pass.

**The history and the matrix that reprojects into it belong to a view, not to a frame.**
`taaHistory`, `taaHistoryIndex`, `taaHistoryValid` and `prevViewProj` are all members of
`Renderer::View`. Reprojecting one view against another's previous matrix is a smear that
only appears once the camera moves — which is to say it looks correct in every still frame
anyone would check it in.

**Both sides of the reprojection are unjittered**, and that is the load-bearing detail.
Reconstructing with the *jittered* inverse reads correctly — it is the transform that
actually produced this depth — but the previous frame is projected with an unjittered
matrix, so the two disagree by exactly the jitter and the history is resampled half a pixel
away every frame, compounding into a blur. Unjittered on both sides maps a static camera's
pixel to itself exactly, so the only thing the jitter moves is where *this* frame sampled
the scene, which is the whole mechanism. Convergence toward an 8x reference does not
distinguish the two; the image's high-frequency energy does.

### Motion correction for dynamic geometry

There is **no motion-vector attachment**. The camera's contribution to every pixel's
motion is exactly recoverable from depth and two matrices, and it is the same for static
and dynamic surfaces alike; an attachment at the G-buffer's sample count would cost ~23 MB
per frame to restate it per sample.

What `recordVelocity` writes is a **correction**, not a velocity: the difference between
where the depth reprojection will land and where the surface actually was. **Zero is the
identity**, so the clear value *is* the static-geometry path — no sentinel magnitude, no
per-pixel branch, and nothing to draw for a scene where nothing moves.

It is a separate single-sample pass because one render pass cannot mix sample counts, and
it tests against the same resolved depth the forward pass uses. `taa.comp` reads it with
`texelFetch` rather than `texture`: a bilinear tap straddles two surfaces along exactly
the silhouettes where the value matters most. **0.041 ms** at twelve draws.

---

## Shadows

### The sun: one map, and why not cascades

**One 4096-square D32 layer**, `kShadowMapSize`, fitted to the scene bounds and the sun
direction with **no camera term in its projection**. `updateSunShadow` deliberately takes no
camera. Sampled through a comparison sampler, so each of the 3x3 taps is a hardware 2x2 PCF
and the loop is 6x6 effective; over Sponza's ~37 m that kernel spans about 5 cm.

**Cascades were removed rather than tuned, and the reason is worth keeping.** Four cascades
were fitted to the *view frustum*, so every quantity that differed between two of them — the
world size of a texel, the world distance the depth bias pushed an occluder, the width the
filter kernel spanned — changed when a surface crossed a split. Which cascade a surface fell
in is a function of where the camera stands, so walking toward a wall slid the shadow across
it: same camera, cascade assignment the only difference, **24% of pixels changed with a peak
delta of 630**. That is not a bias wanting tuning and no crossfade hides it; it is the camera
term in the projection. One map removes it by construction, and one projection means one
bias conversion instead of four that could disagree.

**It costs no more memory.** Four 2048-square layers and one 4096-square map are both 64 MiB
at D32. What it accepts is stated rather than hidden: a scene much larger than its detail is
dense gets uniform texel density where cascades would have concentrated it. `shadowDistance`
caps the fitted box for that case, and an unbounded world would want cascades back — anchored
to the light rather than to the camera.

**Neither face is culled, and that is a parity decision rather than a quality one.** A ray
query has no notion of facing — a triangle is hit from either side — so a raster pass that
culled back faces would disagree with the traced path for exactly the geometry whose front
faces the light cannot see: single-sided walls, open shells, anything enclosing a light.
Depth is `LESS`, so the nearer face wins and closed geometry records the depth `BACK_BIT`
would have left anyway. Do not "optimise" it back; `ShaderVariant::cullMode` is ignored here
for the same reason. Slope-scaled depth bias plus a texel-scaled normal offset on top.

**One layered pass was tried and is 2x slower**, and the finding outlived the cascades that
produced it. Rendering four cascades into a `2D_ARRAY` attachment with `gl_Layer` was the
obvious saving of three pass boundaries and cost 0.61 → 1.14 ms: **depth compression and
hierarchical Z are per attachment layer**, and a four-layer attachment does not get the
treatment four separate ones do. That is still the reason the punctual atlas below renders a
layer at a time rather than in one layered pass, where it would bite 24 times over. Multiview
would be worse again — it broadcasts one draw to every view, forfeiting the per-view culling.

Zero in the baseline table, because ray tracing is on by default and `recordShadows` does not
run at all when it is; `--no-rt` is the switch that puts this path back, and the `no-rt`
golden case is what keeps it drawn.

### Punctual

A separate 1024-square D32 atlas, separate because it is a different resolution and a
perspective rather than orthographic projection. A spot takes one layer, a point six.
`updateLights` assigns them and writes one view-projection per layer into a storage buffer
that both `shadow.vert` and `shadow.glsl` index.

The shader picks a point light's face by major axis (`cubeFace`), in the same order
`updateLights` builds the matrices. **The two are one convention written in two places and
must be changed together**; nothing checks that they agree.

Lights that do not fit keep `params.w = -1` and light without occluding. Measured **0.270
ms** for 7 layers, and the atlas is cached between frames unless something invalidates it.

### Alpha-masked geometry, and the fragment stage

The command list is partitioned four ways — (static | skinned) x (unmasked | masked) —
specifically so the shadow pass can draw the unmasked run through a pipeline with **no
fragment shader at all**, which restores early-Z and the double-rate depth-only path for
the 87% of Sponza's triangles that are simply opaque. The G-buffer, which does not care
about the distinction, still draws two contiguous runs and needed no change.

### Ray-traced shadows

The traced queries live in `rayshadow.glsl` — `tracedSunShadow()` and
`tracedLightShadow()` — called from `lighting_body.glsl` behind `ENABLE_RT`, and from
`shadeRayHit()` for reflection hits. One file and one calculation on purpose: a surface
and its own reflection must agree about every shadow, and two copies of a ray query
drift. The punctual query sits behind the same `params.w >= 0` test in both places, so
the atlas assignment still decides *which* lights shadow (authored `castsShadows:
false` and the layer budget both land in `params.w`); the ray replaces the atlas
lookup, not the policy. No `rgen`, no miss shader, no binding table, no
`vkCmdTraceRays`, no second pipeline type. The sun ray alone measured **+0.180 ms
(+18.9%)** on the lighting pass.

`ENABLE_RT` is one constant, and `render.rt` one setting, where shadows, reflections
and sky visibility used to toggle separately. Supporting the splits would mean making
every pairing of traced and rasterised agree about every light — the mismatched
combination is how reflected shadows came to render differently from the world.

The whole bias apparatus disappears: one 0.02 nudge along the normal replaces slope-scaled
depth bias and the texel-scaled normal offset. It does *not* let the raster shadow pass be
skipped, because volumetric fog reads the map.

`--debug-view shadow` follows whichever technique is live. It used to call `shadowFactor`
unconditionally, which made the one view built to inspect the sun's shadow term the one
view that could not show the traced one.

#### The shadow mask, and why it is off by default

`shadowmask.frag` traces shadow visibility once per **distinct fragment** into an
`r32ui` array — a bit per light, 32 of them (`kShadowMaskLights`, the width of one texel and
deliberately not `lightBudget`; a scene with a wider budget keeps its extra lights and traces
them inline). The lighting pass then reads a bit instead of tracing. `render.rtShadowMask` and
`ENABLE_SHADOW_MASK` (specialisation id 7; **id 2 stays vacant** for whatever brings indirect
light back) gate it, and it is in `featureKey`, so toggling it live rebuilds the pipeline.

The mask runs **only where `samplesAgree` is false**, and the lighting pass's collapsed branch
still traces inline — so four fifths of the screen is bit-for-bit unchanged. On the rest, sample
`s` inherits the mask of the first earlier sample that matches it, else traces its own.

| | `Lighting` | `ShadowMask` | sum |
|---|---|---|---|
| 4x, off | 2.807 | — | 2.807 |
| 4x, on | 1.458 | 1.107 | **2.565** |
| 8x, off | 4.627 | — | 4.627 |
| 8x, on | 1.748 | 1.891 | **3.639** |

**0.24 ms at 4x and 0.99 ms at 8x** — 4.1x from one sample count to the next, exactly as the
shape predicts, and below the 0.4-0.6 ms that was estimated for 4x. The reason is measurable:
the mask pass classifies all 1.44 M pixels in order to skip 83% of them, which costs about
0.42 ms of the 1.35 ms of edge-pixel ray work it deduplicates.

**That 0.42 ms is not recoverable by classifying once**, which was tried. The lighting pass
re-runs `samplesAgree` on the same texels, and having the mask carry the answer instead — an
extra array layer at index `SAMPLE_COUNT` — bought **0.018 ms at 4x and 0.060 ms at 8x**, about
4% of the target, for 5.49 MiB allocated whether the row is on or not. The two classifications
never paid the same price: the mask pass's is a cold walk of the G-buffer, and the lighting
pass's runs microseconds later over texels still in cache. It is the same mechanism that made
the standalone R8 edge mask cost more than the fetches it replaced. It also costs 23.0 MiB at 4x and
46.1 at 8x, allocated at every configuration so the lighting descriptor stays valid.

**It is off by default because it is an approximation and six golden cases say so**: `lit`
moves 3691 pixels (max delta 194), `physics` 40, `skin` 18, `emissive` 12, `particles` 6,
`mirror` 1. Attribution is exact rather than inferred — each scene's `--debug-view edges`
capture *is* `samplesAgree`, and cross-referencing puts every moved pixel on a classifier-red
pixel or immediately adjacent to one. With the row off, `lit`, `physics` and `emissive` compare
0/1,440,000, so the refactor itself is neutral and only the approximation moves anything.

One measurement that corrects a common claim: **MSAA does buy antialiased shadow edges**, and it
is `ENABLE_EDGE_MSAA` that discards them, one pass earlier. The default image differs from
`--no-edge-msaa` at 13,768 non-edge pixels (max delta 184); turning shadows off collapses that
to 95, so 13,673 of them are antialiased shadow edges on flat surfaces. Depth is interpolated
per sample and each sample reconstructs its own `P`, which is why. The argument for the mask
survives — the quality it can lose is confined to pixels that fail `samplesAgree`, and the
measurement above confirms that is where all of it landed — but the AA is discarded rather than
absent.

Attribution note for anyone re-deriving the shadow cost: `--no-rt` also swaps SSR to the
screen-space march. The direct arm is `--no-rt-shadows`, which puts `Lighting` at 0.703 at 4x —
so traced shadowing is **2.11 ms**, not the 1.456 the `--no-rt` difference suggests.

### The ambient term is a constant, on purpose

Both the lighting pass and `shadeRayHit` take ambient from the same flat
`constantAmbient` — one number an author picks, defaulting to zero — attenuated by SSAO
and the baked occlusion texture where those exist. Two richer answers were built and
both went. A traced sky-visibility pass (`rt_ambient.comp`, a cosine-hemisphere gather at
four rays per pixel with a matching gather at reflection hits) was removed for its Monte
Carlo grain — blurred in the world image, raw in reflections — read as noise everywhere
ambient dominates, with "remove the feature" preferred over stacking denoising machinery
on top. The split-sum IBL lookup that preceded it went for being confidently wrong
indoors: the cubes are built from the sky and are scene-blind, so an enclosed arcade was
handed daylight weighted by a Fresnel term that peaks at grazing incidence, measured at
63% of every pixel in a shadowed bay. Nothing available could occlude it — SSAO reaches
`render.ssaoRadius`, half a metre by default, and the vault overhead is twenty units up.

**So unlit surfaces are black, and that is a floor rather than a finish.** Indirect light
belongs to something that can measure the room — baked irradiance, or traced sky
visibility with a real denoising plan — and black is honest until one exists in a way
sky-blue never was. Because the identical constant is added at reflection hits, a surface
and its reflection still agree.

**Removing the term did not remove its switch, and that cost a golden case.**
`render.ibl`, `--no-ibl`, an F9 binding and `ENABLE_IBL` all outlived the single `if` they
gated, so the flag flipped a setting, rebuilt a pipeline and moved no pixel in any scene —
and `golden.sh`'s `no-ibl` case pinned a byte-for-byte copy of `lit` while the suite
counted it as coverage. All of it is retired; `features.glsl` id 2 is vacant and says why.

One piece of the split-sum chain outlived it: `envBRDF`. It was factored out so the
reflection pass could apply exactly the term the lighting pass removed where the two
composite — the double-count that read as a milky film on every smooth surface. With no
prefiltered specular left to cancel against, it is simply how traced reflection radiance
is weighted, and `ibl.glsl` is down to that and the skybox lookup.

---

## Lighting

### The light model

One flat `GpuLight` — position/range, direction, colour/intensity, cone cosines + type +
shadow layer — in a per-frame **storage buffer**, so the count is a number the shader
reads rather than a constant the descriptor layout was built around. Falloff and cone ramp
follow `KHR_lights_punctual`, in `lightRadiance()` in `shadow.glsl`, shared by the
deferred and forward paths.

**A game authors lights the same way a file does** (D20). `GameSetup::look.lights` is a
`std::vector<GpuLight>` and the sun is one of them, built with `makeDirectionalLight`; it was
three fields — `sunDirection`, `sunColor`, `sunIntensity` — until D20 pointed out that the
engine converted them into a `GpuLight` to shade with and converted a scene's directional
light back into them at load. One concept, three representations, two conversions.

`Engine::initLights` walks the scene's lights and then the game's, and **the first
directional light becomes the sun** — taken out of the list rather than left in it, because
`updateLights` puts it back at the head of the buffer. **A second directional light is
dropped and reported**, not demoted to an ordinary entry: the shader routes every directional
light through the sun's cascades, so keeping it would shade it against a shadow map built for
something else. Scene before game, so a file that ships its own sun wins over one a game
authored — which is what `scripts/golden.sh` relies on when it runs a game against eleven
scenes that are not its own.

The three renderer fields `sunDirection`, `sunColorValue` and `sunIntensity` still exist and
are what the cascades and the sky read; they are **derived** now rather than authored.

**`GpuLight::direction` is asymmetric on purpose**: for a directional light it holds the
vector *toward* the light; for a spot, the direction it points. A sun is authored by where
it is, a spot by where it aims. Negating the directional one — which reads as the obvious
thing to do — lit every surface facing away from the sun, and did so for three tiers.

### The budget

`gfx::kDefaultLightBudget` is where the light buffer starts, and it grows rather than capping, so
raising the budget is a config edit rather than a recompile. Over budget, `updateLights`
ranks by `lightImportance()` and reports how many it dropped, once per change.

Two decisions worth reading. The ranking runs **only when the budget binds** — under it
every light is kept, and reordering a set nobody is dropping from would change the order
radiance accumulates in, which moves pixels for nothing. And the survivors are emitted in
**scene order**, not importance order, so the frame stays bit-identical run to run.
`stable_sort`, so equal-importance lights resolve identically on every run.

The budget is clamped to what the buffer was actually allocated for, because a budget
raised after `init()` would otherwise memcpy past a mapped range — a silent overrun
strictly worse than the silent truncation the policy exists to remove.

### Tiled light assignment — and it is tiled, not clustered

`light_tile.comp` assigns lights to **16x16 screen tiles**, one workgroup per tile and one
invocation per pixel, writing a fixed-stride bitmask the deferred loop then walks. The tile's six
planes are its four screen-rectangle sides plus two depth planes built from the tile's own min and
max `gDepth` — **one frustum slab fitted to the depth actually present**, which is what makes it
tiled rather than clustered: a clustered (froxel) scheme subdivides z on a fixed schedule, and the
addressing here has no z coordinate at all (`(tile.y * tilesX + tile.x) * words`).

`frame.tileParams` carries x tiles across, y the tile size, z the mask words per tile — and
**z is zero exactly when assignment is off for this frame**, which is the one value both light
loops branch on. The buffer is per view, not per frame slot, so the dispatch takes a
write-after-read dependency in front of it: the previous frame's lighting draw is still reading
that view's copy. `render.lightTiles` is a live-bound bool rather than a specialisation
constant, so `featureKey` correctly has nothing to do with it.

**Iterating a fixed-stride bitmask low-bit-first is ascending light index, which is why the sum
stays bit-exact.** That is the property that makes the escape hatch meaningful: the same frame
with assignment off is byte-identical to it on, checked on Sponza, on `stress.gltf`, at 1x, under
TAA, with the shadow mask on, and at a 1024-light budget where the stride exceeds the words in
use. No compaction, no per-tile count, no atomic whose completion order is observable.

**It narrows hard, and `stress.gltf` is the wrong scene to see it on.** That scene's forty lights
carry `range: 12` on a ring of radius 5, so every point within radius 7 of the origin is reached
by all forty and there is essentially no residue to remove — its 3.0 ms is ray-traced shadows for
lights that genuinely reach. Changing only the authored range:

| `stress.gltf` range | `Lighting` on | `Lighting` off |
|---|---|---|
| 12.0 (as authored) | 3.034 | 3.039 |
| 6.0 | 1.694 | 1.769 |
| 2.0 | **0.229** | **0.593** |

At range 2 assignment removes **61% of `Lighting`**, byte-identically. On Sponza it gains nothing
and costs 0.057 ms, about 1% of frame, which is the machinery being cheap where it cannot help.

**Why the residue is small in the first place**, and it is the thing to know before reaching for
this: `lighting_body.glsl` already exits on `dot(radiance, radiance) <= 0.0` *before* the shadow
ray and before `shadeLight`, so an out-of-range light already costs one `Light` load and a few
flops. Tiling removes that residue and nothing else. It pays where a scene has many lights whose
reach does not cover the screen — not merely where a scene has many lights.

Above **1024** lights (`kLightTileMaxWords` × 32) the renderer refuses to assign and says so,
running the deferred loop over every light in the view rather than truncating a list a game asked
for. The ceiling is exact: at 1024 the pass runs and there is no warning.

`shadeRayHit` in `raytrace.glsl` is deliberately left on a flat loop — a reflection hit is not in
the tile's slab.

### `render.lightCutoff`, and why it ships at zero

The deferred light loop's early-outs are **bit-exact**: `shadeLight` returns exactly
`vec3(0.0)` when NoL ≤ 0, and `dot(radiance, radiance) <= 0.0` sheds a light that is out of
range or below the horizon, so both skips are provably not approximations. `render.lightCutoff`
adds a second, *approximate* test after them — and defaults to `0.0f`, which is today's
behaviour bit for bit. Nothing moves until someone raises it.

It is expressed **post-exposure**, divided out on the CPU once per frame and squared so the
shader keeps one compare against the `dot` it already computed. Exposure is per-game data
(`GameSetup::look.exposure`, and the demo slides it 0.1–4.0 at runtime), so the same absolute
radiance means a 40× different display brightness across that slider alone; `0.004`
post-exposure means "one 8-bit code value at the tonemap" in every game at every exposure.

**There is no shippable non-zero default, and the measurement is why.** On Sponza, `0.004`
takes `Lighting` from 2.792 to 2.716 ms (−2.7%) with the image unchanged, but the safe ceiling
is between 0.01 and 0.05 — at 0.05, 177 pixels move; at 0.1, 8452 move by up to 143/255. On
`stress.gltf` — 40 lights, the scene picked as the one that should gain most — `0.004` buys
0.4% and `0.1` buys 16.0% with no pixel over 2/255. **Safe headroom is scene-dependent by more
than an order of magnitude**, in opposite directions from the prediction.

The reason is the falloff window. Every `stress.gltf` light carries an authored `range`, and
the windowed `KHR_lights_punctual` falloff reaches exactly zero there, so the bit-exact test
already sheds every out-of-range light for free; what is left genuinely reaches the pixel.
Sponza's auto-placed lights have no such window. **The shape this row rewards is weak lights
with no authored range**, not "many weak lights".

And the cutoff **does not bound the error it causes.** It compares arriving radiance, but what
is dropped is `shadeLight`'s product — and `distributionGGX` at the roughness floor of 0.04
peaks near 1e5. That is how a light under 0.1 radiance moves a pixel by 143/255. The two
failure modes usually named for a threshold like this, dim light leaking through walls and
popping as a light crosses it, are both real; this one breaks first.

The row gates the deferred loop only. `forward.frag` and `raytrace.glsl`'s hit shading carry
the same exact early-out and are deliberately untouched, so a reflection and its surface still
answer the same question.

### The environment chain

`createIblResources` runs the whole chain once at startup through
`sky/irradiance/prefilter/brdf_lut.comp`. Split-sum after Karis: env 128², irradiance 32²,
prefiltered 128² across 5 roughness mips, LUT 256². There is **no per-frame work in it at
all**, and two of its four outputs now have no reader: the irradiance and prefiltered
cubes are still baked and still bound, and nothing has sampled them since the environment
term left the lighting pass. What is read is `envCube`, for the skybox and for a
reflection ray that escapes, and `brdfLut`, for `envBRDF`.

**Baked twice on a scene whose sun is not the renderer's default.** The chain is built from
`sunDirection`, and `Renderer::init` runs before a scene or a game has said what the sun is --
so the first bake uses the member's own initialiser, `{-0.35, 0.85, 0.4}`.
`Engine::initLights` calls `rebakeIblIfSunMoved` once it has resolved the real one, which
compares and re-bakes only if they differ. It replaces the four images and the descriptor set
naming them, and **not** `iblSetLayout`, which `init` creates and every pipeline baked into its
own layout.

It went unnoticed for as long as it did because `GameSetup`'s old `sunDirection` default was
that same vector, so a game that did not override it baked correctly by coincidence. What it
cost was a scene shipping its own directional light: `mirror.gltf`'s spheres reflected a sky
lit from where the sun was not, and correcting it moved that one golden baseline and no other.

**It ships a procedural sky, not an HDR file.** No HDR asset exists in the repo and adding
one brings a licence with it, so `sky.comp` generates the environment analytically. This
is a *feature* for the golden suite — it is bit-identical between runs — and the rest of
the chain would accept an equirect HDR unchanged. Only that one shader is replaced.

### Skybox

`skyboxRadiance()` sits in the branch the deferred lighting shader already takes for
"nothing was written here", sampling the same cubemap the IBL chain was built from. **It
needed no pass**: a fullscreen pass that already runs beats a cube drawn at the far plane.

---

## Screen-space effects

| Pass | Notes | Cost |
|---|---|---|
| **SSAO** | 16 samples on a golden-angle hemisphere rotated by a per-pixel hash, 3x3 box blur, **half resolution**. Reads **depth only** and reconstructs the normal from position derivatives — deliberately, because `gNormal` is multisampled and binding it would force a second shader variant. Multiplied into the glTF occlusion term, not `max()`'d: baked detail and live geometry are different things | 0.150 ms |
| **Bloom** | Soft-knee threshold to half resolution, 13-tap Jimenez downsample, 3x3 tent upsample accumulating into mip 0. Stays in `GENERAL` for the whole chain rather than flipping layout per step | 0.091 ms |
| **SSR** | Linear world-space march with binary refinement, gated on roughness, weighted by Fresnel, faded at the screen edge. Runs after the forward pass so a ray can sample blended surfaces, and before bloom so a reflection glares. Resolution is `render.ssrScale` | 0.430 ms |
| **RT reflections** | Same `ssr_body.glsl`, ray query replacing the march, and the hit shaded from its own geometry through `shadeRayHit`. On a miss it samples the environment cube, which SSR cannot do at all — and that is the *answer* for an escaping ray, not a fallback | ~0.5 ms |
| **RT sky visibility** | `rt_ambient.comp`. Cosine hemisphere against the TLAS; a surviving ray takes the environment radiance in its own direction, a blocked one takes a bounce off what blocked it. Half resolution, four rays. Replaces the diffuse half of the environment term | 2.1 ms |
| **Volumetric fog** | Marches the view ray to the depth buffer testing the sun's shadow map, Henyey-Greenstein phase, exponential height falloff, dithered per pixel so 32 steps do not read as 32 bands. Composited as `src + dst * (1 - src.a)` — additive alone would light the fog without dimming what is behind it. Deliberately does not call `shadowFactor`, which offsets along a surface normal, and a point in mid-air has none. **Off by default**: media is a property of a scene | 0.773 ms |
| **Decals** | Fullscreen per decal, reading resolved depth and discarding outside the decal's unit cube. Albedo only — projecting a normal map means blending octahedral coordinates, which is meaningless across the encoding's fold | 0.038 ms for two |

### `render.ssrScale`, and where the SSR zone's time actually goes

The zone is **93% trace dispatch** — 0.528 ms of 0.570 with ray queries on, and 86% of 0.275 in
the `--no-rt` march arm. The composite draw is a fixed 0.035-0.038 ms floor and both barrier
groups together are 2 microseconds. This is worth stating plainly because the reference said the
opposite for a while, on a measurement taken before the ray-query variant existed: `ssr_rt.comp`
traces a real ray and shades the hit, which is 0.29 ms more than the depth march, and it is what
made the zone ray-bound.

`render.ssrScale` is therefore a resolution fraction on `ssrTarget` and its dispatch, 0.25-1.0,
defaulting to 1.0. It rebuilds targets rather than pipelines, the way `render.msaaSamples`
already does; the dirty check compares the **extent produced**, not the float, so dragging a
slider rebuilds once per pixel step rather than once per frame. At 1.0 the extent arithmetic
returns the same integers and the default path is byte-identical.

At 0.5: `SSR` **0.577 → 0.286 ms (−50%)**, `Frame` 5.200 → 4.862 (−6.7%), VRAM −9.3 MiB. The
trace falls 0.528 → 0.226 and the composite rises 0.038 → 0.084, so the joint-bilateral upsample
costs 0.046 and the trace gives back 0.302.

**0.5 is recommendable; 0.25 is an escape hatch.** On `mirror.gltf`, whose floor is roughness
0.02, half resolution moves 2.51% of pixels with a mean delta of 0.49/255, and nothing above
y=415 moves — every moved pixel is on a surface below the 0.4 roughness cutoff, exactly as the
design predicts. At 1:1 the capture is indistinguishable. At 0.25 the moved pixels nearly double
and the count above 128/255 goes 598 → 1452, which is where the stepping becomes the reflection's
dominant edge.

**Both of those figures are at `--compare-tolerance 2`, the engine's default, and the sentence
does not survive tolerance 0**: 12.69% of pixels move, the topmost moved row is y=343, and 4651
pixels above y=415 move by 1-2/255. The claim is about visible change, not about bit-identity.

#### Does it crawl? Measured: no, and the mean is blind to the question

Every figure above is a still frame, and a staircase that sits still is invisible while one that
swims a pixel per frame is the classic half-resolution tell. `scripts/ssr_stability.py` orbits a
camera over `mirror.gltf` and takes the mean absolute difference between consecutive frames,
restricted to the reflection band — where the band is **measured per frame** as the pixels that
change when `--no-ssr` is passed, dilated by 4 px, rather than located by a scanline that stops
being true the moment the camera turns.

| arm | band | control | ≥8/255 | p99 |
|---|---|---|---|---|
| 1.0, TAA off | 0.8902 | 0.1878 | 1.22% | 12.0 |
| 0.5, TAA off | 0.8635 (**0.97x**) | 0.1878 | 1.84% | 22.2 (**1.85x**) |
| 0.25, TAA off | 0.9122 (1.02x) | 0.1878 | 1.89% | 26.4 (2.20x) |
| 1.0, TAA on | 0.8501 | 0.1831 | 2.36% | 23.5 |
| 0.5, TAA on | 0.8326 (0.98x) | 0.1831 | 2.59% | 23.7 (1.01x) |

The control is the complement of the band — pixels `ssrScale` cannot reach — and it is
**bit-for-bit identical across all three scales**, 0.1878, which is what frame-to-frame change
from camera motion alone looks like on this scene.

**0.5 does not crawl, and the qualification is that the mean cannot see whether it does.** On the
same pixels in the same frames, 0.5's mean is 0.97x while the 99th percentile of that same
distribution is 1.85x, and the share of band pixels taking a ≥8/255 step each frame rises from
1.22% to 1.84%. A mean cannot distinguish an edge sliding smoothly across four pixels from one
that waits three frames and jumps four — and hold-then-jump is exactly what a staircase locked to
a reduced grid does. So the staircase *does* move; it carries no more total change per frame than
full resolution's smoothly-sliding edge, concentrated into about 0.24% of the frame. The extremes
are untouched, and p99.9 actually falls (188.7 → 148.4).

**TAA equalises rather than fixes.** It closes the p99 gap from 1.85x to 1.01x — by raising *full
resolution's own* tail from 12.0 to 23.5 with jitter, up to where half resolution already was.

The upsample is four taps, bilinear weights times a **relative** reverse-Z depth weight at 5%
tolerance, falling back to the nearest texel when every neighbour is rejected. The depth term
earns its 0.046 ms: forcing the tolerance to infinity — plain bilinear — moves 13% more pixels
and 23% more mean error.

**What the bilateral cannot fix, by construction.** It rejects neighbours across a depth edge of
the *reflecting* surface; an edge inside the reflected *content* carries no depth signal at the
reflecting pixel. That is the visible artefact at magnification: the reflected building
silhouette inside a mirror sphere is a 2-pixel staircase at 0.5 and a 4-pixel one at 0.25. The
card that added this predicted bleeding across reflecting geometry, which the filter handles;
this is the failure that survives it.

`ssr_body.glsl` declares its `local_size` explicitly. glslang defaults an unspecified one
to a single invocation per group, which quietly reduces a fullscreen dispatch to the
top-left corner — and on a scene that is mostly rough stone, a reflection pass covering a
fraction of the frame is indistinguishable from one that found little to reflect.

**Reflections shade their hits.** `raytrace.glsl` turns a committed intersection into
geometry and a material — `instanceCustomIndex + geometryIndex` indexes a hit-record table
built beside the BLAS geometries, and the vertex, index and hit-record buffers arrive as
device addresses in the caller's push constants rather than as three more descriptor sets.
The reproject-into-screen-space step that used to stand in for this is gone, and with it
the blind spot it inherited from SSR: a ray that finds geometry the camera cannot see now
returns that geometry rather than sky.

**The structure is rebuilt at most once a frame, and never at the call site that dirtied
it.** `setInstances` and `instancesGrew` set a flag; `rebuildAccelIfStale` consumes it from
`Engine::endFrame`, which runs before that frame's `drawFrame` — so the structure a frame
traces always describes that frame's instances, and a game creating twenty props in a loop
pays for one build rather than twenty. It used to build on the spot, and the demo's
`Game::init` was 82% acceleration-structure rebuilds because of it. The same deferral
applies to `createPipelines`: only the *first* `setScene` builds eagerly, because until one
scene exists there is no descriptor layout to build a pipeline layout from; every later one
marks `pipelinesDirty`.

**An instance a game moves must say `kInstanceDynamic`, and the cost of not saying it is a
full rebuild every frame.** `createMesh` builds every instance static — nothing in a mesh
says it will move — so an instance attached to a dynamic body and left unflagged is baked
into the static tier, falls out of it on the next step, and `staticTierStale` reports it
forever. `Engine::addInstance` takes `scene::InstanceMotion` with no default for exactly
this reason; a caller reaching `InstanceTable::create` another way says so with
`setFlags(id, kInstanceDynamic, 0)`. The renderer logs the case once and names the flag.

**The reflection ray and the reflection march are bounded by different numbers.** They were
one — `ssrMaxDistance`, a march budget that divides into a fixed step count — and a ray
query inheriting it stopped at eight metres, so a polished sphere reflected a bubble of
scene surrounded by environment cube. `rtMaxDistance` defaults to 200; a ray query costs
the same at any range, so there is nothing to buy by keeping it short.

**Every world-unit length either pass reads is a settings row**, and none of them derives
from the scene. `render.ssrMaxDistance` is the march budget, `render.ssrThickness` how far
behind a surface a march may still count as a hit, `render.ssaoRadius` the occlusion
hemisphere and `render.ssaoBias` the offset that stops a flat wall occluding itself. The
last three had no key at all until D11, so a scene at a different unit scale needed a
recompile; none of them scales with the bounds, because contact occlusion is contact scale
at every scene size and a wall's thickness is a property of the content.
[tooling.md](tooling.md#configuration) has the table of which world units derive and which
are rows, and rule 7 in [principles.md](principles.md#a-world-unit-is-derived-or-it-is-a-row-and-never-a-literal)
the rule behind it.

---

## Submission

### The instance table drives everything

Recording is **one `vkCmdPushConstants` per pass and one `vkCmdDrawIndexedIndirect`**.
Per-draw push constants are gone entirely — vertex shaders read
`instances[gl_InstanceIndex]`.

Measured **0.108 → 0.076 ms** on Sponza's 103 objects and **1.565 → 0.072 ms** on 4097.
CPU recording is O(passes), not O(draws x passes).

Two device features are **checked at device creation rather than assumed**:
`drawIndirectFirstInstance` (needed, and originally not on the list) and
`multiDrawIndirect`. `shaderDrawParameters` is *not* needed — Vulkan's `gl_InstanceIndex`
already includes `firstInstance`. They are checked rather than assumed because without
`drawIndirectFirstInstance` every draw resolves to instance zero: a whole-scene symptom
with a one-line cause and no diagnostic attached to it.

### Instancing

Fell out of the command builder with **no new type and no new shader**: the builder
extends the previous command when the next slot draws the same geometry, and
`gl_InstanceIndex` counts up from `firstInstance` across the run. 4097 objects submit as
**2 draw calls**, at GPU cost identical to the per-draw path — so the saving is entirely
CPU and entirely real.

Runs are capped at `kMaxInstancesPerCommand = 64`, and that cap is a **culling** decision
rather than a submission one: a command is the unit the cull can switch off, so its bounds
are the union over its run, and an unbounded run is one un-cullable blob.

### GPU culling

`engine/shaders/cull.comp`, one thread per indirect command, dispatched once per *view* — the
camera, the sun, and up to `kMaxShadowLayers` = 24 atlas layers, which is `kCullViews` = 26.
**0.010 ms.** Sponza drops 103 → 78 camera instances and 1133 → 684 shadow instances. A
second registered view adds one more dispatch, into list 0, and no more than one.

Three decisions:

- It does **not compact**. An atomic-append cull makes command *order* depend on which
  thread won, and this renderer is bit-identical run to run; culled commands keep their
  position and get `instanceCount = 0`.
- The test is in **clip space** rather than against extracted planes, because the reverse-Z
  infinite projection has no far plane for a plane extractor to find.
- **No octree.** A tree accelerates a CPU testing objects one at a time; a GPU tests all
  of them at once. It is worth having for picking, physics broadphase and audio occlusion,
  which are subsystems, so it is delegated there.

### Mesh LOD, in the same dispatch

The third test in `cull.comp`, after the frustum and the Hi-Z pyramid, and the only one
that changes what a surviving command *draws* rather than whether it draws. `screenCoverage`
projects the same eight corners, takes the area of the screen-space rectangle around them
as a fraction of the viewport, and `selectLod` walks that against thresholds the CPU pushed;
the selected level's `(firstIndex, indexCount)` replaces the command's own.

- **A level is a second range of the same index buffer, over the same vertices.**
  `meshopt_simplify` returns indices into the original vertex array, so a chain costs
  indices and no vertices, and every level draws through the pipeline, the descriptor set
  and the vertex buffer LOD 0 draws through.
- **Optional per mesh, and zero levels is the default.** `Primitive::lods` is an inline
  array of three ranges with a count of how many are real; a mesh with none has one level
  and the selection can only return it. Nothing in a glTF authors a chain and nothing
  requires one.
- **The chain rides inside the record it describes** — `Primitive` on the CPU,
  `GpuCommandBounds` on the GPU. A per-command LOD array laid out beside the command array
  is the shape that gets out of step the moment a run merges, a variant sweep reorders the
  list or `appendModel` grows `prims`, and level 0 in the GPU record is copied out of the
  indirect command written in the same statement rather than out of a second description
  of it.
- **The camera view only.** The sun's shadow projection is orthographic and covers the world
  rather than the screen, so "what fraction of the viewport does this cover" is not a question
  it can be asked, and a caster that dropped a level while the surface it shades kept its own
  is a shadow that stops fitting what casts it.
- **Never for deforming or blended geometry.** A skinned command draws out of the buffer
  `skinning.comp` wrote, whose contents are its bind-pose vertices; a blended one is built
  by the forward pass on the CPU and never reaches this dispatch at all.

`--no-lod` and `render.meshLod` turn it off; `render.lodThreshold` is the coverage below
which LOD 1 is selected, and each level below that is a quarter of it — a level halves the
triangle count, and halving the linear size on screen quarters the area, so the sequence
holds a roughly constant triangle-per-pixel density.

**Chains are built by the bake, never at load.** `scene::buildLodChains` runs inside
`substrate-bake` and writes into the C15 sidecar; a load from a document has no levels and
the selection resolves to LOD 0 for everything. See
[systems.md](systems.md#the-scene-sidecar-and-the-lod-chains-it-carries) for the format and
[limitations.md](limitations.md#lod-does-nothing-in-sponza-and-the-thresholds-margin-is-thin)
for what it does and does not buy here.

---

## Pipelines and descriptors

### No registry, and the one array that is not one

Seven-odd named `VkPipeline` members for the screen-space passes. A registry earns its
place when pipelines are created from *data*, and the trigger it named was a variant
selected **per draw** rather than per keypress — which is exactly what G5 built, so the
three *geometry* families are an array now and nothing else is. What that array is not is
a registry: it is indexed by a number a material stores, it holds no strings to look a
pipeline up by, and it owns nothing but the handles.

### Shader variants

`gfx::ShaderVariant` is `{name, gbuffer vert/frag, shadow frag, forward frag, constants,
cull, blend}`. `Renderer::addShaderVariant` returns the index a material puts in
`GpuMaterial::shader`, and that field is the whole of the selection — the geometry goes
into the same buffers, the same instance table and the same indirect commands. Variant 0
is the engine's own triple and exists before a game can register anything, so a
zero-initialised material already names the default and nothing that predates variants has
to be told they exist.

**Created lazily, per (variant, pass).** Registering compiles nothing. A variant's
G-buffer, shadow and forward pipelines are built the first time a draw command carrying its
index reaches each pass, so a game may declare forty and pay for the two a level places.
`destroyPipelines()` nulls all of them, which is what makes a feature toggle, a resize and a
shader hot reload rebuild a game's shaders on exactly the path they rebuild the engine's —
by forgetting them rather than by knowing about them. Still nothing enumerates: this is
one live pipeline per (variant, pass) asked for, and no cross-product anywhere.

**The command builder groups by variant**, generalising the unmasked/masked split it
already made from two groups to N. Each group is a contiguous run, and `VariantRange`
— `{variant, first, count, unmasked}` — names it. Three properties are worth stating:

- *Instancing survives*, by the argument the builder already made. A merged run shares a
  primitive, one primitive has one material, and one material has one variant — so a run
  was already all-one-variant, exactly as it was already all-masked or all-unmasked.
- *Grouping fragments runs*, because a merge also requires adjacent slots. On Sponza that
  is worth nothing (its 103 primitives are all distinct and nothing merges), and on an
  instance-heavy scene it grows with the number of variants **in use**. The builder walks
  the slot table once per used variant, and one O(slots) pass decides which those are, so a
  registered-but-unplaced variant costs nothing on the CPU either.
- *The range list is ordered by variant, the command buffer by half.* Commands stay
  static-then-skinned, because that split is what lets a pass bind one vertex buffer per
  half; the ranges are `inplace_merge`d into variant order, so `drawSceneIndirect` binds a
  pipeline once per variant rather than once per half per variant. Both binds are lazy —
  binding unconditionally measured as most of a 0.1 ms regression on the punctual atlas
  pass, where a twenty-four-layer loop multiplies whatever the draw loop does by
  twenty-four. A scene with one variant records exactly what it recorded before variants
  existed: one pipeline bind, one indirect draw.

The forward pass is the exception and has to be: depth order is the whole point of it, so
its list cannot be sorted by anything else and a variant appearing twice in that order gets
two runs. Reordering to save a bind would put a far surface over a near one.

**`engine/shaders/gbuffer_contract.glsl`** is the fixture a variant is compiled into, made
includable — the four attachments, the five varyings, set 1's material table and bindless
array, `sampleOr`, and a `gbufferWrite(Surface)` that owns the octahedral packing and the
specular antialiasing so a game shader does not copy either. `gbuffer.frag` includes it
too, which is what stops the contract and the engine's own shader drifting apart.

**A forward variant reaches the opaque depth behind it**, at `set = 4, binding = 0`, and it
is the one thing that pass offers the G-buffer pair cannot. The depth is already the forward
pass's attachment, so binding it as a texture costs a descriptor and no image: the attachment
is `DEPTH_READ_ONLY_OPTIMAL` with `STORE_OP_NONE`, which is what makes reading the image a
pass is rendering to legal rather than undefined. An intersection highlight, a soft particle
and a water line are all this comparison. Both depths are reverse-Z, so `viewDistance()`
inverts the projection before they are subtracted — subtracting them raw gives a band whose
width in the world changes with distance. `game/battle_arena/shaders/shield.frag` is the
worked example.

**`constant_id` 0..7 are the engine's** in every id space a variant can be compiled into,
and `ShaderVariant::constants` is supplied from 8. One number answers "what do I write in
`layout(constant_id = ...)`" for all three of a variant's pipelines, and a constant the
engine adds later cannot collide with one a game already shipped.

**What a variant does not reach.** The shadow and velocity passes keep the engine's vertex
stage, so a variant that only shades differently is unaffected and one that *moves*
vertices in its own `gbuffer.vert` will cast an undisplaced shadow and write an undisplaced
motion vector. That is a limitation rather than an oversight: making a vertex shader reach
every geometry pass is a different row, and the two passes are named in the contract header
so it is refused in writing rather than discovered.

`GpuMaterial` gained `shader` and `params` for this, and only for this. `shader` is read by
the CPU alone — by the time a fragment runs, the answer is the pipeline it is running in —
and `params` is four floats no engine shader reads, which is what lets a game carry a stripe
width or a pulse rate without widening the struct per game or keeping a second buffer in
step with the material table.

### Feature constants

`engine/shaders/features.glsl` holds the shading id space — `SAMPLE_COUNT`, `ENABLE_SHADOWS`,
`ENABLE_PUNCTUAL_SHADOWS`, `ENABLE_SSAO` — shared by `lighting.frag`,
`lighting1x.frag` and `forward.frag`. `gbuffer_contract.glsl` declares `ENABLE_GSAA`;
`tonemap.frag` a non-boolean `TONEMAP_OPERATOR` plus `ENABLE_BLOOM`. Ids 8 and above in
any of these belong to whichever shader variant is being compiled.

`GraphicsPipelineDesc::constants` is a `std::vector<uint32_t>` indexed by `constant_id`.

**There is no permutation cache**, and that is stricter than a map rather than lazier:
`featureKey()` is compared each frame and the affected pipelines are rebuilt. One live
build per pipeline, created when asked for, at a cost of one hitched frame per keypress
— which is what a debug toggle is worth. Nine constants would be 512 possible
permutations; enumerating the cross-product is what turns flags into pipelines, and
nothing here enumerates. Shader variants answer the same warning the same way and are
above, under *Shader variants*: lazily, one per (variant, pass), never a product.

**A descriptor layout cannot be specialised away**, which is the sharper lesson: hardware
support for ray query selects a shader **file** (four lighting variants), and the constant
selects behaviour within it.

### Reflection as a check, not a generator

`SpirvReflect.{h,cpp}` walks the module for descriptor bindings and `SpecId` decorations;
`Renderer::verifyShaderBindings` compares them against the hand-written layouts and aborts
in Debug on a mismatch. **Layouts stay hand-written** — generating them would remove the
mismatch by removing the ability to read what is bound.

Three arms, all verified against deliberate breakage: a binding the layout lacks, a
descriptor-type disagreement, and a `constant_id` the pipeline supplies no value for.

**Run per variant, not per family**, and that is where it earns most. A family's layout was
written by hand beside its shaders; a variant's GLSL arrives from a game and is compiled
against a layout its author never saw, so this is the only thing between a contract
violation and a black surface with no explanation.

Deliberately not a library: it skips opcodes it does not know and returns nothing for a
module it cannot parse.

### Descriptor sets

Set 0 is the frame set — uniforms at binding 0, then storage buffers for lights, shadow
matrices, the instance table and the previous-transform history. Set 1 is whatever the
pass reads. The descriptor pool is `maxSets = 16`, with headroom, and
`destroyFrameResources()` frees its sets rather than nulling them — the original leaked
`kFramesInFlight` sets per resize and took the process down on the sixth swapchain
recreate.

**There are `kMaxViews` frame sets per frame slot, not one**, and a pass binds the one its
view names through `View::uniformSlot`. The uniform block, the light buffer and the
shadow-matrix buffer behind them are per view for the same reason, and it is not
housekeeping: two views recorded into one command buffer read their matrices at *submit*
time, so a single block would have the second view's `updateUniforms` overwrite what the
first view's already-recorded draws are about to read. **The first view would then render
with the second view's camera** — one view right, the other subtly wrong, and nothing
invalid. The light selection is per view by construction too, since `updateLights` ranks
and culls against the camera it is handed.

Bindings 3 and 4 are the instance table, which is shared: `ensureInstanceCapacity` writes
them into *every* view's set, because a view whose set still named the pre-growth buffer
would draw from freed memory.

**A block is written only by a view that exists.** `kMaxViews` blocks are allocated once at
`createFrameResources`; a one-view frame performs exactly the three uploads it always did.
That is what keeps the cost of holding the machinery for two views at zero for a frame that
uses one.

### The image table, and the one descriptor array that grows

`gfx::ImageTable` is what a game loads a PNG through — `e.images().load("res:/ui/hero.png")`
returns an `ImageId`, and `destroy` takes it back. It is split in two on purpose, and the
split is the same one `SceneData` draws against `GltfScene`:

- **The table is the lifetime** and holds no Vulkan at all: entries, a free list, a
  generation per slot, a revision counter. `Engine` owns it, exactly as it owns the
  instance table.
- **The renderer is the residency**: the `GpuImage` behind each slot, the sampler, and the
  descriptor array they live in — with `syncImages()` at the top of `drawFrame` bringing
  the second in line with the first whenever `ImageTable::revision()` has moved.

That split is not filing. It is what makes the rules that actually go wrong here — a slot
handed to two holders, a stale handle resolving to whatever took its place — provable in
the unit suite, which links no Vulkan.

**The overlay's descriptor array is the only one in the engine that grows.**
`kMaxOverlayImages` is deleted rather than raised, and the ceiling that replaced it is the
device's: the smaller of `maxPerStageDescriptorSampledImages` and
`maxDescriptorSetSampledImages`, less the reserve above, reported by
`Renderer::maxImageSlots()` and given to `ImageTable::init`. `ensureImageCapacity` doubles
towards it by rebuilding the layout, the array's own descriptor pool and the set.
`PARTIALLY_BOUND` is deliberately absent, so every allocated slot is written and a wrong
index draws the font atlas rather than reading undefined data.

**The layout declares the capacity, not the ceiling, and that distinction cost half the
Debug frame rate for a day.** `VARIABLE_DESCRIPTOR_COUNT` let one layout be created at the
device's limit with each allocation taking as much of it as it needed, so growth touched no
pipeline — which was the point, since a layout of a different width is a different layout
to everything built against it. What it did not survive is that **the validation layer
charges the *declared* count once per draw that samples the array**, at about 8 ns a
descriptor, and the ceiling on this machine is 1,044,480: the overlay's single text draw
cost 8.5 ms of CPU in every Debug frame for descriptors no image would ever occupy. Sizing
the layout to the capacity makes the charge proportional to what is resident — one or two
slots in every scene in the tree — at the price of rebuilding the five pipeline layouts the
set appears in each time the array doubles, log2(N) times over a run, on the same event
that already waits for the device. Release never saw it, and no check in
[tooling.md](tooling.md) could: the golden suite turns the HUD off and the baseline table
is Release.

**Growth and release wait for the device rather than deferring by frames-in-flight**, and
that is a decision rather than an oversight. Rewriting a descriptor an in-flight command
buffer bound, or freeing an image it samples, needs either a wait or a per-frame retirement
list — and this engine deliberately has neither: `ensureInstanceCapacity` says so in as
many words, and `GltfScene::unloadModel` took the same trade. Images are not the third case
that would make one worth writing, because they arrive on the same event the other two do,
at load time from `Game::init`. The trigger for revisiting it is a caller that loads images
*per frame during play*, which is streaming, which is C10.

---

## Shader hot reload

`pollShaderReload` takes the newest mtime under both shader trees once a second and, on any
change, re-runs `glslangValidator` over every source and rebuilds every pipeline. No
dependency tracking, no partial invalidation.

**The recompiled SPIR-V never reaches the build tree.** Each shader compiles to one temp
file outside it, whose bytes `gfx::overrideShaderBinary` reads back and unlinks; from then
on `readShaderBinary` answers for that name out of memory, ahead of both shader
directories. A shader that fails to compile publishes nothing, so the module already bound
stays bound — a syntax error costs an error message rather than a black screen, which is
what the old write-aside-and-rename dance bought by leaving the built `.spv` on disk. Why
the artifact had to go is in [tooling.md](tooling.md#hot-reload-leaves-nothing-behind).

The environment bake is re-run too. Skipping it would have been easier, but a reload that
ignores four of forty-nine shaders is a tool that looks like it works.

~3.3 s per reload on Sponza with the demo, nearly all of it 49 `glslangValidator`
processes — it scales with the shader count and nothing else, which is why it was ~1.4 s
when there were 28 of them. `--hot-reload auto|on|off`, and `auto` is on in Debug. It was
`render.shaderHotReload` until D14 decided a recompile loop is a developer control rather
than a preference.

---

## Swapchain and resize

The resize is consumed **before** the acquire, never after. A successful acquire hands
`imageAvailable` to the presentation engine to signal, and `vkDeviceWaitIdle` does not
drain that — it waits on queue work, and the presentation engine is not a queue.
Abandoning the frame at that point would have `handleResize()` destroy a semaphore with a
signal operation outstanding.

`SUBOPTIMAL` from the acquire did acquire an image and did signal, so that frame finishes
normally and the resize happens at the top of the next one.

`--resize-every N` drives the cycle in-process with `glfwSetWindowSize` for soak testing.
See [limitations.md](limitations.md) for what that soak could and could not prove.
