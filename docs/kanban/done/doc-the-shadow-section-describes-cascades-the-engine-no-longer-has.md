---
id: doc-the-shadow-section-describes-cascades-the-engine-no-longer-has
title: The shadow section describes cascades the engine no longer has
arc: doc
size: S
verification: inspection
---

# doc-the-shadow-section-describes-cascades-the-engine-no-longer-has — The shadow section describes cascades the engine no longer has

Found while correcting the render-target table for C31, which was stale in the same
direction and by the same amount.

[rendering.md](../../architecture/rendering.md)'s **Cascades** section opens *"Four cascades
in one 2048-square D32 array, split logarithmic/uniform at `cascadeSplitLambda` 0.7"*. The
engine creates `shadowMap` with a layer count of **1** at `kShadowMapSize` = **4096**
([`Renderer.cpp:2094`](../../../engine/gfx/Renderer.cpp#L2094)), and the constant's own doc
block argues the replacement: *"4096 costs no more memory than the cascades: four
2048-square layers and one 4096-square map are both 64 MB at D32."* So the section is not
merely imprecise — it describes a design that was measured, replaced, and argued against in
the header, while the reference still presents it as current. `grep -c cascade
engine/gfx/Renderer.cpp` returns 7, against a section of twenty-odd lines about them.

The same numbers are wrong in at least three more places, and they should go in one pass
rather than one at a time:

- The pass table's `recordCull` row — *"one dispatch per view (camera, 4 cascades, up to 8
  atlas layers)"*. `kCullViews` is `2 + kMaxShadowLayers` = **26**: the camera, the sun, and
  24 atlas layers.
- The pass table's `recordShadows` row — *"Four cascades into one D32 array, one render pass
  each"*.
- The culling section's *"camera, four cascades, eight atlas layers"*.

**The cascade *reasoning* is worth keeping and the trap in it is worth keeping most.** The
measurement that one layered pass is 2x slower, and why — depth compression and hierarchical
Z are per attachment layer — is exactly the kind of finding that gets rediscovered
expensively. It should move to a paragraph that says a cascaded sun was tried and what
replaced it, not be deleted with the section.

This is a documentation row and nothing in the engine changes. It is filed separately from
C31 rather than folded into it because C31's reference update is the render-target table,
and a row that also rewrote the shadow chapter would have made its own golden result harder
to attribute.

## Verification

- `inspection`: every count in [rendering.md](../../architecture/rendering.md) that names a
  cascade, a shadow layer or a cull view agrees with `kShadowMapSize`, `kMaxShadowLayers`
  and `kCullViews` as the header declares them. There is no automated check for this and
  that is the finding: the drift survived because nothing compares prose to a constant.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — the Shadows chapter and the
three counts listed above. This card *is* the reference update.

## Outcome

Every count in [rendering.md](../../architecture/rendering.md) that names a cascade, a shadow
layer or a cull view now agrees with `kShadowMapSize`, `kMaxShadowLayers` and `kCullViews`.
The **Cascades** section is now *The sun: one map, and why not cascades*, and it keeps the
argument rather than deleting it: cascades were fitted to the view frustum, so the world size
of a texel, the world distance the bias pushed an occluder and the width the kernel spanned
all changed when a surface crossed a split — **24% of pixels changed with a peak delta of
630** for a camera move alone. That is the finding, and `shadow.glsl`'s own header is where
it was recorded while the reference still described the thing it replaced.

The layered-pass measurement was kept and **re-pointed**, which is the part the card asked
for and the part that is easy to get wrong: `0.61 → 1.14 ms` was measured on four cascades in
a `2D_ARRAY`, and the reason — depth compression and hierarchical Z are per attachment layer —
is still why the punctual atlas renders a layer at a time. Deleting it with the cascades
would have thrown away a live argument about the *other* shadow path.

**The card under-counted. Six more claims were stale, not three**, and one of them was a
statement about behaviour rather than a number:

- The pass table's `recordShadows` row, *"Four cascades into one D32 array, one render pass
  each"*.
- The `recordCull` row and the culling section's *"camera, four cascades, eight atlas
  layers"* — `kCullViews` is 26.
- **"The shadow pass culls front faces"**, which has not been true since the pass stopped
  culling either face. That is a *parity* decision with a long argument in `Renderer.cpp`: a
  ray query has no notion of facing, so culling back faces would make the raster path
  disagree with the traced one for exactly the geometry whose front faces the light cannot
  see. A reader acting on the old sentence would have "fixed" it back.
- The corners-from-an-explicit-slice paragraph, which described fitting to a frustum slice
  and has nothing left to describe.
- *"the shader routes every directional light through the cascades"*, the fog row's *"testing
  the cascades"*, the ray-shadow section's *"the cascade fade"*, and TAA's *"cascade
  stair-stepping"*.
- `architecture/README.md`'s one-line summary, which was outside the file the card named and
  said *"four cascades in one array"*. Found by sweeping `docs/` rather than by reading the
  card's list, which is the only reason it was found at all.

**What made this survive is worth stating, because it is the card's own verification line:**
nothing compares prose to a constant, and the drift was invisible to every check the project
runs. The engine builds, the goldens are byte-identical and the suite is green with the
reference describing a design that was measured, replaced and argued against in the header of
the file that replaced it.

## Verification results

- `inspection`: `grep -n cascade docs/architecture/rendering.md` returns nine lines, and all
  nine are inside the section that explains what cascades were and why they went. Every
  surviving count — `4096`, `24`, `kCullViews` = 26, `1024`-square atlas — was read off
  `engine/gfx/Renderer.h` rather than carried over.
- A sweep of `docs/` for `four cascades`, `eight atlas layers` and `cascadeSplitLambda`
  returns nothing outside `done/` cards and this one.
- No engine code changed, so nothing else could have moved. `scripts/kanban.py` clean.
