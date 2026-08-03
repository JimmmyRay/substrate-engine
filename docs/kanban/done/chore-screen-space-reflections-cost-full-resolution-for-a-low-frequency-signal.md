---
id: chore-screen-space-reflections-cost-full-resolution-for-a-low-frequency-signal
title: Screen-space reflections cost full resolution for a low-frequency signal
arc: chore
size: M
verification: golden, trace
---

# chore-screen-space-reflections-cost-full-resolution-for-a-low-frequency-signal — Screen-space reflections cost full resolution for a low-frequency signal

`recordSsr` dispatches one thread per pixel at full `renderExtent`. This card adds
`render.ssrScale`, a resolution fraction for `ssrTarget` and its dispatch, with a bilateral
upsample where the pass is read.

`SSR` is **0.643 ms of the demo's 5.377 ms GPU frame — 12%, the second-largest zone.** The
dispatch is `(renderExtent.width + 7) / 8` by the same in height
([`recordSsr`’s `vkCmdDispatch`](../../../engine/gfx/Renderer.cpp)) into a single-sampled
`ssrTarget` allocated at `extent`
([`view.ssrTarget` in `createRenderTargets`](../../../engine/gfx/Renderer.cpp)). Note it does **not**
scale with MSAA — it is per-pixel already — so this is not a 4x card; it is worth doing because
the zone is large, not because the baseline moved. **Estimated saving 0.3-0.4 ms.**

A float scale rather than a bool: 0.25-1.0, defaulting to 1.0. `ssrTarget` lives in
`createRenderTargets`, so a change rebuilds targets rather than pipelines — heavier than a
specialisation constant, but `render.msaaSamples` is already `kNone` and does exactly that
(the `1 2 4 8` keys switch it live), so both the precedent and the machinery exist.

**This is the highest quality risk of the optimisation set, for a reason specific to this
renderer.** `ssrRoughnessCutoff` defaults to **0.4**
([the `render.ssrRoughnessCutoff` row](../../../engine/core/Settings.h)), so SSR runs only on surfaces
*below* that roughness — the sharp, mirror-like half. Half-resolution is normally defensible
because reflections are blurry; here it is gated to precisely the cases where blur shows. The
second problem is the upsample: bilateral filtering bleeds across depth discontinuities, so
reflection edges at geometry boundaries are where it fails, and at a 4x baseline those are the
pixels the sample count was bought for.

**Do not start this before
[chore-reflections-have-no-golden](chore-reflections-have-no-golden.md).** There is currently
no golden case covering reflections at all, so this change would land with no automated
coverage of the thing it changes.

**What I expect to be wrong about:** whether 0.5 is usable at all given the roughness cutoff.
It is possible the honest answer is that the scale knob ships defaulting to 1.0, someone with a
weak GPU sets it to 0.5, and the project never recommends it — which is still a better outcome
than the pass having no knob.

**Provenance.** Every figure above was measured at `37c2d44`, before the per-view refactor (C31, C32) landed. Re-run the arm before acting on a delta.

## Verification

- `scripts/baseline.py --config release --zones --samples 4 --runs 3 -- res:/Sponza/glTF/Sponza.gltf`,
  **more runs per arm than usual**. `SSR` is the least stable zone in the trace: 0.643 and
  0.858 were measured across two arms whose SSR configuration was identical, with min 0.550 and
  max 2.668. A delta under ~0.25 ms is not readable.
- `scripts/golden.sh` at `ssrScale = 1.0` — thirteen cases, **byte-identical**, since the default path must not
  move.
- The reflection case from the prerequisite card, at 1.0 and at 0.5, compared deliberately.
  **Pixels will move at 0.5**; the card's job is to say how much and decide whether it is
  offered as a recommended setting or only as an escape hatch.

## Reference update

[rendering.md](../../architecture/rendering.md), the SSR section, and
[limitations.md](../../architecture/limitations.md) if half resolution turns out to have a
stated bound.

## Outcome

**`render.ssrScale` landed, 0.25-1.0, default 1.0 — and the first thing it had to do was refute
the reference.** `limitations.md` recorded that half-resolution SSR had already been tried and
reverted because the zone "is not ray-bound", with the composite and the barriers holding the
time; the card did not mention that entry. Measured today with temporary zones inside
`recordSsr`, the split is:

| | default (ray query) | `--no-rt` (march) |
|---|---|---|
| `SSR` total | 0.570 | 0.275 |
| trace dispatch | **0.528 (93%)** | **0.236 (86%)** |
| composite draw | 0.038 (6.7%) | 0.035 |
| both barrier groups | 0.002 | 0.002 |

The old entry's closing advice — "attack the composite, not the trace" — points at 6.7% of the
zone, and it is wrong in both paths. The likely reason it was ever true is that it predates
`ssr_rt.comp`: tracing a real ray and shading the hit is 0.29 ms more than the depth march. That
entry is now struck through with the measurement beside it.

**The knob pays, and by more than the readability floor.** Three invocations per arm, 239 frames
each, medians:

| arm | `SSR` | `Frame` |
|---|---|---|
| 1.0 | 0.575 / 0.577 / 0.579 | 5.166 / 5.215 / 5.220 |
| 0.5 | 0.286 / 0.286 / 0.286 | 4.830 / 4.858 / 4.897 |
| 0.25 | 0.175 / 0.189 | 4.680 / 4.743 |

1.0 → 0.5 is **−0.291 ms on `SSR` (−50%)** and **−0.35 ms on `Frame` (−6.7%)** — the arms do not
overlap at all, against a card-stated floor of 0.25 ms below which nothing is readable. VRAM
falls 9.3 MiB. The instrumented 0.5 run says where it goes: trace 0.528 → 0.226, composite
0.038 → 0.084, so the upsample costs 0.046 and the trace gives back 0.302. `Lighting` sat at
2.77-2.81 in every arm.

**The decision the card asked for: 0.5 is recommendable, 0.25 is an escape hatch.** On
`mirror.gltf` — the case the prerequisite card built for exactly this — 0.5 moves 36,154 of
1,440,000 pixels over tolerance (2.51%), max delta 236, mean 0.4882; `mirror-no-rt` moves 34,260
(2.38%). **Nothing above y=415 moves at all**: the bounding box is the mirror floor band and the
sphere row, and the sky, upper walls and every surface above the 0.4 roughness cutoff are
untouched — which is the roughness gate doing precisely what the card said it would. At 1:1 the
0.5 capture is indistinguishable from 1.0, including on the roughness-0.02 floor. At 0.25 the
moved pixels nearly double to 4.42% and the count above 128/255 goes 598 → 1452, which is where
the stepping becomes the reflection's dominant edge.

**The card's stated doubt was half right, and the half it missed is the interesting one.** It
expected the risk to be bleeding across the *reflecting* geometry's depth edges. The
joint-bilateral upsample handles that — four taps, bilinear weights times a relative reverse-Z
depth weight at 5% tolerance, nearest-texel fallback when every neighbour is rejected — and it
earns its 0.046 ms: forcing the tolerance to infinity, which is plain bilinear, moves 13% more
pixels and 23% more mean error. What survives it is stepping **inside the reflected content**,
because an edge in what is reflected carries no depth signal at the reflecting pixel. That is a
2-pixel staircase at 0.5 and a 4-pixel one at 0.25, visible at 3x magnification and not at 1:1.

Implementation notes worth keeping: a file-local `scaledBy(VkExtent2D, float)` returns the
extent untouched at 1.0, so the default path is the same integers; the dirty check compares the
**extent produced** rather than the float, so dragging a slider rebuilds once per pixel step
instead of once per frame; `ssr_body.glsl`'s dispatch grid comes from `pc.texel` rather than
`textureSize(litColor)`, with G-buffer fetches through a uv-derived `gcoord` that lands back on
`coord` exactly at 1.0; and the upsample pipeline is built only when the scale is below 1.0.
Three new shaders — `ssr_upsample_body.glsl` and the `ssr_upsample.frag` / `ssr_upsample1x.frag`
pair, following the existing `ssr.comp` / `ssr1x.comp` split.

Verification: **13 of 13 byte-identical** at the default, run twice. `./test.sh release` and
`./test.sh debug` both 1017/1017, Debug also running `spirv-val` over the three new shaders.
Validation clean at `ssrScale=0.5` at 4x, at 1x, and under `--resize-every 20` for 240 frames.
No `VK_ERROR_DEVICE_LOST` occurred.

Two corrections to the card's own figures: `SSR` measures 0.575-0.579 today rather than the
0.643 it recorded at `37c2d44`, so the per-view refactor moved it down and the 12%-of-frame
framing is 11%. Its 0.3-0.4 ms estimate is right at 0.25 and slightly optimistic at 0.5.

**Deferred, with a destination.** Every capture here is `--locked` with TAA off, so nothing says
whether the stepped silhouette is stable or crawls under camera motion — the one case that would
demote 0.5 to an escape hatch, and one a golden image structurally cannot see. Opened as
[chore-no-check-says-whether-a-half-resolution-reflection-crawls](../backlog/chore-no-check-says-whether-a-half-resolution-reflection-crawls.md).

Reference updated: `rendering.md` gains "`render.ssrScale`, and where the SSR zone's time
actually goes" with the split, the numbers and the artefact the filter cannot reach, and the SSR
row of the screen-space table points at the row; `limitations.md`'s half-resolution entry is
struck through and superseded.
