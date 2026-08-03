---
id: bug-the-debug-frame-spends-seven-milliseconds-recording-commands
title: The debug frame spends seven milliseconds recording commands
arc: bug
size: M
verification: trace, golden-11
---

# bug-the-debug-frame-spends-seven-milliseconds-recording-commands — The debug frame spends seven milliseconds recording commands

The debug build lost half its frame rate on 2026-07-31 — `./run.sh demo` went from 231 FPS
to 82 — and the whole of it is CPU time inside `Renderer::record`. This card finds what
`52c7037` did to put it there and takes it out.

**Bisected to a single commit before this card was opened.** What is *not* known is the
mechanism: nothing on the CPU side of that commit obviously costs seven milliseconds, and
`record` is a single profiler zone over twenty-five passes, so the trace cannot say which
pass holds it.

## The debug timeline

Every row below is the debug build. Nothing here is a debug-against-release comparison —
both sides of the regression are the same configuration with the same flags, which is what
makes it a regression rather than a difference.

```
2026-07-30 22:41  showcase.gltf   cpu= 0.989   241.6 FPS
2026-07-30 23:28  showcase.gltf   cpu= 1.391   216.1 FPS
2026-07-30 23:55  showcase.gltf   cpu= 1.411   231.0 FPS
2026-07-31 00:14  Sponza.gltf     cpu= 2.217   205.2 FPS
2026-07-31 00:15  Sponza.gltf     cpu= 1.796   341.5 FPS   <- last good
        ---- 52c7037 lands 00:21 ----
2026-07-31 00:43  showcase.gltf   cpu=12.153    81.8 FPS   <- first bad
2026-07-31 01:20  showcase.gltf   cpu=11.403    87.1 FPS
2026-07-31 02:22  physics.gltf    cpu= 7.508   132.5 FPS
2026-07-31 04:05  showcase.gltf   cpu= 8.090   122.5 FPS
```

Read out of `debug_frames/substrate.log`, which had kept 1377 runs and turned out to be a
complete before/after series. `run.sh` rebuilds on every launch, so each run's binary is the
tree as of its timestamp.

**Two neighbouring commits are exonerated by that bracket:**

- `d9f052a` (23:57) — the 00:14 and 00:15 runs are after it and still fast.
- `9ca76d8` (00:49) — it landed *after* the first bad run.

`52c7037` — "Give the camera a second projection, and the shaders one way to undo it" — is
the only commit between the last good run and the first bad one.

## Where the time goes

Medians over the 240-frame window of the 04:05 run, `showcase.gltf`, 4x MSAA, 1600x900,
from `debug_frames/profile.json`:

| zone | median |
|---|---|
| `Frame` (CPU, depth 0) | **8.012 ms** |
| `Renderer::record` | **7.544 ms** |
| `Frame` (GPU) | 3.541 ms |
| `simulate` | 0.411 ms |
| `writeback` | 0.014 ms |
| `Engine::spatialIndex` | 0.012 ms |
| `game` | 0.004 ms |

`record` is 94% of the CPU frame; everything else on the CPU sums to under 0.5 ms. The
frame's three blocking zones are `waitFence` 0.017, `acquire` 0.005, `present` 0.049 — so
the CPU is genuinely busy for those 7.5 ms rather than waiting on the GPU.

**The cost does not scale with content.** `physics.gltf` at **5 draws** costs 7.5 ms, the
same as `showcase.gltf` at **107**. It is a fixed per-frame cost, so culling and batching
have no purchase on it.

## What `52c7037` changed on the CPU side

The commit is mostly shader-side. What it touched in `Renderer.cpp` / `Renderer.h`:

- `FrameUniforms` gained `glm::vec4 depthLinear`, **inserted mid-struct** between
  `cameraForward` and `sunDirection`, so every member below it moved by 16 bytes.
- `SsrPush` lost a `float nearPlane` and gained a `uint32_t pad` — same size.
- `FogPush` lost `nearPlane` and a `pad`; `ParticleSimPush` was reordered and lost three
  pads.
- `flags.w` now carries an orthographic bit; `Renderer::cameraNear` was deleted.
- `updateUniforms` gained one `camera.depthLinear()` call per frame.

None of that is seven milliseconds of anything on its face, which is the finding rather than
a reason to doubt the bracket. The plausible shapes worth testing are a uniform buffer whose
size crossed a threshold and changed how it is placed or validated, and a per-frame
descriptor or push-constant path that the validation layers now charge much more for.

## What has been ruled out, and on what evidence

- **Validation error spam.** The 04:22:12 run costs 8.973 ms with **one** warning in the
  whole log. The 16–17 ms runs at 04:21:50 and 04:22:27 are `--sync-validation` runs, whose
  SYNC-HAZARD flood is separately explained and not this.
- **Per-draw cost** — identical at 5 and 107 draws.
- **The sprite arc (P4/P5/P6)** — `stats.sprites` is 0 in this scene, so `recordSprites`
  early-outs at `Renderer.cpp:610`.
- **The scene tree (G3)** — `Scene::update` sweeps every node every frame but runs inside
  `writeback`: 0.014 ms.
- **The node inspector (G6)** — not called from `engine/` at all; `ui.inspector` is `false`.
- **2D physics (P7)** — inside `simulate`, 0.411 ms.
- **G9's `DemoWorld`** — untracked, and `game/demo/CMakeLists.txt` does not compile it.
- **Per-frame pipeline recreation** — `variantPipeline` (`Renderer.cpp:2785`) caches.
- **Shader hot-reload polling** — `Renderer.cpp:6470`, outside the `record` scope, 1 Hz.

## A clue, not a comparison

Release did not move across the same commits. That is worth exactly one thing here: it says
whatever `52c7037` introduced is cheap when optimised and unvalidated, and expensive at `-O0`
with layers on. It is a hint about the mechanism's shape — not a reason to discount the
regression, which is real, is in the configuration the engine is developed in, and halved it.

Note also that the `-O0`-versus-validation question tracked by
[chore-the-debug-build-carries-a-tax-nobody-measured](../backlog/chore-the-debug-build-carries-a-tax-nobody-measured.md)
is **not** this card's question. Both sides of the bracket above carry the same flags. That
card is about the standing tax; this one is about a step change on top of it.

## Verification

- **Name the pass first.**
  [chore-one-profiler-zone-covers-every-pass-the-cpu-records](../backlog/chore-one-profiler-zone-covers-every-pass-the-cpu-records.md)
  is the instrument this card needs: with per-pass CPU zones inside `record`, one debug run
  says which of the twenty-five holds the 7.5 ms. Doing it the other way round means bisecting
  a mechanism blind.
- **Confirm the bracket by building it**, since the timeline above is inferred from a log
  rather than measured on purpose: `52c7037` and `52c7037~1`, debug, three runs each,
  `scripts/baseline.py --config debug --zones --samples 4 --runs 3`. Compare
  `Renderer::record` medians — never FPS, which moves with too much else.
- After the fix, the same command on `showcase.gltf` puts `Renderer::record` back near its
  pre-regression median — CPU busy around 1.4 ms, FPS above 200, in **debug**.
- `scripts/golden.sh` — eleven cases, byte-identical. `52c7037` landed golden-clean and the
  fix must too.
- `./test.sh debug`, then `./test.sh release` — each its own invocation.

Each arm run more than once: a run settles into one of two states about 5% apart and holds it
for every frame.

## Reference update

[architecture/tooling.md](../../architecture/tooling.md) — the "should the frame be threaded"
table quotes `Renderer::record` at ~0.08 ms and CPU busy at 0.14 ms without saying which
configuration those are from. A reader in debug cannot use them, and nothing caught a 55x
move against them. That section needs to name its configuration, whatever this card concludes.

## Outcome

**The premise held and had grown.** Re-measured before anything was believed: `showcase.gltf`,
4x MSAA, 1600x900, debug, 300 frames, medians over the 239-frame window — `Renderer::record`
**9.80 ms** against the card's 7.544, inside a `Frame` of 11.35 ms. Four independent runs at
HEAD landed between 9.79 and 9.98. So the day of P4, P6, G4, G5 and C17 that landed on top of
it did not dilute the cost; it added about two milliseconds to it.

**The bisect was wrong, and the reason it was wrong is worth more than the bracket was.**
`52c7037` is exonerated. The mechanism landed in **`9ca76d8`** — P1, "Hand a game its images
by handle, and let the array they live in grow" — which the card's own timeline placed
*after* the first bad run and therefore ruled out. What defeats that reasoning is a sentence
already on the card: `run.sh` rebuilds on every launch, so a run's binary is **the tree** as
of its timestamp, and a tree carries uncommitted work. The 00:43 run was a tree with P1's
edits in it; the commit followed at 00:49. A log of timestamped runs brackets a *working
tree*, never a commit, and nothing about the shape of that evidence says so. The Verification
section asked for the bracket to be confirmed by building both sides — it was not, because
attribution arrived first and is stronger than a timing bracket: the code that costs the time
did not exist at `52c7037`.

**Where the time went.** Twenty-five temporary `Profiler::scope` calls inside `record`, one
per pass, one debug run: `recordOverlay` **8.48 ms** of the 9.80, everything else summing
under 0.6 ms. Sub-dividing that pass put **8.47 ms on a single `vkCmdDraw`** — the overlay's
one unclipped text draw. The same build with `--validation off` records the whole frame in
**0.84 ms**, so the cost is entirely the validation layer.

**The mechanism.** `overlaySetLayout`'s one binding declared `imageSlotCeiling` descriptors —
`min(maxPerStageDescriptorSampledImages, maxDescriptorSetSampledImages)` less P6's 4096
reserve, which on this machine is **1,044,480**. The validation layer charges the binding's
*declared* count once per draw that samples the array, and the overlay's fragment shader is
the one thing in the frame that indexes it. Measured at three declared widths, holding
everything else fixed:

| declared descriptors | `Renderer::record` |
|---|---|
| 1,044,480 (the device ceiling) | 9.80 ms |
| 65,536 | 1.72 ms |
| 1,024 | 1.30 ms |

Linear at about **8 ns a descriptor per draw**. `imageCapacity` — what is actually allocated
and written — was 1 in every one of those runs, so the whole charge was for descriptors no
image will ever occupy.

**The fix is one line of intent: the layout declares the capacity, not the ceiling.**
`VARIABLE_DESCRIPTOR_COUNT` is removed and `createOverlaySetLayout` rebuilds the layout
inside `ensureImageCapacity`, so its width is the number of slots the array holds.
A layout of a different width is a different layout to everything built against it, so the
rebuild sets `pipelinesDirty` and `drawFrame`'s existing dirty check rebuilds the five
pipeline layouts and their pipelines — after the `vkDeviceWaitIdle` that `syncImages` already
takes, and only when the array doubles, which is log2(N) times over a run. The ceiling is
untouched: it is still the device's, `maxImageSlots()` still reports it to `ImageTable::init`,
and `kMaxOverlayImages` stays deleted. What changed is that the ceiling is now what the array
may grow *to* rather than what its layout claims to hold.

**Before and after**, `scripts/baseline.py --config debug --zones --samples 4 --runs 3
--frames 600`, Sponza, 717 frames per arm:

| | before | after |
|---|---|---|
| `Lighting` | 1.793 | 1.826 |
| GPU `Frame` | 3.072 | 3.382 |
| wall | 8.095 | 3.428 |
| **CPU busy** | **8.024** | **0.718** |
| FPS | 124 | 292 |

`Lighting` is flat across the arms and GPU `Frame` moves within its own spread, which is what
a CPU-side repair should look like. The card's target was CPU busy near 1.4 ms and FPS above
200 in debug; both are beaten. On `showcase.gltf` at 4x, the same 300-frame run as the
premise: `Renderer::record` **9.80 -> 1.29 ms**, `Frame` **11.35 -> 4.67 ms**.

**The instrument stayed temporary, and the boundary with
[chore-one-profiler-zone-covers-every-pass-the-cpu-records](../backlog/chore-one-profiler-zone-covers-every-pass-the-cpu-records.md)
is exactly where that card drew it.** Its argument is correct — the trace could say nothing
about which of twenty-five passes held the time, and per-pass scopes turned that into a name
in one run. But the zones it wants are a permanent instrument with its own verification, so
what went in here was three throwaway rounds of scopes, reverted before the fix was written;
nothing of them is in the tree. That card is still worth doing and its argument is now
stronger, not weaker: a regression of this shape found itself in twenty minutes with zones
and took a day and a log archive without them.

**What this says about the checks, and it is the part with teeth.** A per-draw cost the
validation layer charges is invisible to every check in
[tooling.md](../../architecture/tooling.md). The golden suite turns the HUD off, so the one
draw that paid it is never recorded; the readback suite passes at either width; the baseline
table is Release, where there is no layer to charge anything; and `Renderer::record` had a
published cost of `~0.08 ms` that named no configuration, so a 55x move against it in Debug
matched nothing. That section now states both configurations, and the Debug row is a number a
future regression can fail against.

**Not done here.** The standing `-O0`-plus-layers tax is still
[chore-the-debug-build-carries-a-tax-nobody-measured](../backlog/chore-the-debug-build-carries-a-tax-nobody-measured.md)'s
question — after this, Debug records in 1.29 ms against Release's ~0.08, and that ratio is
that card's to explain. `GltfScene`'s texture array declares `textures + 1 +
kTextureSlotHeadroom` and was never the shape of this defect, so it was left alone.

**Verification.** Golden 11 of 11 byte-identical. Readback 9 of 9 bit-identical plus the lit
silhouette, run because this change is in the descriptor array those cases draw through. 805
tests green under debug and asan. Zero validation errors in two debug runs, one with the
settings panel open and one at `--sprites 10000`, both of which exercised the growth path and
its pipeline rebuild — 1 -> 2 and 1 -> 4 slots.

**Reference.** [rendering.md](../../architecture/rendering.md) carries the mechanism and why
the layout is sized to the capacity; [tooling.md](../../architecture/tooling.md)'s threading
section now names its configuration and states Debug beside Release. The baseline table there
was separately stale — it quoted zones the profiler no longer emits under the defaults and a
4x `Lighting` of 0.979 — and was re-run rather than half-updated, which moved `README.md`'s
headline from 265 FPS to 305. That re-run is a re-measurement of today's tree and **not** an
effect of this card, which changes nothing in Release.
