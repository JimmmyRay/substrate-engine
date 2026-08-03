---
id: chore-the-frame-has-half-a-millisecond-nothing-names
title: The frame has half a millisecond nothing names
arc: chore
size: S
verification: trace, golden-11
---

# chore-the-frame-has-half-a-millisecond-nothing-names — The frame has half a millisecond nothing names

Every GPU pass inside `Frame` gets a `GpuScope`, and their medians still do not add up to
`Frame`. This card puts zones on whatever is in the gap — barriers, layout transitions, the
render-pass load and store — so the largest unattributed block in the frame has a name before
anyone tries to optimise it.

Measured on the demo scene, release, 4x MSAA, three runs of 900 frames, summing the zones
nested inside `Frame`. ~~`Cull` is excluded from every sum because `recordCull(phase 0)` runs
four lines *before* the `Frame` scope opens.~~ **False, and corrected while executing this
card:** C33 moved the cull inside the frame scope —
[`recordCull(f.cmd, frameSlot, 0, CullViews::Scene)`](../../../engine/gfx/Renderer.cpp) is
twenty-one lines *after* [`GpuScope frameZone`](../../../engine/gfx/Renderer.cpp), so `Cull`
belongs in every sum below and was wrongly left out of all four rows.

| arm | Σ children | `Frame` | gap |
|---|---|---|---|
| Sponza, no demo world | 3.164 | 3.229 | 0.065 (2.0%) |
| demo world | 4.838 | 5.377 | **0.539 (10.0%)** |
| demo `--no-rt` | 4.317 | 4.606 | 0.289 (6.3%) |
| demo `--no-particles` | 4.740 | 5.019 | 0.279 (5.6%) |

The control arm's 2% is the floor this instrumentation already achieves; the demo's 6-10% is
the finding. The demo adds four passes and skinned geometry, each with its own sync, which is
the obvious hypothesis — but `--no-rt` removes ~0.25 ms of the gap while `--no-particles`
keeps ray tracing and still shows only 0.279, and `SSR` swings ±0.2 ms between arms with
identical SSR settings. **The cause is unmeasured, and the point of this card is that it
cannot be measured with the zones that exist.**

Worth doing first among the lighting-and-frame optimisation cards, because it is the one that
says whether the others are aiming at the right thing. It is also the cheapest: no pixel
changes, so `golden-11` is a regression check rather than a decision.

**What I expect to be wrong about:** that the gap is one thing. It may be several small ones,
in which case the answer is a handful of zones and no follow-up card.

**Provenance.** Every figure above was measured at `37c2d44`, before the per-view refactor (C31, C32) landed. Re-run the arm before acting on a delta.

## Verification

- `scripts/baseline.py --config release --zones --samples 4 --runs 3 -- res:/Sponza/glTF/Sponza.gltf`
  — the demo arm above. The gap between `Frame` and the sum of its children must close, or the
  card must say what is still outside it and why.
- `scripts/golden.sh` — eleven cases, byte-identical. Adding zones changes no pixel; a moved
  one is a defect.

**One constraint rather than a risk:** `GpuProfiler` caps at 64 zones per frame
([`kMaxZonesPerFrame`](../../../engine/gfx/GpuProfiler.h)) and warns once on overflow. A
barrier-level sweep can reach that, and the warning is easy to miss in a log nobody reads.

## Reference update

[tooling.md](../../architecture/tooling.md), the `## Profiling` chapter — the zone list, and
whatever this finds about what sits between passes.

## Outcome

**The gap does not exist. It was an estimator, not a measurement, and the card's whole table
is the artifact of it.** Σ(median of each zone) is not median(Σ of the zones). Several GPU
zones are strongly right-skewed and skewed *together*, because a heavy frame is heavy in all of
them at once — `GBuffer` median 0.475 against p90 1.048, `Particles` 0.305 / 0.778, `AsRefit`
0.129 / 0.799, `SSR` 0.637 / 0.902. Sum-of-medians therefore undershoots median-`Frame` by
about 10% on the demo arm and 1.4% on the control arm, which reproduces this card's four rows
exactly. Dumping the raw timestamps and differencing *per frame* over 233 frames of the demo
arm: median `Frame` 5.271, median per-frame union of its children 5.259, **median residual
0.011 ms — 0.2%**. Every one of the eighteen inter-zone holes measures 0.001 ms in every frame,
and that residual is the bottom-of-pipe timestamp writes themselves. The frame was already
fully instrumented.

It also disposes of the three sub-findings the card offered as evidence: `--no-rt` "removing
0.25 ms of the gap" is `AsRefit`, the second-most-skewed zone, leaving the trace;
`--no-particles` removing another chunk is `Particles`; and `SSR` "swinging ±0.2 ms between
arms with identical settings" is the same skew read through a median.

What landed is the measurement rather than the zones. `scripts/baseline.py` gained
`unnamed_gpu_ms()` — per frame, `Frame`'s span less the **union** of the GPU zone spans clipped
to it, union so a nested zone counts once and clipping so a zone recorded outside `Frame`
neither counts as named time nor drags the residual negative — and `--zones` now closes the GPU
block with an `unnamed` row (`--json` gains `unnamedGpuMs`). It reads **0.012 ms median on the
demo arm (0.005 min, 0.020 max, 0.22% of `Frame`) and 0.009 ms on the control arm (0.28%)**.
Two GPU commands genuinely lacked a zone and now have one: `HiZLayout` on the depth-pyramid
layout transition, **0.001 ms**, inside `Frame`; and `InstanceUpload` on the instance-table copy
and material updates, **0.013 ms demo / 0.011 ms control**, which sits *outside* `Frame` by
construction and was deliberately left there — folding it in would change every `Frame` number
`tooling.md` publishes.

**What the estimate did not predict**, beyond the premise being false: `kMaxZonesPerFrame` was
never near. The card flagged the 64-zone cap as the constraint to watch; the demo arm records 19
zones and the overflow warning appears zero times in a run. And the nine passes absent from the
zone table — `Shadows`, `PunctualShadows`, `Forward`, `Fog`, `TAA`, `Velocity`, `Present`,
`Sprites`, `DebugLines` — all early-return before recording anything under these defaults
(shadows because `rtActive`, `Forward` on `blendedCommandCount == 0`, `Present` at native
resolution, the rest on feature flags), so their absence was correct and not a missing zone.

Two defects in existing text were caught and fixed: the card's provenance claim about `Cull`
(struck through above), and the `HiZLayout` comment written earlier in this card's own
execution, which asserted it was "the only GPU command inside `Frame` with no zone" and linked a
`tooling.md` section that did not exist.

Verification: `scripts/baseline.py --config release --zones --samples 4 --runs 3 --
res:/Sponza/glTF/Sponza.gltf`, 717 frames — `Frame` 5.402, `Lighting` 2.780, `unnamed` 0.012;
control arm, same command with no scene argument, 717 frames — `Frame` 3.202, `Lighting` 1.843,
`unnamed` 0.009. `scripts/golden.sh check release` — **11 of 11, byte-identical**, first run, no
device-lost. No pixel moved, as predicted.

**No follow-up card.** The card allowed for "several small ones, in which case the answer is a
handful of zones and no follow-up"; the answer turned out to be two small ones and a corrected
estimator. Its own claim to be worth doing first among the lighting-and-frame optimisation cards
holds for the opposite reason to the one it gave: those cards are not aiming at a phantom
0.54 ms, and `Lighting` at 2.78 of a 5.40 ms frame is where the time actually is.
