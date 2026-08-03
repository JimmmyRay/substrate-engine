---
id: chore-emissive-costs-four-samples-to-be-read-once
title: Emissive costs four samples to be read once
arc: chore
size: S-M
verification: golden, trace, validation
---

# chore-emissive-costs-four-samples-to-be-read-once — Emissive costs four samples to be read once

`gEmissive` is a multisampled G-buffer attachment whose value is fetched and added unmodified,
once, at the end of shading. This card resolves it to a single-sampled target in the G-buffer
pass instead, so a fifth of the G-buffer stops being multiplied by the sample count.

The G-buffer is **20 bytes per sample** — `R8G8B8A8_SRGB` albedo, `R16G16_SFLOAT` normal,
`R8G8B8A8_UNORM` ORM, `B10G11R11_UFLOAT_PACK32` emissive and `D32_SFLOAT` depth
([the `kAlbedoFormat`..`kHdrFormat` constants](../../../engine/gfx/Renderer.cpp)). Emissive is 4 of those 20,
so at 4x it is 16 bytes per pixel of write bandwidth and about 23 MiB of the 92 MiB that
separates 4x from 1x — measured VRAM is 508 MiB at 4x against 416 at 1x. Its only consumer is
one line at
[the emissive add at the end of `shadeSample`](../../../engine/shaders/lighting_body.glsl), added after
shading and unaffected by lights, shadowing or occlusion.

A hardware resolve attachment gives coverage-weighted emissive, which is arguably *more*
correct than the current behaviour: on a pixel where `samplesAgree` returns true the shader
broadcasts sample 0's emissive rather than averaging. **Estimated saving ~0.1 ms plus the
memory**, and both halves scale with the sample count, which is what makes it a 4x card.

**This one should not get a toggle, and that is a finding rather than an omission.** Making it
optional changes the attachment count and the descriptor set layout, and this tree already
established that a layout cannot be specialised away — it is why `lighting_rt.frag` is a
separate file rather than a constant inside `lighting.frag`. An optional emissive layout means
a second shader variant, a second pipeline layout and conditional target allocation, for
0.1 ms. Pick one layout and commit.

**What I expect to be wrong about:** the size. 4 bytes of 20 is a bandwidth argument, and the
G-buffer pass may be bound by something else entirely — `GBuffer` is one of the bimodal zones,
and a 0.1 ms claim sits inside its run-to-run swing. This may need more runs per arm than the
usual three to say anything at all.

**Provenance.** Every figure above was measured at `37c2d44`, before the per-view refactor (C31, C32) landed. Re-run the arm before acting on a delta.

## Verification

- `scripts/baseline.py --config release --zones --samples 4 --runs 3 -- res:/Sponza/glTF/Sponza.gltf`,
  and more runs than usual: `GBuffer` swings about 5% run to run, so a 0.1 ms delta on a
  0.48 ms zone is at the edge of readable. The `vramMiB` counter is the firmer number here.
- `scripts/golden.sh` — thirteen cases; the `emissive` case is the one that carries this, and it exists
  precisely because nothing else in `engine/assets/` sets `emissiveFactor`. **Pixels may move
  at partially-covered emissive silhouettes**; read the diff rather than re-snapping it.
- Zero validation errors with layers on — this changes attachment counts.

## Reference update

[rendering.md](../../architecture/rendering.md), the G-buffer layout table.

## Outcome

**Built, measured, reverted — and the card's central memory claim is structurally impossible, not
merely optimistic.** A hardware resolve attachment resolves *from* a multisampled attachment, and
one render pass instance cannot mix sample counts, so the multisampled `gEmissive` must still
exist and must still be written by the G-buffer pass. The change therefore **adds** a
single-sampled image rather than removing a multisampled one: **+6.3 MiB at 4x and at 8x**
(1600x900 B10G11R11 is 5.49 MiB plus VMA block rounding), widening the 4x-vs-1x gap from 92.1 to
98.4 MiB rather than closing 23 of it. The mechanism that would deliver the card's number is
lazily-allocated transient attachment memory, which this NVIDIA desktop driver does not offer.

The time did not appear either, and for a second independent reason. On an immediate-mode GPU the
multisampled samples are written during rasterisation regardless of `storeOp`, and NVIDIA's
colour compression already collapses the fully-covered case. Three invocations per arm at 4x,
medians in ms:

| | GBuffer | GBufferLate | Lighting | Frame | vramMiB |
|---|---|---|---|---|---|
| before | 0.479 / 0.478 / 0.479 | 0.036 | 2.791 / 2.800 / 2.797 | 5.202 / 5.299 / 5.144 | 515.1 |
| after | 0.478 / 0.480 / 0.479 | **0.049** | 2.794 / 2.790 / 2.791 | 5.353 / 5.240 / 5.271 | **521.4** |

`GBuffer` did not move at all. `Lighting` moved 0.14%, far inside a zone whose own min varies by
0.03 — the read side does drop from N fetches to one, but only on `samplesAgree == false` pixels,
which is the silhouette minority. `Frame` is inside its own ±0.08 swing. **The one consistent,
outside-the-noise delta is `GBufferLate` +0.013 ms**, identical in all three invocations at 4x and
again at 8x (0.047 → 0.060) — that is the resolve, and it is a cost. 1x allocated nothing, resolved
nothing and measured 423.0 MiB in both arms; 8x cost the same +6.3 MiB.

**The implementation was correct and the validation was the check that mattered**, since this
changed attachment counts: **zero errors at 1x, 2x, 4x and 8x**, 120 frames each. Golden: **13 of
13 match**, nothing re-snapped. One case moved pixels — `emissive`, **96 of 1,440,000, every one
exactly 1/255**, single channel, bounding box y 137-407 / x 599-910, tracing the emissive sphere's
rim against the sky with a few bloom-carried pixels just outside it. Nothing else in the frame
moved, and `lit`, `msaa1`, `mirror` and `particles` were byte-identical — `msaa1` holding exactly
is the 1x path proving it changed nothing. That move is the coverage-weighted silhouette a resolve
is arguably *more* correct about than broadcasting sample 0, exactly as the card predicted.

**The decision: reverted.** Ninety-six pixels below the suite's own tolerance of 2 is not worth
6.3 MiB of VRAM and a resolve in every frame, forever. The card's premise was a saving; the
measurement is a cost in both currencies, with a correctness gain nobody can see. Precedent from
[chore-the-edge-classifier-runs-on-every-pixel-that-does-not-need-it](chore-the-edge-classifier-runs-on-every-pixel-that-does-not-need-it.md),
closed the same way: do not keep a change that buys nothing.

**One trap recorded for anyone rebuilding it.** A resolved texel is *already* coverage-weighted,
so the emissive add has to move out of the per-sample loop into `main()`. Adding it inside and
dividing by `SAMPLE_COUNT` applies coverage twice and halves emissive at a half-covered
silhouette. The working implementation also needed `gEmissive` declared `sampler2D`
unconditionally in both `lighting_body.glsl` and `ssr_body.glsl` (SSR declares it only to match
the set layout and never fetches it), and the resolve on **phase 1 only** — nothing reads emissive
between the two G-buffer passes and phase 1 loads phase 0's samples, so one resolve covers both.

The card's own "what I expect to be wrong about" guessed the size might be wrong because `GBuffer`
is bound by something else. It is more specific than that: `GBuffer` did not move by any amount,
and the measurable delta landed in a different zone as a cost. Its reasoning about declining a
toggle was sound and was followed.

**Deferred, with a destination.** `--sync-validation` at 4x reports **2760**
`SYNC-HAZARD-READ_AFTER_WRITE` messages — and the identical 2760 with the identical per-shader
distribution at `--msaa 1`, where this change altered no barrier and allocated no image, which is
what proved they are pre-existing. That count and its distribution are now on
[bug-sync-validation-reports-a-wall-of-hazards-nobody-has-read](../backlog/bug-sync-validation-reports-a-wall-of-hazards-nobody-has-read.md).

Reference updated: `limitations.md` gains "Resolving the emissive attachment cannot save what it
looks like it saves", and the `gEmissive` row in `rendering.md`'s target table points at it.
