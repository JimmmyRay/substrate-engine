---
id: C34
title: A game asks for a view
arc: C
size: M
verification: golden-11, validation, tests-hosted
---

# C34 — A game asks for a view

Last of C25's four. Afterwards a game creates a view with its own camera and extent, gets
back a handle, and can sample what that view drew — which is the point of the whole split, and
none of the first three rows is reachable from a game without it.

The renderer holds 26 cull view slots and offers a game zero. That was C25's sharpest
sentence and it stays true until this row: C31 gives the targets an owner, C32 gives the
matrices somewhere to live, C33 records the chain twice, and a game can still ask for none of
it.

What this has to decide, and what the three rows before it deliberately do not:

- **The handle.** `core::Handle<ViewTag>` with a generation, as every other engine table uses,
  so a view destroyed and a slot reused are distinguishable.
- **What a game gets back.** An image it can bind as a material texture is the useful answer —
  a mirror, a security monitor, a character-select preview and a minimap are all "sample the
  view somewhere in the scene". A capture to host is what `--capture-target` already does and
  is not this.
- **Who drives the camera.** The view holds a `scene::Camera` the game writes, resolved at the
  same point in the frame the main camera is. Not a callback.

Without this there is no split-screen, no mirror, no security monitor, no portal, no minimap
that shows the world, no character-select model preview and no second viewport for an editor —
and G17 already recorded that two players share one screen because the engine has one camera.

Expected to be wrong about: whether the engine surface is `Engine::createView` or whether a
game reaches `Renderer` directly. Every other resource a game makes goes through `Engine`, and
a view that did not would be the first exception.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. No golden case asks for a view.
- Zero validation errors with layers on, with a game-created view sampled by a material in the
  same frame it was drawn.
- `tests-hosted` for the handle table itself: creating, destroying and reusing a slot, with a
  stale handle refused. The table is host-side, so it is testable with no device — the drawing
  is not, and is covered by the two rows above.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) and
[limitations.md](../../architecture/limitations.md) — the latter records that two players
share one view, which is the row this closes.

## Outcome

`e.views().create(e.images())` returns a `ViewId`, `views().camera(id)` is the camera a game
writes between frames, and `views().image(id)` is what the view drew. `gfx::ViewTable` is the
lifetime half and holds no Vulkan; `Renderer::syncViews` is the residency half and reconciles
against `revision()`, exactly as `syncImages` does. All three of the card's open questions
resolved the way it guessed, and one of them for a reason the card did not give:

- **`Engine::views()`, not `Renderer`.** The card expected this and the argument turned out to
  be structural rather than aesthetic: the table has to be reachable from `init` before a
  device is guaranteed useful, it is the thing the unit suite tests, and taking `ImageTable&`
  by argument is what lets a view's destination be an ordinary image slot.
- **`core::Handle<ViewTag>` with a generation**, and the free list behaves like every other:
  a destroyed handle is refused rather than resolved, and a reused slot is a different view.
- **A camera the game writes, not a callback**, read at the same point in the frame the main
  camera is.

**What a game gets back is an `ImageId`, and that is the decision worth recording.** The card
said "an image it can bind as a material texture" and the cheap reading of that is a second
kind of texture handle — `viewImage(id)` returning a `GpuImage` the sprite path would need a
new branch for. Instead `ViewTable::create` adopts an ordinary slot through a new
`ImageTable::adopt`, so a view's output is the *same kind of thing* as a loaded PNG and every
consumer that already takes an `ImageId` takes this one with no change at all. The whole cost
is one `external` flag on an image entry and one `overlayBorrowed` flag beside the renderer's
handle.

**Two defects the verification caught, and neither was visible in a still frame.**

1. **A resize left the descriptor naming a destroyed image view.** `destroyViewTargets` frees
   every destination and `createViewTargets` builds new ones at the new extent, but the image
   descriptor array still named the old ones — and `syncViews` cannot cover it, because it
   reconciles against the *table's* revision and a resize does not move it. The rebind now
   happens in the same function that replaced the images. Reported as
   `VUID-vkCmdDraw-None-02699` on the first draw after the first resize.
2. **A double free at shutdown.** The destination handle is copied into `overlayImages` so the
   descriptor write can find it, and the image array's teardown frees everything in it — so
   the same `VkImage` was destroyed by `destroyViewTargets` and again by
   `destroyImageResources`. The first attempt guarded on `ImageTable::Entry::external`, which
   is wrong for a reason worth stating: **teardown runs after the table is gone**, so the
   borrow has to be tracked beside the handle rather than on the entry. Reported as
   `VUID-vkDestroyImageView-imageView-parameter` and a segfault.

Both were found by running the layers over a *game-created* view rather than an internally
registered one, which is the difference between this row's verification and C33's.

## Verification results

- `scripts/golden.sh check release` — **all 11 cases match**, byte-identical, exit 0. No
  golden case asks for a view, which is what makes that a statement about the one-view path.
- `validation`: **0 validation errors**, 300 frames with layers on and `--resize-every 60`,
  with a game-created view **sampled by a sprite in the same frame it was drawn** — five
  swapchain rebuilds through the path that produced defect 1, and a clean shutdown through
  the path that produced defect 2. The tonemap leaves its destination a colour attachment, so
  the chain now transitions it to `SHADER_READ_ONLY_OPTIMAL` before anything reads it; that
  transition is what makes a mirror a mirror rather than a one-frame-late mirror.
- The mirror is **not a black image**, which no validation run would have told me: a capture
  with the sprite over Sponza differs from the `lit` golden, and the sprite's region reads a
  mean channel value of **49.7/255 with 1599 of 2800 sampled pixels above black**. A view
  that rendered nothing would have passed every other check on this list.
- `tests-hosted`: `./test.sh debug` and `./test.sh asan`, each its own invocation — **1016
  tests, 104 suites, all passed** in both. Nine of them are `ViewTableTests`: a default handle
  naming nothing, a created view having a camera and an adopted image slot, the camera written
  through the handle, destroy releasing the image slot, a stale handle refused and a second
  destroy being a no-op, a reused slot being a different view, the table refusing past its
  capacity, the revision moving on lifetime changes **and not on a camera write**, and an
  out-of-range slot yielding a dead entry.
- The game-created view was driven by a temporary probe in `game/demo`, behind an environment
  variable, creating a view and a sprite that samples it. It is not in the tree: a switch that
  existed only to verify this row would outlive its reason, and the demo does not want a
  mirror in it.

## Deferred

- **Split-screen still is not a thing a game gets**, and the card's framing slightly
  overreached in listing it beside the mirror. A second view lands in an *image*; putting it
  in half the window is compositing the game does itself, at whatever size and place it likes.
  That is the right split — recorded in
  [limitations.md](../../architecture/limitations.md), which this row rewrote rather than
  deleted, since `scene::Camera` being driven by input player 0 is unchanged and is a
  different limitation from the one about views.
