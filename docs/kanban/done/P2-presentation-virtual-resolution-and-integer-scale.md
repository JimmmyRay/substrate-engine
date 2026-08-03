---
id: P2
title: Presentation: virtual resolution and integer scale
arc: P
size: M
verification: golden-11, validation, readback, tests-hosted
---

# P2 — Presentation: virtual resolution and integer scale

The offscreen target, the integer-scale blit, the letterbox, and the switches that take jitter, TAA and the tonemap curve out of the 2D path. Where the pixel-perfect guarantee actually lives, and it is verifiable here with no sprite in existence

## Presentation is one number, and native is the degenerate case

```cpp
void MyGame::configure(GameSetup& setup, Settings&) {
    setup.virtualResolution = {320, 180};      // omit for native
    setup.uiInsideVirtual   = true;            // the HUD scales with the world
}
```

**There is not a virtual-resolution path and a native path.** There is one path: render into a
target of size `V`, present at the largest integer `S` that fits, letterboxed. Native is
`V = the window extent`, which makes `S` exactly 1 and the blit a no-op to be elided. Writing
it as two modes would be writing the same code twice and then discovering they disagreed about
rounding.

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- `scripts/golden.sh check release` -- **eleven** cases, byte-identical. The suite is eleven
  and not twelve: `no-ibl` was retired when the flag it switched outlived its feature, so a
  card naming `golden-12` names a count that has not existed since. Every case runs at native,
  where `S` is 1 and the blit is elided, so a moved pixel here is this row breaking the
  degenerate case it claims to subsume.
- Zero validation errors with layers on, in every capture.
- **The readback.** This is the row that first makes the P arc's own standard checkable, and
  the first version of this card did not name it -- which meant the card could have been closed
  green on eleven byte-identical images that prove only that `S = 1` still works.

  [arcs.md](../arcs.md) defines a P row as one whose correctness is judged in texels: *"a texel
  authored is a texel presented, checked by reading back the presented image and comparing it
  against the source file"*, and Phase 1's milestone is that *a readback of the swapchain is
  bit-identical to the source file scaled by three*. So:

  ```bash
  scripts/readback.sh release
  ```

  Each case is `--readback <image>` plus `--capture`: scale 1 and scale 3, with the UI inside
  the virtual target and outside it, plus a window that is *not* an integer multiple. The
  expected image is **computed from the source** -- expanded by the integer scale, placed at
  the letterbox offset -- rather than snapped from a previous run, so there is nothing to
  re-snap when it fails. Tolerance is 0 and the pixel budget is 0: bit-identical, not close.

  Byte-identical goldens at scale 1 prove the degenerate case. Necessary, and not sufficient.
- **Hosted unit tests for the scale and letterbox arithmetic.** The rounding is the row --
  which way an odd leftover pixel goes, what happens when the window cannot hold the virtual
  target, and that the native case reduces to an identity that can be elided. `Presentation.cpp`
  holds no Vulkan for exactly this reason, so `./test.sh debug` and `./test.sh asan` cover it.
- **A resize.** The integer scale is derived from the window, so it changes under the user's
  hands: `--resize-every N` with a virtual resolution set, validation on, zero errors.

## Reference

[architecture/rendering.md](../../architecture/rendering.md).

## Outcome

### What landed

One path, with native as its degenerate case, exactly as the card argued for it. `renderExtent`
is a `Renderer` member every `renderArea`, viewport, dispatch round-up and inverse-texel push
constant reads -- 35 sites that had spelled `swap.extent` -- and it equals the window's extent
unless a game named a virtual resolution. `presentTarget`, an offscreen image in the
swapchain's own format, is created only where `identityPresent` is false; `recordPresent` blits
it up with `VK_FILTER_NEAREST` behind a full-image black clear, and records nothing at all
where the layout is the identity. That elision is what kept the golden suite byte-identical,
and it is a property of one test in one place rather than a mode flag every pass has to agree
with.

The arithmetic is `engine/gfx/Presentation.{h,cpp}`, in `SUBSTRATE_HOSTED_SOURCES` and holding
no Vulkan -- the same split `ImageTable` drew, for the same reason: what goes wrong here is an
off-by-one in a scale or a bar one pixel wider on the left than the right, and none of that
needs a device to be proved.

`GameSetup` gained `virtualResolution`, `uiInsideVirtual` and `pixelExact`; `Config` gained
`--virtual-resolution`, `--ui-outside-virtual`, `--readback`, `--readback-expected`, `--width`
and `--height`. `Renderer::uiFromWindow` maps the cursor back through the scale and the bars,
`framebufferWidth()` now reports the surface a caller draws onto rather than the window, and
`FrameCapture` gained `compareReadback`.

### What the readback actually proved

`scripts/readback.sh release` -- five cases, all bit-identical, tolerance 0 and 0 pixels
allowed over it:

| Case | Result |
|---|---|
| `native-inside` | 64x48 at 1x at (0,0), **0 of 3072** differ |
| `native-outside` | 64x48 at 1x at (0,0), **0 of 3072** differ |
| `scale3-inside` | **192x144 at 3x** at (0,0), **0 of 27648** differ |
| `scale3-outside` | 64x48 at 1x at (0,0), **0 of 3072** differ |
| `letterbox` | **192x144 at 3x at (20,30)**, **0 of 27648** differ |

`scale3-inside` is the P arc's Phase 1 milestone discharged in its own words: *a readback of
the swapchain is bit-identical to the source file scaled by three.* It is a stronger statement
than any golden case, because the expected image was **computed from the source file** rather
than snapped -- there was nothing to re-snap against, so the only way to pass was to be right.

**What it proves that the goldens cannot** is the whole chain end to end: an sRGB PNG decoded
by the sampler, multiplied by a white vertex tint, blended into an `_SRGB` attachment, re-encoded
by the hardware, blitted up by three, and read back -- and every one of those steps returned the
byte it was given. It also proves the two things the eleven goldens are structurally blind to,
since all eleven run at native: that the scale is 3 and not 1, and that the bars are 20 and 30
and not 30 and 20.

**`letterbox` is the case the card did not name and the row needed.** The specification in
`tooling.md` asked for scale 1 and scale 3 with the UI inside and outside -- four cases, none of
which has a bar in it, because 960x540 is exactly 3x 320x180. The row's central decision is what
happens when the window is *not* a multiple, and a suite of those four would have verified
everything except the thing that was actually decided. 1000x600 presents at 3x with 20 columns
and 30 rows of bars, and the image is expected 20 in and 30 down; a scale that rounded up, a
leftover leaning the wrong way, or bars an axis out all fail it in texels.

### The non-integer-multiple decision, stated

The window is usually not a multiple, so this is the common case rather than the edge:

- **The scale floors and the remainder becomes bars.** Fitting exactly would be a fractional
  scale, and a nearest blit at 3.125x doubles every 33rd column of texels -- a pattern that
  *moves when the window is dragged*. That is the artefact the row exists to remove,
  reintroduced by the step meant to remove it.
- **An odd leftover leans right and down.** `(W - width) / 2` truncates, so a 3-pixel remainder
  is 1 left and 2 right. Somebody has to take it and it has to be reproducible, or a
  letterboxed capture is a coin toss.
- **A window smaller than `V` even at 1x crops, centred, rather than shrinking.** There is no
  half scale that preserves a texel; downscaling would be the one place in the path where a
  texel authored is not a texel presented. A game in a too-small window sees the middle of its
  world at the size it was drawn.
- **A zero extent records nothing**, because a minimised window is a thing a window manager
  hands over and `vkCmdBlitImage` rejects it.

`pixelExact` turned out to be **four** things rather than the three the card's title names.
Beside the jitter, TAA and the tonemap curve there is the overlay's sampler: C5 chose linear
because an icon is drawn at whatever height a layout gives it, and at 1:1 linear is *nearly*
exact -- the sample lands on a texel centre and the neighbour's weight is zero. "Nearly" is
decided by how many sub-texel bits the hardware keeps, and that is not a margin a bit-exact
claim can be built on. It is one switch rather than four because it is one decision: a game
that turned off three of them has not made a decision, it has made a bug that presents as a
slightly soft sprite.

### What the estimate did not predict

- **Ordering, and it cost a full failing run to find.** The presentation fields have to be set
  *before* `render.init`, because `createRenderTargets` sizes every target from them. Set
  afterwards the flag parses, the field is assigned, and the frame is built at the window's
  resolution and then quietly agrees with itself about it -- four readback cases reporting "at
  1x" with the virtual resolution on the command line.
- **The comparison had to be a rectangle, not a frame.** The first version wrote a
  swapchain-sized expected image with the source expanded onto a black canvas, which reported
  512701 of 518400 pixels differing: the scene is still behind the image. What is being asserted
  is about the texels of the source and nothing else, so `compareReadback` compares the region
  and `comparePng` was left alone.
- **The source has to be opaque, and that is a property of the check rather than of the image.**
  `ui_test.png` has an alpha ramp by design (C5 wanted one). Alpha is blended in the framebuffer
  in linear space, so a translucent source would make the expectation a linear-space composite
  the comparison had to reproduce -- a rounding step between the file and the answer, in the one
  check that exists not to have any. `engine/assets/readback.png` is generated opaque, and every
  other property of it is a claim being checked: not square (a transposed blit), a one-texel
  border with four corner colours (which way an offset moved), a per-column ramp that is no
  multiple of any scale (a doubled column), a single-texel diagonal (any filter tap).
- **`--width` / `--height` had to exist.** A check whose answer is derived from the window has
  to *pin* the window rather than inherit it, which is the argument `--locked` already makes
  about the clock. Two rows in the number-flag table.
- **`readback` was not in the board's verification vocabulary**, because nothing had ever been
  able to run it. Added to `scripts/kanban.py` and to the closing-a-card table by this row.

### Deferred, with the trigger stated

- **A stretch or aspect-fill mode.** The only alternatives to bars are a fractional scale
  (refused by the guarantee) or a crop of the world (which changes what the game shows rather
  than how it is presented). One mode, stated in the header rather than selected.
- **Per-image sampler choice.** `pixelExact` swaps the whole overlay array between linear and
  nearest. Trigger: a game wanting a linear UI icon and a nearest sprite sheet at once.
- **Render targets that survive a resize at a fixed virtual resolution.** `createRenderTargets`
  rebuilds targets whose size did not change, since only the layout depends on the window. It is
  a handful of allocations on an event a human generates by dragging. Trigger: a measurement.
- **A `Present` GPU zone is recorded but not quoted.** No `trace` token on this card: the blit
  is a full-screen transfer that exists only where a virtual resolution does, and there is no
  before-arm to compare it against. P4 is the row that will have a sprite count to attach it to.

### Verification

- `scripts/golden.sh check release` -- **11 of 11 match.** Every case runs at native, so this is
  the check on the elision, and it held byte-identical across a change that moved 35 extent
  references.
- `scripts/readback.sh release` -- **5 of 5 bit-identical**, plus the resize soak clean.
- `./test.sh debug` -- **706 of 706**, including 11 new `Presentation` cases.
- `./test.sh asan` -- **706 of 706**.
- Validation, layers on, debug build: 240 frames at 1000x600 with `--resize-every 20` and a
  320x180 virtual resolution -- **zero errors and zero warnings** across 12 swapchain recreates,
  with the scale genuinely moving between 3x and 2x under the resize. A second run with
  `--ui-outside-virtual` and physics debug lines on -- zero errors, readback bit-identical.

### Found and left alone

**`--sync-validation` reports 2160 hazards, and it reported exactly the same 2160 before this
row.** Run at 1000x600 with and without `--virtual-resolution 320x180`, the counts are equal
and the eight distinct pipelines named are the same eight -- `depth_pyramid.comp`, `ssao.comp`,
`ssao_blur.comp`, `bloom_threshold.comp`, `ssr_rt.comp`, and the `lighting_rt`, `composite` and
`tonemap` fullscreen draws. Every one is `READ_AFTER_WRITE` against a
`SYNC_IMAGE_LAYOUT_TRANSITION`, and **none of them is `recordPresent`**: the blit and the clear
it added introduce no hazard the layer can see, which is the check worth having on new barriers.

So this is pre-existing and outside the card. It is recorded here rather than fixed because it
is a real finding with a real cost -- the standard in `tooling.md` is zero *validation* errors
and that still passes, while synchronization validation is opt-in and has never been part of
it. Whether those transitions are genuinely under-synchronised or the layer is being strict
about barriers that are correct is a question worth its own card, and it touches five passes
this row did not.
