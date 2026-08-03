---
id: chore-the-mask-pass-classifies-what-the-lighting-pass-then-classifies-again
title: The mask pass classifies what the lighting pass then classifies again
arc: chore
size: S
verification: golden, trace
---

# chore-the-mask-pass-classifies-what-the-lighting-pass-then-classifies-again — The mask pass classifies what the lighting pass then classifies again

`shadowmask.frag` runs `samplesAgree` over all 1.44 M pixels to decide which 17% it has work
for. The lighting pass then runs `samplesAgree` again, on the same pixels, from the same
G-buffer, to decide which branch to take. **The second one is free information the first one
already produced.**

It is measured: the classification costs about **0.42 ms of the 1.35 ms** of edge-pixel ray work
the mask deduplicates — which is why the mask's net saving is 0.24 ms at 4x rather than the
0.4-0.6 that was estimated. Paying for it once instead of twice is most of the gap.

The mask is an `r32ui` array with 32 light bits in a texel
([`kShadowMaskLights`](../../../engine/shaders/rayshadow.glsl)), so the obvious shape is to have
it carry the classification too and let the lighting pass read one texel rather than re-fetching
`gNormal` and `gAlbedo` for every sample. **There is a real question inside that**, and it is the
work: a spare bit in the same texel is free but couples the mask's layout to a light count that
is deliberately not `lightBudget`; a separate R8 image is clean but is a second image and a
second sample, and this tree has already measured a standalone R8 edge mask costing more than
the six fetches it replaced — see
[chore-the-edge-classifier-runs-on-every-pixel-that-does-not-need-it](../done/chore-the-edge-classifier-runs-on-every-pixel-that-does-not-need-it.md).
The difference here is that the classification is being computed **anyway**, by a pass that is
already writing to that pixel, so the earlier card's finding does not settle this one.

**Only reachable when `render.rtShadowMask` is on**, which it is not by default. That does not
make it dead work — it makes it the row that would move the mask's net saving far enough to
justify turning it on, which is the decision
[chore-a-shadow-ray-is-traced-once-per-sample-and-answered-once-per-pixel](../done/chore-a-shadow-ray-is-traced-once-per-sample-and-answered-once-per-pixel.md)
left open.

**Provenance.** Established at the commit that landed the mask. Both figures — the 0.42 ms and
the 1.35 ms — come from that card's instrumented run, not from a fresh measurement; re-run
before acting on a delta.

## Verification

- `scripts/baseline.py --config release --zones --samples 4 --runs 3 -- res:/Sponza/glTF/Sponza.gltf`
  with `--rt-shadow-mask`, several invocations per arm. `Lighting` **plus** `ShadowMask` is the
  quantity — either alone is misleading, because this moves work between them. Also at
  `--msaa 8`, where the mask's saving is four times larger and any regression will show first.
- `golden`: thirteen cases byte-identical **with the row off**, which is the default and the
  claim the shipping path makes. With the row on, the six cases it already moves must not move
  *further* — capture them before and after and compare the two on-arm images to each other, not
  to the baselines.

## Reference update

[rendering.md](../../architecture/rendering.md), "The shadow mask, and why it is off by default",
whose 0.24 ms figure this card is trying to change.

## Outcome

**Built, measured, reverted — and the card's premise is wrong by an order of magnitude.**

The shape chosen was an **extra array layer on the existing mask image**, not the spare bit and
not a separate R8: `shadowMask` created with `msaaSamples + 1` layers, layer `SAMPLE_COUNT`
holding the pixel's `samplesAgree` answer. A spare bit would silently redefine
`kShadowMaskLights` from "the width of one `r32ui` texel" to "the width less the classifier's
bit"; a layer is one image, one binding, one descriptor and one already-existing barrier, so the
earlier R8 finding genuinely did not apply to it.

It works and it is bit-neutral. Goldens with the row off: **13 of 13 byte-identical**, verified
by `cmp` against every baseline rather than at the suite's tolerance. With the row on, the six
cases the mask already moves were captured before and after and compared **to each other**:
`lit`, `physics`, `skin`, `emissive`, `particles` and `mirror` all **0 of 1,440,000**, mean delta
0.0000, `cmp`-identical. Validation zero errors and zero warnings at 4x and 8x with the mask on.
`./test.sh release` 1019/1019. No device-lost.

And it pays — just not for what the card wanted. `ShadowMask`'s **minimum is flat to 0.004 ms**
in both arms at both sample counts, so the added store costs nothing measurable and the whole
delta is in `Lighting`: **4x min 1.431 → 1.413 (−0.018 ms)**, **8x min 1.691 → 1.631
(−0.060 ms)**, non-overlapping across all six invocations at each count. The 3.3x ratio between
sample counts against a predicted 2.33x (12 versus 28 removed fetches) is the right order, which
is what makes it signal rather than drift. The mask's net saving moves 0.24 → ~0.26 at 4x and
0.99 → ~1.05 at 8x.

**The 0.42 ms this card was aiming at is the wrong number for it.** That figure is the cost of
the *mask pass's* classification — a cold walk of the G-buffer, and one this card cannot remove,
because the mask pass must classify to know which pixels it has rays for. What this card removes
is the *second* classification, in a pass running microseconds later over the same texels with
them still in cache. Measured, the second one is worth 0.018 ms at 4x, not 0.42. The two passes
were never paying the same price for the same work — which is the same mechanism that made the
standalone R8 edge mask cost more than the six fetches it replaced.

**Reverted, on three grounds.** It delivers 4% of what it was opened for, so it does not move the
decision it exists to move — 0.24 → 0.26 changes nothing about "off by default". It charges
**5.49 MiB in the default configuration**, where the row is off and it buys exactly nothing, on
top of the 23 MiB the mask image already costs there for the same reason. And it makes the last
array layer of an image whose layers are samples mean something that is not a sample, which is a
small cleverness with a permanent reading cost. The finding is worth more than the code, and it
is now in `rendering.md` beside the 0.42 ms so the next reader does not re-derive it.

Reference updated: `rendering.md`'s shadow-mask section records that the 0.42 ms is not
recoverable by classifying once, with both numbers and the cache reason.
