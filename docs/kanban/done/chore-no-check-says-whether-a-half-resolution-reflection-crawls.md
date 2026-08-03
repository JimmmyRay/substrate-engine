---
id: chore-no-check-says-whether-a-half-resolution-reflection-crawls
title: No check says whether a half-resolution reflection crawls
arc: chore
size: S-M
verification: scripted-input, golden, inspection
---

# chore-no-check-says-whether-a-half-resolution-reflection-crawls — No check says whether a half-resolution reflection crawls

`render.ssrScale = 0.5` is recommended on the strength of still frames: 2.51% of pixels moved on
`mirror.gltf`, none of them above the roughness cutoff, indistinguishable at 1:1. **Every one of
those captures is `--locked` with TAA off, so nothing in the tree says whether the stepped
silhouette inside a reflection is stable or crawls as the camera moves.**

That is the one case that would demote 0.5 from a recommendation to an escape hatch, and it is
also the case the golden suite structurally cannot see: a golden is one frame, and temporal
stability is a property of a sequence.

The artefact to watch is specific. `rendering.md` records that the joint-bilateral upsample
rejects neighbours across a depth edge of the *reflecting* surface, but an edge inside the
reflected *content* carries no depth signal at the reflecting pixel — so the reflected building
silhouette inside a mirror sphere is a 2-pixel staircase at 0.5. A staircase that sits still is
invisible; one that swims a pixel per frame is the classic half-resolution tell.

**The check has to be a moving camera and a difference between frames, not an image.** The
shape that already exists for "what a run *did*" is `scripts/locomotion.sh`, which reads state
back from the far end of the chain rather than comparing pictures; the analogous thing here is a
scripted camera orbit over `mirror.gltf` and a per-frame metric on the reflection band — mean
absolute difference between consecutive frames, restricted to the pixels below the roughness
cutoff — compared between `ssrScale = 1.0` and `0.5`. If 0.5's inter-frame difference is close
to 1.0's, it does not crawl. If it is several times larger, it does.

Worth running with TAA on as well as off, since TAA is the mechanism that would hide it, and the
project ships with TAA off.

**Provenance.** Left open deliberately by
[chore-screen-space-reflections-cost-full-resolution-for-a-low-frequency-signal](../done/chore-screen-space-reflections-cost-full-resolution-for-a-low-frequency-signal.md),
which said it "could not test and will not claim either way".

## Verification

- `scripted-input`: a scripted camera path over `mirror.gltf`, both arms, the per-frame metric
  above. The number is the deliverable — a claim that it "looks stable" is exactly what this card
  exists to replace.
- `golden`: thirteen cases byte-identical. Adding a script and a metric must move no pixel; if a
  camera path is added to the golden suite it is a fourteenth case and a new baseline, not a
  change to an existing one.
- `inspection`: record what the metric was at 1.0, 0.5 and 0.25, and state on the card whether
  `rendering.md`'s recommendation of 0.5 stands, is qualified, or is withdrawn.

## Reference update

[rendering.md](../../architecture/rendering.md), the `render.ssrScale` section, whose
recommendation of 0.5 currently rests on still frames only, and
[limitations.md](../../architecture/limitations.md) if temporal stability turns out to bound it.

## Outcome

**It does not crawl — and the metric this card specified is, by itself, blind to whether it
does. Both halves are the finding.**

`scripts/ssr_stability.py` is the whole of the new code: no engine change, no new flag, no
fourteenth golden case. It assembles a sequence out of independent runs, which is legal because
of three properties the golden suite already rests on — frame N is a function of N under
`--locked`, `--camera-spin` is per frame and not scaled by `dt` (so a scripted orbit is exactly
repeatable, and the `--input-script` route this card suggested was not needed), and the renderer
is bit-identical run to run. It reuses `--no-ssr`, `--no-bloom`, `--taa`,
`--set render.ssrScale`, `--capture-frame` and `golden.sh`'s software-device refusal.

**The band is measured rather than located, and that correction matters.** `rendering.md` locates
the reflection band by a scanline, which stops being true the moment the camera orbits. Per frame
the harness takes the pixels that change when `--no-ssr` is passed — the pass's own definition of
where it acts — dilated by 4 px, since a quarter-res texel reaches four full-res pixels past the
scale-1.0 band. That is 36.0% of the frame.

Mean absolute difference between consecutive frames, 0-255 per channel, 11 pairs (frames 60-71),
`mirror.gltf`, 0.1 degrees of yaw per frame, RT on, 4x MSAA, release, bloom off so the control is
genuinely a control:

| arm | band | control | ≥8/255 | p99 | band vs 1.0 | p99 vs 1.0 |
|---|---|---|---|---|---|---|
| 1.0, TAA off | 0.8902 | 0.1878 | 1.22% | 12.0 | 1.00x | 1.00x |
| 0.5, TAA off | 0.8635 | 0.1878 | 1.84% | 22.2 | **0.97x** | **1.85x** |
| 0.25, TAA off | 0.9122 | 0.1878 | 1.89% | 26.4 | 1.02x | 2.20x |
| 1.0, TAA on | 0.8501 | 0.1831 | 2.36% | 23.5 | 1.00x | 1.00x |
| 0.5, TAA on | 0.8326 | 0.1831 | 2.59% | 23.7 | 0.98x | 1.01x |
| 0.25, TAA on | 0.8851 | 0.1832 | 2.78% | 24.5 | 1.04x | 1.04x |

The shipping frame (bloom on) agrees: 0.5 reads 1.00x/1.02x on the mean and 1.83x/1.04x at p99;
0.25 is worse on every column.

**The control is the number that makes the rest mean anything.** It is the complement of the band
— pixels `ssrScale` cannot reach — and with TAA and bloom off it is **0.1878 for all three
scales, bit for bit**, asserted by the harness and the assertion passed with zero channel values
moved. That is what "no crawl" looks like on this scene: 0.19/255 of frame-to-frame change from
camera motion alone. With bloom on the assertion is downgraded to a printed drift, because
bloom's chain reaches further than any fixed radius — it caught itself doing exactly that, 25
channel values at 0.25, which is why bloom-off is the default arm.

**Verdict: the 0.5 recommendation stands, qualified.** By the criterion this card wrote — "if it
is several times larger, it does" — 0.5 is **0.97x** with TAA off and 0.98x with TAA on. It does
not crawl. 0.25 is 1.02x/1.04x on the mean but 2.20x on the tail, which is exactly consistent with
"escape hatch".

The qualification is that **on the same pixels in the same frames, 0.5's mean is 0.97x while the
99th percentile of that same distribution is 1.85x** (12.0 → 22.2), and the share of band pixels
taking a ≥8/255 step each frame rises 1.22% → 1.84%. A mean cannot distinguish an edge sliding
smoothly across four pixels from one that waits three frames and jumps four — and hold-then-jump
is precisely what a staircase locked to a reduced grid does. So the staircase *does* move; it
carries no more total change per frame than full resolution's smoothly-sliding edge, concentrated
into about 0.24% of the frame. The extremes are untouched (≥64/255: 0.44% against 0.42%) and
p99.9 actually falls, 188.7 → 148.4. Confirmed visually as well: at 0.25 the reflected wall
silhouette inside a mirror sphere is an unmistakable staircase against 1.0's smooth curve.

**"TAA is the mechanism that would hide it" is half right**, and the correction is worth having.
TAA does close the p99 gap, 1.85x → 1.01x — but not by suppressing half resolution. It raises
*full resolution's own* tail from 12.0 to 23.5 with jitter, up to where half resolution already
was. TAA equalises rather than fixes.

**One reproducibility defect in the reference, found on the way.** `rendering.md`'s "2.51% of
pixels" and "nothing above y=415 moves at all" are at `--compare-tolerance 2`, the engine's
default, and the sentences do not say so. At tolerance 0, **12.69%** of pixels move, the topmost
moved row is **y=343**, and **4651 pixels above y=415** move by 1-2/255. The figures reproduce to
the digit at the stated tolerance — mean delta 0.4882 against the recorded 0.49, count above
128/255 exactly 598 — so the measurement is right and only the sentence was unfalsifiable.
Corrected.

Golden: **13 of 13, byte-identical.** Nothing tracked was modified, so no pixel could move.
`VK_ERROR_DEVICE_LOST` or a hang inside `vkCreateDevice` hit 3 times in ~220 launches; each was
re-run once and every one succeeded on the retry, and the harness now retries only when a run
never reached a device and prints when it does.

Reference updated: `rendering.md` gains "Does it crawl? Measured: no, and the mean is blind to the
question" with the table and the control, plus the tolerance correction on the still-frame
figures; `tooling.md` gains "Temporal stability is a sequence, and no image can hold one",
recording what the harness is, why a sequence can be assembled from independent runs here, why the
band is measured rather than located, and that it is a hand-run tool rather than a gate.
