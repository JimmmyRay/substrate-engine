---
id: chore-a-shadow-ray-is-traced-once-per-sample-and-answered-once-per-pixel
title: A shadow ray is traced once per sample and answered once per pixel
arc: chore
size: M
verification: golden, trace, validation
---

# chore-a-shadow-ray-is-traced-once-per-sample-and-answered-once-per-pixel — A shadow ray is traced once per sample and answered once per pixel

Shadow visibility is resolved inside `shadeSample`, which the MSAA resolve loop calls once per
sample — so an edge pixel traces the same lights four times at 4x. This card moves the trace
out of the sample loop into a per-fragment shadow mask written before lighting, leaving the
light loop to read a bit.

`Lighting` is **2.790 ms of the demo's 5.377 ms GPU frame — 52%** — and `--no-rt` puts it at
1.334, so **1.456 ms of it is traced shadowing** (release, 4x, three runs of 900 frames).
`lightShadow` is called from inside `shadeSample`
([the `lightShadow` call in `shadeSample`](../../../engine/shaders/lighting_body.glsl)), and the resolve
loop at [the `SAMPLE_COUNT` resolve loop in `main`](../../../engine/shaders/lighting_body.glsl)
calls `shadeSample` `SAMPLE_COUNT` times on any pixel where `samplesAgree` returns false.
Roughly a fifth of the screen fails that test — derived from `--no-edge-msaa` putting
`Lighting` at 6.564 against 2.790, solved against `4·c₁ = 6.564`. **Estimated saving 0.4-0.6
ms**, and it grows with sample count, which is why fixing 4x as the baseline promotes this
card.

**The quality loss is narrower than it looks, and the reason is worth stating.** `samplesAgree`
tests normal and albedo only
([`samplesAgree`](../../../engine/shaders/lighting_body.glsl)), so a
shadow boundary crossing a flat wall already collapses to one `shadeSample` at sample 0 and is
already resolved once per pixel. **MSAA is not currently buying anti-aliased shadow edges at
all.** What a per-pixel mask would lose is confined to pixels that *fail* `samplesAgree` — 
geometric silhouettes — and then only where samples straddling that silhouette disagree about
shadow state. A lit object edge against a shadowed background is exactly that case, which is
why the mask should be keyed per distinct fragment rather than per pixel: the classifier
already knows which pixels have more than one.

Follows the SSAO template for its toggle — a settings row, a specialisation constant gating
*the read* rather than the binding, and a pass that does not run — per
[`ENABLE_SSAO`](../../../engine/shaders/features.glsl). Take the next free constant
id; **id 2 is vacant and spoken for** by whatever brings indirect light back.

**What I expect to be wrong about:** the estimate. It assumes shadow cost is proportional to
its share of `shadeSample`, and the BVH traversals on edge pixels may be more coherent — and
so cheaper per ray — than the average.

**Provenance.** Every figure above was measured at `37c2d44`, before the per-view refactor (C31, C32) landed. Re-run the arm before acting on a delta.

## Verification

- `scripts/baseline.py --config release --zones --samples 4 --runs 3 -- res:/Sponza/glTF/Sponza.gltf`,
  several runs per arm. `Lighting` is the zone to quote; `GBuffer`, `Bloom` and `SSR` are
  bimodal and their deltas are not readable at this size.
- `scripts/golden.sh` — thirteen cases; `lit` and `no-rt` are the ones that carry this. **This card is
  expected to move pixels at silhouettes**, so a diff is a decision, not a pass: read it, and
  either accept and re-snap deliberately or change the fragment keying.
- Zero validation errors with layers on — a new target, a new descriptor and a new pass.
- The specialisation constant **must** be added to `featureKey()`
  ([`Renderer::featureKey`](../../../engine/gfx/Renderer.cpp)). Missing it makes the setting
  appear to work and change nothing, which reads as "the optimisation did not help".

## Reference update

[rendering.md](../../architecture/rendering.md), the deferred lighting section, and
[tooling.md](../../architecture/tooling.md)'s baseline table.

## Outcome

**Landed, and shipping off by default** — because turning it on costs six golden baselines, and a
re-snap is a separately authorized act this row did not have.

`shadowmask.frag` writes shadow visibility once per **distinct fragment** into an `r32ui` array,
a bit per light, 32 of them (`kShadowMaskLights` — the width of one texel, and deliberately not
`lightBudget`: a scene with a wider budget keeps its extra lights and traces them inline, which
both the mask pass and the light loop clamp to). It is a fragment shader with **no colour
attachment**, storing through `imageStore`, because `vUV` is what `worldFromDepth` reconstructs
the ray origin from and interpolating it exactly as the lighting pass does is the only way the
two agree on a grazing ray. `gbuffer_read.glsl` was extracted so the mask pass and the resolve
loop classify fragments through the same code rather than two copies. `VulkanContext` now
requests `fragmentStoresAndAtomics`; without it the layer rejects the module.

**Keyed per distinct fragment, and only where the resolve loop shades per sample.** The mask pass
runs `samplesAgree` and returns immediately where it is true, and the lighting pass's collapsed
branch passes `useMask = false` and still traces inline — so four fifths of the screen is
bit-for-bit unchanged. On the rest, sample `s` inherits the mask of the first earlier sample
`sampleMatches` accepts, else traces its own.

| | `Lighting` | `ShadowMask` | sum |
|---|---|---|---|
| 4x off | 2.799 / 2.813 / 2.809 | — | 2.807 |
| 4x on | 1.450 / 1.460 / 1.464 | 1.094 / 1.105 / 1.121 | **2.565** |
| 8x off | 4.632 / 4.622 | — | 4.627 |
| 8x on | 1.749 / 1.747 | 1.889 / 1.892 | **3.639** |

**0.24 ms at 4x, 0.99 ms at 8x** — 4.1x from one sample count to the next, exactly the shape the
card predicted, and **below its 0.4-0.6 ms estimate at the shipping baseline**. The reason is
measurable rather than a guess: the mask pass classifies all 1.44 M pixels to skip 83% of them,
costing ~0.42 ms of the 1.35 ms of edge-pixel ray work it deduplicates. Frame agrees at 8x
(~1.0 ms on the minima); at 4x the `Frame` median moves 0.53 but the minima move 0.22, and SSR's
bimodality is in that median, so 0.24 is the number to quote. VRAM cost is 23.0 MiB at 4x, 46.1
at 8x, 5.8 at 1x — allocated at every configuration so the lighting descriptor stays valid, as
SSAO's is.

**The trap the card named was checked by experiment, not by reading.** Two runs with
`--input-script 120:Toggle.RtShadowMask`, read off the trace: forward, `ShadowMask` recorded for
117 frames then never again and `Lighting` 1.076 → 1.829; reverse, `ShadowMask` absent then
appearing at frame 120, `Lighting` 1.845 → 0.975. A live row write rebuilding the pipeline
mid-run can only happen through `featureKey()`. Specialisation id **7**; id 2 left vacant as the
card required.

**Six golden cases move, and the attribution is exact rather than inferred.** `lit` 3691 pixels
(max delta 194), `physics` 40, `skin` 18, `emissive` 12, `particles` 6, `mirror` 1;
`albedo normal depth ssao msaa1 no-rt mirror-no-rt` unchanged. Each scene's `--debug-view edges`
capture *is* `samplesAgree`, and cross-referencing puts every moved pixel on a classifier-red
pixel — the 14 exceptions in `lit` are immediately adjacent to one, and the six `particles`
pixels that first read as off-edge were hidden under a blended particle, red when re-captured
with `--no-particles`. `--no-bloom` reproduces them, so it is not post-process bleed. The worst
region is Sponza's alpha-tested foliage, scattered single pixels rather than a structured band.
**The refactor itself is provably neutral**: with the row off, `lit`, `physics` and `emissive`
each compare 0/1,440,000, and the full suite is **13 of 13 byte-identical** — verified by `cmp`
against every baseline, not at the suite's tolerance of 2.

**The decision, and it is deliberately not mine to finish.** The card offered "accept and re-snap
deliberately or change the fragment keying". The keying is already the strict one the card asked
for, so the remaining choice is the re-snap — and `tooling.md` is explicit that a snap is a
separately authorized act taken by someone who has decided the new image is correct. The new
image is not more correct; it is an approximation bought for 0.24 ms at the shipping sample
count. So the row ships at `false`, the flag became `--rt-shadow-mask` rather than
`--no-rt-shadow-mask`, and everything needed to flip it — the numbers, the exact pixel counts and
the attribution — is written down here and in `rendering.md`. **Turning it on is one edit and six
re-snaps whenever that trade is judged worth it.**

**Two corrections to the card.** Its claim that "MSAA is not currently buying anti-aliased shadow
edges at all" is true of the shipping configuration and false of MSAA: the default image differs
from `--no-edge-msaa` at 13,768 non-edge pixels (max delta 184), and turning shadows off
collapses that to 95 — so **13,673 are antialiased shadow edges on flat surfaces**. Depth is
interpolated per sample and each sample reconstructs its own `P`. What discards them is
`ENABLE_EDGE_MSAA`, one pass earlier. The card's *argument* survives whole — the loss is confined
to pixels failing `samplesAgree`, and the measurement confirms that is where all of it landed —
but the AA is discarded, not absent. And its "1.456 ms is traced shadowing" came from `--no-rt`,
which also swaps SSR to the screen-space march; the direct arm is `--no-rt-shadows`, giving
`Lighting` 0.703 and therefore **2.11 ms** of traced shadowing at 4x.

Validation: **zero errors** at 4x, 8x, 1x and `--no-rt` — load-bearing here, since this adds a
target, a descriptor and a pass. Unit suite 1017/1017. One `VK_ERROR_DEVICE_LOST` at
`vkCreateDevice` on the first Debug launch; re-run once and it did not recur.
`--sync-validation`'s hazards include the two new pipelines and are the same tree-wide artefact
already carried by
[bug-sync-validation-reports-a-wall-of-hazards-nobody-has-read](../backlog/bug-sync-validation-reports-a-wall-of-hazards-nobody-has-read.md).

**Deferred, with a destination.** The lighting pass re-runs `samplesAgree` immediately after the
mask pass computed it; having the mask carry the classification would pay for most of that
0.42 ms and is the row that would move this trade. Opened as
[chore-the-mask-pass-classifies-what-the-lighting-pass-then-classifies-again](../backlog/chore-the-mask-pass-classifies-what-the-lighting-pass-then-classifies-again.md).

Reference updated: `rendering.md` gains "The shadow mask, and why it is off by default" under
ray-traced shadows, with the table, the pixel counts, the MSAA-shadow-AA correction and the
`--no-rt-shadows` attribution note. `tooling.md`'s baseline table is unchanged, correctly: the
default path did not move.
