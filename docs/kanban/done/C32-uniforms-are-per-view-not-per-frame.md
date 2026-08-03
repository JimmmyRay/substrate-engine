---
id: C32
title: Uniforms are per view not per frame
arc: C
size: M
verification: golden-11, validation, trace
---

# C32 — Uniforms are per view not per frame

Second of C25's four. Afterwards the block a pass binds is chosen by `(frame slot, view)`
rather than by frame slot alone, so two views recorded into one command buffer do not
overwrite each other's matrices.

`FrameSync` holds one `uniforms` buffer, one `lightBuffer`, one `shadowMatrixBuffer` and one
`frameSet` per slot ([`Renderer.h:952-958`](../../../engine/gfx/Renderer.h#L952)).
`updateUniforms(camera, slot)` fills that one block, and every pass binds
`frames[slot].frameSet`. Record a second view into the same command buffer and the second
`updateUniforms` rewrites the block the first view's already-recorded draws will read at
submit time — so the first view silently renders with the second view's camera. **This is the
failure that looks like it works**: one view is correct, the other is subtly wrong, and
nothing is invalid.

Also per-frame today and per-view by nature, all written at the bottom of `updateUniforms`
or of `drawFrame`:

- `prevViewProj` ([`Renderer.h:1831`](../../../engine/gfx/Renderer.h#L1831), written
  [`Renderer.cpp:5501`](../../../engine/gfx/Renderer.cpp#L5501)) — reprojecting view B against
  view A's matrix is a smear that only appears when the camera moves.
- `taaHistoryIndex` (h1822, flipped at c7198) and `taaHistoryValid` (h1829).
- `framesSubmitted` (h2030), which drives the jitter phase at c5448.
- `cullViewProj[0]` (c5496), `visibleInstances`, `visibleTriangles`.

Slots 1 and 2..25 of `cullViewProj` are the sun and the punctual atlas and are **not** per
camera view — `updateSunShadow` deliberately takes no camera. Only slot 0 is contested, which
is what makes a second camera view a 27th list rather than a second set of 26.

The light selection is view-dependent by construction: `updateLights(slot, viewPosition,
viewProj)` ranks and culls against the camera, so its outputs are per view too.

Expected to be wrong about: whether the extra blocks are worth their memory when nothing has
asked for a second view yet. A `kMaxViews` of 2 costs one extra uniform block, light buffer
and shadow-matrix buffer per frame slot; a larger one should wait for a caller.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. Every case is one view, so view 0 must
  land in exactly the block it lands in today.
- Zero validation errors with layers on, including a capture where two blocks are bound in
  one command buffer.
- `trace`: the per-frame upload cost must not grow measurably for a one-view frame. A second
  block written every frame whether or not a second view exists is the mistake to catch here.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — the frame-uniform description
and the TAA section, both of which describe one block per frame.

## Outcome

`FrameSync` holds `kMaxViews` of each of `uniforms`, `lightBuffer`, `shadowMatrixBuffer` and
`frameSet`; `View` gained a `uniformSlot` that says which one it reads. **No pass needed a
signature for it** — every record method already reaches the view C31 gave it, so
`frames[slot].frameSet` became `frames[slot].frameSet[view.uniformSlot]` at fifteen call
sites and nothing else moved. That is the compounding C31 was landed alone to buy, and it is
worth recording as the concrete return: this row is 15 mechanical call sites and four
declarations, against the 445 the container cost.

`prevViewProj`, `visibleInstances` and `visibleTriangles` moved onto the `View` as well.
`taaHistoryIndex` and `taaHistoryValid` were already there — C31 took them when it took the
history images they index, which the card's inventory listed as this row's work and was not.

**Two of the card's six inventory items are deliberately not done, and that is a decision
rather than an omission.** `cullViewProj[0]` and the jitter phase are on
[C33](../backlog/C33-a-pass-chain-records-without-presenting.md) now, with the argument
written on that card. The reason is this card's own trace clause: *"A second block written
every frame whether or not a second view exists is the mistake to catch here."* Growing
`kCullViews` from 26 to 27 is a 27th cull dispatch in every frame, one-view frames included —
the same mistake in a different resource. C33 is the row that gives a 27th list something to
record, so it is the row that should add it.

**What the extra blocks actually cost.** One `FrameUniforms`, one light buffer at
`lightBufferCapacity` and one 24-matrix shadow buffer, per frame slot — under 0.05 MiB in
total, which is below the resolution of the VRAM figure the baseline table reports. They are
allocated up front rather than on demand because a view created mid-frame that had to
allocate its own would need the device idle to do it.

Expected to be wrong about whether the memory was worth it with no second view asking. It
was not the interesting question: the memory is unmeasurable and the *upload* is what the
card was right to worry about, since a block written unconditionally would have cost every
frame forever.

## Verification results

- `scripts/golden.sh check release` — **all 11 cases match**, byte-identical, exit 0.
- **And byte-identical again with `View::uniformSlot` defaulted to 1** — the whole eleven,
  through the second block, with **0 validation errors** over 240 frames and 8 resizes. This
  is the check that says block 1 is allocated, written, bound and *equivalent*, rather than
  merely allocated; a row that only ever exercised block 0 would have proved nothing about
  the machinery it added. The default is back at 0 and the experiment is not in the tree.
- `validation`: **0 validation errors**, release, 360 frames with layers on and
  `--resize-every 30`, which is 12 round trips through `destroyFrameResources` /
  `createFrameResources` and therefore 12 allocations and frees of `kMaxViews` sets per slot.
  The card asked for a capture with two blocks bound in one command buffer; **nothing binds
  two yet**, which is C33 by construction, and the substitute above is the strongest
  available statement in this row's scope.
- `trace`, `scripts/baseline.py`, the published table's own methodology (3 runs of 900
  frames, release, engine defaults). At 4x: **`Frame` 3.202 ms** against the published
  3.202, **`Lighting` 1.853** against 1.850, `GBuffer` 0.481 against 0.480, `wall` 3.280
  against 3.282, `CPU busy` 0.155 against 0.149. `updateUniforms` is **0.003 ms** median CPU
  with `updateLights` 0.002 inside it. The one-view frame did not get slower for holding the
  machinery for two, which is what the card asked.
- One observation the table run turned up and this row did not cause: **VRAM reads 505.0 MiB
  at 4x against the published 508.1**, at every sample count and by about the same 3 MiB.
  This row *adds* memory, so the drift is older than it and predates C31 as well. Not
  re-snapped here — a table re-snapped on a row that did not move it would attribute another
  row's change to this one.

## Deferred

- `cullViewProj[0]` and the TAA jitter phase — moved onto
  [C33](../backlog/C33-a-pass-chain-records-without-presenting.md), which is the row that
  can pay for them. Recorded on that card rather than here, because a Deferred entry that
  names a destination is the only kind that gets acted on.
