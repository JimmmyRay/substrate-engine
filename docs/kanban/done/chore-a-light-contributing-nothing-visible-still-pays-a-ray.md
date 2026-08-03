---
id: chore-a-light-contributing-nothing-visible-still-pays-a-ray
title: A light contributing nothing visible still pays a ray
arc: chore
size: S
verification: golden, trace
---

# chore-a-light-contributing-nothing-visible-still-pays-a-ray — A light contributing nothing visible still pays a ray

The light loop skips a light only when its radiance is *exactly* zero, so a light whose
contribution disappears in the tonemap still pays a full BVH traversal, per sample. This card
adds `render.lightCutoff`, a threshold below which a light is skipped, defaulting to `0.0f` —
today's behaviour, bit for bit.

The test is at
[the radiance test in `shadeSample`](../../../engine/shaders/lighting_body.glsl):

```glsl
if (dot(radiance, radiance) <= 0.0) continue;
```

It sheds lights that are out of range or below the horizon and nothing else. The shape this
matters for is many weak lights, which is measured: `game/demo/assets/stress.gltf` — 40 lights
over two meshes — puts `Lighting` at **3.003 ms of a 3.845 ms frame, 78%**, with `GBuffer` at
0.088 against Sponza's 0.477. Cost is tracking light count, not geometry.

The cheapest experiment on the optimisation list: one constant, one uniform, one `bench.sh`
run. It is a settings row rather than a specialisation constant — a float compare against zero
is nothing next to a traversal — so it needs no pipeline rebuild and is live-adjustable,
sitting next to `ssrRoughnessCutoff` and `lodThreshold` in shape
([the `render.ssrRoughnessCutoff` row](../../../engine/core/Settings.h)).

**This card gives up a property the shader currently holds deliberately.** The existing
early-outs are argued as bit-exact at
[the `dot(N, L)` note in `shadeSample`](../../../engine/shaders/lighting_body.glsl) — 
`shadeLight` returns exactly `vec3(0.0)` when NoL ≤ 0, so the skip is provably not an
approximation. A threshold is an approximation, and that is a different kind of change. The
default of `0.0f` is what makes it shippable before that argument is settled: nothing moves
until someone raises it.

Two failure modes for whoever does raise it, worst first. **Skipping the shadow but keeping
the contribution leaks a dim light through walls**, which is the visible artefact. Skipping the
light entirely can pop or band as the camera moves and a light crosses the threshold. And
"imperceptible" is pre-tonemap, so the threshold is exposure-dependent — exposure is per-game
data (`GameSetup::exposure`), not a constant, which is an argument for expressing the cutoff
relative to it rather than in absolute radiance.

**Provenance.** Every figure above was measured at `37c2d44`, before the per-view refactor (C31, C32) landed. Re-run the arm before acting on a delta.

## Verification

- `scripts/bench.sh 3 4 -- res:/Sponza/glTF/Sponza.gltf` at the default, then at a raised
  cutoff, and the same pair on `res:/stress.gltf` where the effect should be largest.
- `scripts/golden.sh` — eleven cases, **byte-identical at the default**. That is the claim the
  default is making, and a moved pixel at `lightCutoff = 0.0` is a defect.

## Reference update

[rendering.md](../../architecture/rendering.md), the lighting section, and
[limitations.md](../../architecture/limitations.md) if a raised cutoff turns out to need a
stated bound.

## Outcome

`render.lightCutoff` landed at every stop on the settings path — the `X(render, lightCutoff,
Float, float, 0.0f, 0.0f, 0.1f, kNone, ...)` row in `Settings.h`, from which the JSON parse,
`--set`, `--dump-settings`, `--write-default-config` and the generated panel all follow with no
further edit; `bindLive(Id::render_lightCutoff, &r.lightCutoff)` in `SettingsBind.cpp`, so the
table's storage *is* the renderer's field; a new `lightParams` vec4 in `FrameUniforms` and
`frame.glsl`; one extra test in `lighting_body.glsl` **after**, not replacing, the exact
`dot(radiance, radiance) <= 0.0` early-out. Both bit-exact early-outs are untouched. Golden:
**13 of 13**, and byte-identical in the strong sense — each `actual.png` was `cmp`'d against its
baseline rather than accepted at the suite's tolerance of 2, because "13 cases match" is a
tolerance-2 verdict and the claim the 0.0 default makes is stronger than that.

**Expressed post-exposure, not in absolute radiance.** `tonemap.frag` applies exposure as a bare
`hdr *= frame.params.x` immediately before the curve, and `Engine::initRenderer` takes it from
`GameSetup::exposure`, which the demo's own panel slides 0.1–4.0 at runtime — so the same
absolute radiance means a 40x different display brightness across that slider alone. Divided out
on the CPU once per frame and squared, so the shader keeps one compare against the `dot` it
already has: no extra ALU per light per sample.

**The card's central prediction is wrong, and that is the finding.** It says the effect should
be largest on `stress.gltf`; measured, that is the scene where the cutoff buys the *least*.
Three runs at 4x, medians from the trace:

| arm | `Lighting` | `Frame` |
|---|---|---|
| Sponza, 0.0 | 2.792 | 5.011 |
| Sponza, 0.004 | 2.716 (−2.7%) | 4.911 (−2.0%) |
| stress, 0.0 | 3.015 | 3.886 |
| stress, 0.004 | 3.004 (−0.4%) | 3.877 (−0.2%) |
| stress, 0.05 | 2.830 (−6.1%) | 3.734 (−3.9%) |
| stress, 0.1 | 2.534 (**−16.0%**) | 3.277 (−15.7%) |
| Sponza, 0.1 | 2.501 (−10.4%) | 4.777 (−4.7%) |

The reason is in the asset: every one of the forty stress lights carries `"range": 12.0`, and
the windowed `KHR_lights_punctual` falloff reaches exactly zero at the range — so the existing
bit-exact test already sheds every out-of-range light for free, and what remains genuinely
reaches the pixel. Sponza's auto-placed lights have no such window, which is why the low cutoff
pays there instead. **The shape this row rewards is weak lights with no authored range**, not
"many weak lights".

**A third failure mode, worse than the two the card named.** The cutoff does not bound the error
it causes: it compares *arriving* radiance, but what gets dropped is `shadeLight`'s product, and
`distributionGGX` at the roughness floor of 0.04 peaks near 1e5. That is how a light below 0.1
radiance moves a Sponza pixel by **143/255**. Light-through-walls and popping are both real, but
this is what breaks first. Safe headroom is scene-dependent by more than an order of magnitude —
stress tolerates the full 0.1 row ceiling with no pixel over 2/255, Sponza breaks between 0.01
and 0.05 — so **there is no single shippable non-zero default**, which is independent support
for the card's decision to ship 0.0.

Two defects found while in there. `Renderer.cpp`'s `updateUniforms` documented `ambient.w` as
"the SSR roughness cutoff for the lighting pass's reflectionCoverage" while `Renderer.h`
documented the same word as an intensity; it is neither — `frame.glsl` says `w unused`, nothing
samples it, and `reflectionCoverage` no longer exists anywhere in `engine/shaders/`. It is
exactly the slot a later reader would reach for as the spare, which is why this row added its
own vec4; both comments are corrected. And `./test.sh release` came back **1015/1016** with
`NavMesh.APathAcrossAFlatWorldStaysInTheFlatWorld` red — confirmed red at `8bad63f` by a
`--gtest_filter='NavMesh.*'` run of its own (27 tests, 26 pass), unrelated to anything here, and
opened as
[bug-the-funnel-leaves-a-corner-in-a-path-across-an-open-xy-plane](../backlog/bug-the-funnel-leaves-a-corner-in-a-path-across-an-open-xy-plane.md).

Reference updated: `rendering.md` gains "`render.lightCutoff`, and why it ships at zero" under
the budget section, and `limitations.md`'s "Many-lights scaling is delegated" now says why a
threshold is not the cheap version of a culling structure.
