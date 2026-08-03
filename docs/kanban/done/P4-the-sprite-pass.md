---
id: P4
title: The sprite pass
arc: P
size: M-L
verification: golden-11, readback, tests-hosted, validation, trace
---

# P4 — The sprite pass

Layers, sprites, `SpriteId`, one instanced draw per layer. Rotation, pivot, flip, texel UV rect, tint

## A sprite is a struct, and the world is flat

```cpp
void MyGame::init(Engine& e) {
    e.camera().setOrthographic(180.0f);        // world units of visible height
    background = e.sprites().createLayer({ .order = -10 });
    actors     = e.sprites().createLayer({ .order = 0 });
}

void MyGame::fixedUpdate(Engine& e, float step) {
    SpriteId s = e.sprites().create(actors, SpriteDesc{
        .image  = hero,
        .uv     = {0, 0, 16, 16},              // texels, not normalised
        .size   = {16.0f, 16.0f},
        .pivot  = {0.5f, 1.0f},                // feet
        .flipX  = facingLeft,
    });
    e.sprites().setPosition(s, at);
    e.sprites().destroy(s);
}
```

The UV rect is in **texels**, not normalised coordinates. Normalised is where half-texel errors
come from, and an atlas the artist authored is measured in pixels in the tool that made it.
The engine does the division, once, against the dimensions it loaded.

## P4, the sprite pass

**The pass already exists under another name, and that is the argument for its shape.**
[`particle.vert`](../../../engine/shaders/particle.vert) is six vertices from a `const vec2 kCorners[6]`
indexed by `gl_VertexIndex`, with no vertex buffer and no vertex attributes; per-instance data
comes from an SSBO through `gl_InstanceIndex`; the texture is a bindless index carried as
`flat out uint vTexture` from `e.flags.x`; the blend is `PremultipliedOver`, depth-tested with no
depth write. A sprite pass is that, minus the billboard axes, plus rotation, pivot, flip and a
UV rect.

So P4 is a pass recorded inline as a method — `recordSprites(cmd, slot)` — in the grain of every
other pass, and **not** a `SpriteRenderer`, a `SpriteBatch` or a second renderer. If the
extraction ever knows the name of a layer, it has gone too far; that is D4's reviewer test
applied here.

Three decisions inside the row:

- **Sorting is on the CPU, and that is a deliberate stop.** The particle system sorts on the GPU
  with a bitonic sort, and reaching for it here would be the second occurrence of a pattern, not
  the third. A layer is a sort key that changes rarely and a `std::sort` over live sprites is
  measurable before it is optimised. The trigger to move it is a measurement, not a hunch.
- **UV rects are texels.** See Part 2. Half-texel errors are the
  single most common way pixel-art rendering goes subtly wrong, and normalised coordinates in
  the public API are where they come from.
- **A sprite is not an `InstanceTable` entry.** Lit sprites are, and that is P6. Making every
  sprite an instance would put ten thousand blended entries through a G-buffer built for
  materials that a flat quad does not have.

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- `scripts/golden.sh check release` -- ~~twelve~~ **eleven** cases, byte-identical. (`no-ibl`
  was retired for pinning a copy of `lit`; this card was written while the count was twelve,
  and P1's and P2's carried the same stale number.) No case in the suite draws a sprite, so
  every one of them is a check that a pass added to the frame costs a scene without sprites
  exactly nothing.
- **The readback, and this card did not name it.** ~~It named the golden set, validation and
  a trace -- and a sprite pass closed on those three would have been verified in *pixels
  somebody snapped* rather than in texels computed from the source, which is the one thing
  [arcs.md](../arcs.md) says a P row may not do:~~ *"a texel authored is a texel presented,
  checked by reading back the presented image and comparing it against the source file
  rather than against a snapped reference."*

  P2 built the machinery -- `scripts/readback.sh`, `gfx::compareReadback`, `--readback` and
  `--readback-expected` -- and this row is the first that can point it at a sprite. **Five
  cases is not enough and the reason is structural**: all five draw the source through the
  *overlay*, which is an axis-aligned quad in pixel coordinates with a `1 / extent` push
  constant. A sprite goes through a world position, a pivot, a rotation, an orthographic
  projection, a viewport transform and a divide by `textureSize`, and none of those six is
  touched by drawing a rectangle at pixel (0, 0).

  ```bash
  scripts/readback.sh release      # seven cases now, tolerance 0, 0 pixels allowed over it
  ```

  `--readback-sprite` draws the same source file as **one sprite**, through an orthographic
  camera at one world unit per texel, with its top-left corner on texel (0, 0). Everything
  after that is P2's check unchanged. Two cases: at 3x, and at 3x through a letterbox.
- **Phase 2's milestone is a number**: *"ten thousand unlit sprites across several layers,
  sorted correctly, holding frame time, with the readback still bit-exact."* All four
  clauses have to be discharged, and the count has to be reproducible by whoever reads this
  card rather than by whoever wrote it.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation. The sort, the handles
  and the packing are Vulkan-free by construction, so they are provable without a device --
  the same split `ImageTable` and `Presentation` drew, for the third time.
- Zero validation errors with layers on, in every capture, **including across a buffer
  growth event**. That is P1's requirement restated for the same hazard: a growth path first
  exercised in somebody's game is a growth path that has never been tested.
- Per-pass GPU cost from `scripts/baseline.py` trace medians, several runs
  per arm -- never the `GPU @` log line. Two arms differing in the sprite count and nothing
  else, which means the *camera* has to be the same in both.

## Reference

[architecture/rendering.md](../../architecture/rendering.md).

## Outcome

### What landed

`scene::SpriteTable` (`engine/scene/SpriteTable.{h,cpp}`, hosted), owned by `Engine` and
reached as `e.sprites()`, handing out `SpriteId` and `SpriteLayerId` -- two distinct
`core::Handle<Tag>` types, because `destroy(layer)` and `destroy(sprite)` are different
verbs on different arrays. `Renderer::recordSprites` is the pass, a method in the grain of
every other, plus `sprite.vert`, `sprite.frag`, one descriptor set layout, one pipeline and
one storage buffer per frame in flight. No base class, no `SpriteRenderer`, no `SpriteBatch`,
and nothing in `SpriteTable` knows what a descriptor set is.

**Sprites take their textures from P1's `ImageTable`, their projection from P3's
orthographic camera and their pixel-exactness from P2's presentation path -- and all three
of those are the row not doing something.** No second texture array, no second camera, no
retrofit into a frame that did not preserve texels. The ordering argument in
[order.md](../order.md) is the reason this row was small.

### Where the pass draws, which is the decision the row turned on

**After the tonemap, into the virtual target, with no depth test.** An unlit sprite is
display-referred art -- the texel in the file is the one the artist chose -- so exposure, a
curve and a temporal resolve are three corrections applied to a value that needs none.
Downstream of the tonemap is also the only place in this frame where a texel reaches the
swapchain unaltered, which is the arc's whole standard.

What that costs is stated in the header rather than left to be discovered: a sprite is **not
occluded by 3D geometry, not bloomed, not reflected by SSR and not fogged.** That is the
opposite trade to P6's lit sprite, which goes through the G-buffer and by construction
carries no pixel-exact guarantee -- and it is why they are two passes rather than one with a
flag. `arcs.md` had already written the P6 half of this argument; this row is the other half
of the same sentence.

### The card said one draw per layer. It is one draw for all layers

One blend state makes one global sort possible, and one global sort makes one draw possible.
That is `particle.frag`'s own argument arriving at the same answer one subsystem along, and
it leaves a layer as **purely a sort key** -- which is all the card asked a layer to be. Ten
layers would have been ten draws and ten sorts to keep consistent with each other, for no
property the single sort does not already have.

### The sort is on the CPU, and the measurement declined the row that would move it

The card kept CPU sorting as a deliberate stop with *"the trigger to move it is a
measurement, not a hunch."* The measurement now exists and it points the other way, because
the sort is not where the time goes:

**`SpriteTable` keeps its `GpuSprite` array dense and already in draw order.** A layer is
the sort key and a *position is not part of it*, so `setPosition` is two float writes into
the buffer the draw will read and `prepare()` sorts only on create, destroy or reorder. Ten
thousand sprites moving every frame re-sort **nothing** -- a hosted test asserts exactly
that against `sortCount()`, because it is the claim the whole budget rests on. A GPU sort
would be moving work that is not being done. The trigger is now sharper and is recorded in
`limitations.md`: a game whose sprite *set* changes shape every frame at a stated count.

### What the readback proved, at what count

`scripts/readback.sh release` -- **7 of 7 bit-identical**, tolerance 0, zero pixels allowed
over it:

| Case | Result |
|---|---|
| `native-inside`, `native-outside` | 64x48 at 1x, **0 of 3072** differ |
| `scale3-inside` | 192x144 at 3x, **0 of 27648** differ |
| `scale3-outside` | 64x48 at 1x, **0 of 3072** differ |
| `letterbox` | 192x144 at 3x at (20,30), **0 of 27648** differ |
| **`sprite`** | **192x144 at 3x, 0 of 27648 differ -- through the sprite pass** |
| **`sprite-letterbox`** | **192x144 at 3x at (20,30), 0 of 27648 differ** |

The two new cases are the arc's Phase 2 milestone discharged in its own words: *the readback
still bit-exact*. What they prove that the other five cannot is the sprite path end to end --
a world position through an orthographic projection, a pivot, a quad rasterised from
`gl_VertexIndex`, a UV rect divided by `textureSize`, a tint unpacked from RGBA8 and
converted from sRGB, a premultiplied blend into an `_SRGB` attachment, and a nearest blit
by three. Every one of those is a place a half-texel can be lost, and the expected image was
**computed from the source file**, so there was nothing to re-snap: the only way to pass was
to be right.

**Ten thousand sprites is proved separately from bit-exactness, and deliberately so.** The
readback case draws one sprite because the claim is about texels; the count is the trace
arm's job and a 10,000-sprite readback would have been ten thousand overlapping quads whose
expected image is not computable from a source file. The two halves of the milestone are two
checks.

### The defect the verification caught, and it was a good one

**At yaw 0 the camera looks down +Z, and the whole world comes out mirrored.**
`glm::lookAt` with a forward of `+Z` produces a right vector of `-X`, so the first
`--readback-sprite` run placed the sprite on the left edge and drew it on the right --
27,648 of 27,648 texels differing, max delta 192. It looks exactly like a sign error in the
projection and is not one; a flat world looks down **-Z**, which is `yaw = pi`.

This is worth recording because **no check other than the readback would have found it.**
The image was crisp, correctly sized, correctly oriented vertically and in the right place
for a mirrored world. A golden case would have been snapped from it. Somebody looking at a
screenshot of a symmetric test pattern would have accepted it. It is the precise failure
mode the arc's verification boundary exists for, and it fired on the first run of the first
row that could trigger it.

### What it costs

`--sprites <N>` is a run mode rather than a game somebody has to write, for the reason
`--capture` and `--resize-every` are: a number quoted on a card that nobody else can re-run
is an anecdote. Both arms use the same orthographic camera, so the two differ in the sprite
count and nothing else. 960x540, 1x MSAA, release, medians over 6 runs of 600 frames per arm:

| | `--sprites 1` | `--sprites 10000` |
|---|---|---|
| `Sprites` | 0.001 ms | **0.053 ms** |
| `Lighting` | 0.019 / 0.018 ms | 0.019 / 0.018 ms |
| `Frame` | 0.202 / 0.201 ms | **0.256 / 0.256 ms** |
| CPU busy | 0.078 / 0.080 ms | 0.117 / 0.114 ms |

Ten thousand sprites across four layers: **+0.052 ms on the GPU, +0.055 ms of `Frame`, and
+0.037 ms of CPU**, at roughly 4x overdraw. `Lighting` not moving across the arms is itself
the check that the pass is downstream of it. The CPU figure is one `memcpy` of 640 KB into
mapped memory and **not a sort**.

### What the estimate did not predict

- **The row came in well under M-L, and P1/P2/P3 are why.** The three decisions that could
  have been expensive -- where the textures come from, what the projection is, and whether
  the frame preserves texels -- had all been made by rows that landed alone specifically so
  they could be.
- **`textureSize` is the right place for the texel-to-normalised divide, and it was forced
  rather than chosen.** `ImageTable` holds no `VkImage` (P1's split), so nothing on the CPU
  side knows a file's dimensions; the card said *"the engine does the division, once,
  against the dimensions it loaded"* and the only place that sentence is true is the
  fragment shader. It is also the answer that cannot drift: an atlas re-exported at a
  different size needs no number in a game changed.
- **The stress mode had to spawn in batches, and that is a verification requirement rather
  than realism.** Creating all N sprites before the first frame sizes the buffer exactly
  once, so `ensureSpriteCapacity`'s doubling path would never have run with layers on --
  P1's own lesson, restated. Eight batches means three real reallocations per stress run:
  `1250 -> 2500 -> 5000 -> 10000`, all clean.
- **`srgbToLinear` was extracted at two occurrences, not three**, because
  [principles.md §8](../../architecture/principles.md) states the shader rule that way and
  the alternative is D8's four-copies pattern starting over. It moved to
  `engine/shaders/srgb.glsl` and the golden set was byte-identical across the move, which is
  the check worth having on a rule that claims to change no SPIR-V.
- **A layer got `order` and nothing else.** `visible` was not added: it is two lines, and two
  lines is exactly what makes it worth waiting for a caller. Trigger recorded in
  `limitations.md` -- a game with a HUD layer it toggles.

### Deferred, with the trigger stated

- **GPU sprite sorting.** Declined by measurement, above. Trigger: a game whose sprite set
  changes shape every frame at a stated count.
- **A `visible` flag on a layer.** Trigger: the first game with a layer it toggles.
- **A per-sprite Z, or depth interaction with 3D geometry.** No trigger -- P6 is the row that
  wants both and gets them by going through the G-buffer.
- **Per-image sampler choice.** Inherited from P2 unchanged: `pixelExact` swaps the whole
  image array between linear and nearest, so a game that wants crisp sprites gets a crisp UI
  with them. Trigger unchanged -- a linear UI icon and a nearest sprite sheet in one game.
- **A rotated or non-integer-positioned sprite is not pixel-exact**, and cannot be: a texel
  rotated by 30 degrees does not land on a texel. The guarantee is about the identity case
  and `scripts/readback.sh` pins exactly that. Recorded in `limitations.md` so it is not
  read later as a bug.

### Verification

- `scripts/golden.sh check release` -- **11 of 11 match**, across a new pass in the frame and
  a shader header extraction that touched `overlay.frag`.
- `scripts/readback.sh release` -- **7 of 7 bit-identical**, plus the resize soak clean.
- `./test.sh debug` -- **724 of 724**, including **18 new `SpriteTable` cases**.
  `./test.sh asan` -- **724 of 724**.
- Validation, layers on, debug build: 240 frames at 1000x600 with a 320x180 virtual
  resolution, `--sprites 10000` and `--resize-every 20` -- **zero errors and zero warnings**
  across 12 swapchain recreates and 3 sprite-buffer growth events. A second run windowed
  rather than headless, 180 frames at 5000 sprites with a capture -- zero errors, and the
  capture shows crisp nearest-neighbour texels inside a 20/30 letterbox.
- `scripts/baseline.py --zones` -- the table above, two arms, two independent pairs of runs.

### Found and left alone

**`Renderer::stats.particles` has the same staleness `stats.sprites` was written to avoid**:
it is assigned only where a draw happens, so a scene whose last particle died reports the
previous frame's count on the HUD until another one is born. `stats.sprites` is assigned
before its early return instead. One line, in a pass this row did not touch, and reporting it
is cheaper than a drive-by edit to a subsystem whose golden case is byte-identical today.
