---
id: C5
title: Screen-space imagery
arc: C
size: M-L-M
verification: golden-12, validation
---

# C5 — Screen-space imagery

A texture index per overlay vertex, the font atlas as slot zero, `Renderer::loadImage`, `DrawList::image`, `ui::Context::image` -- all four as the row named them. ~~Reuses the bindless array — no second descriptor set~~ **The premise was false; the decision it forced is recorded below.** The overlay got its own array in its own set, which cost one descriptor set layout and came back an M after all: the L was the cost of *not knowing*, and the alternative it was measured against -- rebuilding the overlay pipeline on `setScene` -- is the one that would have been expensive, in a way no line count shows. Five hosted tests, zero standard-validation errors, twelve golden cases byte-identical, and the demo draws one so the descriptor half has a caller outside the suite

## C5's premise did not hold, and it is the whole reason the row was an M

The row said it reuses the scene's bindless texture array and therefore needs no second
descriptor set. **It could not.** Checked rather than assumed:

- The array lives in the *scene's* descriptor set — every pipeline that samples it is built
  against `scene->descriptorSetLayout()` ([`Renderer.cpp:2235`](../../../engine/gfx/Renderer.cpp#L2235)
  and the five sites after it), and that layout does not exist until `setScene`.
- The overlay pipeline is built with the base pipelines, against a set layout of its own
  ([`Renderer.cpp:2466`](../../../engine/gfx/Renderer.cpp#L2466) -- `singleImageSetLayout` when
  this was written, `overlaySetLayout` since), and it draws **before any scene is loaded** — the status and error text a user sees while a scene is still opening
  goes through it.

So reusing the scene's array means making the overlay pipeline scene-dependent: rebuilt on
`setScene`, and unavailable before one exists. That trades a working pre-scene text path for
a descriptor set, which is not a trade this row was proposing to make.

**The decision: a second image array, in the overlay's own set.** It keeps the overlay
independent of scene lifetime, which is a property worth more than the sharing was -- the
alternative rebuilds the overlay pipeline on `setScene` and has no text path before a scene
exists, which is to say no way to tell a user why the scene did not open.

Three things fell out of implementing it that the argument above did not predict:

- **It is one binding, not two.** The font atlas became slot zero of the array rather than a
  binding beside it, so there is exactly one place a UI texture is registered and
  `loadImage` has nothing to decide. `overlay.frag` reads slot zero as R8 coverage and every
  other slot as colour, which is one comparison in a shader that already branched on
  nothing. The row named this ("the font atlas as slot zero") and the re-costing lost it.
- **Its own layout, not a wider `singleImageSetLayout`.** That one is shared by four passes
  that want exactly one image, and widening it for the overlay would have widened it for all
  of them -- a descriptor array bound where a reflection buffer is meant.
- **Every slot is written at init**, with the atlas, and `PARTIALLY_BOUND` is deliberately
  absent. That is `GltfScene`'s own argument about its own array: a partially-bound array
  lets a shader read a slot nothing wrote, and a stale index is exactly how a vertex gets
  there. Written slots make a wrong index draw the font atlas -- visible, and harmless.

**The row was an M after all**, and the L was the price of not having checked. Worth keeping
as the general lesson: a size that comes from an unverified premise is not an estimate.

**What C5 left for a later row, and named while doing it.**
[`Renderer.h`](../../../engine/gfx/Renderer.h#L729) argues that the overlay's images need no unload
because "UI images are a handful of atlases and logos loaded at startup", and ends: *"The moment
a game streams UI art is the moment to revisit it."* That moment is a sprite atlas, and the
revisit is the board rather than C10 — a growable image table the overlay becomes
the first caller of, with its behaviour unchanged. C5 is closed and P1 must not reopen its
decision: the separate array was right, and what changed is only how many callers it has.

## What C5 found in a path it did not touch

The demo draws the test image so the descriptor half has a caller outside the unit suite --
the coverage gap D4 recorded about `decal`, not repeated. That caller immediately reported
eleven `SYNC-HAZARD-WRITE_AFTER_WRITE` from `Uploader::recordImageWithMips`, which C5 did
not write and did not change.

The transition that makes a fresh image writable covers **every** level and granted
`COPY_BIT` alone -- but only level 0 is written by the copy; levels 1..n-1 are first written
by `vkCmdBlitImage`, at `BLIT_BIT`. The same omission on the source side of the per-level
transition. Both fixed, and the final transition in the same function already named both
stages, which is what says these were oversights rather than a design.

**Nothing had ever exercised it.** The font atlas goes through this function with one mip
level, so the loop never runs; every scene texture in the tree comes from the KTX2 cache
through `addImageLevels`, which does not generate mips at all. A 64x64 PNG loaded for a UI
panel was the first mip chain this engine had generated since the cache landed.

`Animation.h` used to state "no retargeting, no IK, no event track" as a deliberate line, and
C7 moved it by exactly one item: an event is data already in the file, where a solver is not.
The header now says so. Retargeting and IK stay declined below, with their trigger unchanged.

Two decisions inside the row worth keeping:

- **Only the current clip fires.** A cross-fade advances the outgoing clip too, and firing
  its events would give one step two footsteps.
- **Every crossing in the step fires, capped at one per event per update.** The first half is
  what makes it work at 20 Hz -- a game must not drop footsteps exactly when it is already
  struggling. The second stops a dropped frame that lapped the clip eleven times producing
  eleven footsteps.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- Zero validation errors with layers on, in every capture.

## Reference

[architecture/rendering.md](../../architecture/rendering.md).

## Outcome

Recorded above, under *C5's premise did not hold, and it is the whole reason the row was an M*.
