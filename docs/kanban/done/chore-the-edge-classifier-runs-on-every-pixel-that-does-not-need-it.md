---
id: chore-the-edge-classifier-runs-on-every-pixel-that-does-not-need-it
title: The edge classifier runs on every pixel that does not need it
arc: chore
size: S
verification: golden, trace
---

# chore-the-edge-classifier-runs-on-every-pixel-that-does-not-need-it — The edge classifier runs on every pixel that does not need it

`samplesAgree` re-derives the edge classification inside the lighting pass, on every pixel,
from `2(SAMPLE_COUNT - 1)` multisample fetches. This card computes it once into an R8 mask and
has the lighting pass read one texel.

The classifier costs six multisample `texelFetch` calls per pixel at 4x and fourteen at 8x
([`samplesAgree`](../../../engine/shaders/lighting_body.glsl)), and it
is paid on *every* pixel — including the roughly four in five that then take the fast path and
shade once. The same G-buffer data is available to any pass that runs after the geometry
passes, so the predicate can be evaluated once and stored.

This is not a case for a new toggle. The mask is **bit-identical** to what it replaces — same
predicate, same data — so exposing a setting would offer a choice with no observable
difference. It lives under `render.edgeMsaa`, which already exists
([the `render.edgeMsaa` row](../../../engine/core/Settings.h)) and already gates the feature.

`debugView == 7` already renders exactly this classification
([the `debugView == 7u` branch](../../../engine/shaders/lighting_body.glsl)), which
makes the mask directly comparable against the current predicate rather than inferred from a
timing change.

**Estimated saving 0.05-0.1 ms at 4x** — small, and the honest reason to do it is that it is
cheap and carries no quality risk, not that it is large. It grows at 8x, which the project
does not ship.

Context for why the classifier is worth keeping at all: `--no-edge-msaa` puts `Lighting` at
**6.564 ms against 2.790** on the demo scene at 4x. It is already a 2.35x saving, and it
differs from the full per-sample resolve on 0.462% of pixels by more than 2/255.

**What I expect to be wrong about:** that one texel beats six fetches. An R8 read is a
different cache path from a multisample fetch of data the shader is about to read anyway, and
the saving may land inside the noise floor. If it does, that is the result and the card closes
without the change.

**Provenance.** Every figure above was measured at `37c2d44`, before the per-view refactor (C31, C32) landed. Re-run the arm before acting on a delta.

## Verification

- `scripts/baseline.py --config release --zones --samples 4 --runs 3 -- res:/Sponza/glTF/Sponza.gltf`,
  several runs per arm, against a 0.05 ms noise floor. `Lighting` is the zone.
- `scripts/golden.sh` — thirteen cases, **byte-identical**. This card claims bit-exactness, so
  any moved pixel falsifies the claim rather than needing a re-snap.

## Reference update

[rendering.md](../../architecture/rendering.md), the hybrid MSAA section.

## Outcome

**Built, measured, reverted.** The card's own stated doubt was right, and the mechanism is
sharper than it guessed: the six fetches are not merely not-slower where they are, they are
**nearly free**. Nine runs per arm, medians from the trace:

| | before | after |
|---|---|---|
| `Lighting` | **2.799** (2.793 / 2.801 / 2.799) | **2.789** (2.796 / 2.784 / 2.789) |
| `EdgeMask` | — | **0.057** (0.057 / 0.056 / 0.057) |
| `Frame` | **5.121** | **5.164** |

`Lighting` moved −0.010 ms, one fifth of the card's 0.05 ms noise floor and inside a
run-to-run spread of 0.017 across all six invocations — zero, not "a saving in the noise". The
new pass cost 0.057, so `Lighting` + `EdgeMask` is **+0.047 ms (+1.7%)** and `Frame` is
**+0.043 ms (+0.84%)**. `samplesAgree` reads `gAlbedo` and `gNormal`, which the shader is about
to read anyway: the lines are already resident and the latency hides under shading. Given its
own pass it loses that residency and pays a render pass, two barriers and 1.4 MB of
write-then-read on top — about 6x.

**The replacement was correct, and that is worth stating separately from the cost.**
`--debug-view edges` was byte-identical across the change (`md5 386e255c...` both sides, `cmp`
clean), and the goldens were **13 of 13 byte-identical with the change in**, including `msaa1`,
`mirror` and `mirror-no-rt`. The revert is on cost alone. The 1x path needed a decision but no
special handling: `lighting1x.frag`'s `samplesAgree` is already `return true`, the pipeline was
not built at 1x, and the pass was never recorded — the image was still allocated and still
transitioned so the bound descriptor stayed valid at every sample count.

**Two things the card had wrong, both of which would have made this look like a win.** It frames
the saving as a `Lighting`-zone one, and `Lighting` is the zone that did not move: any
implementation necessarily moves work *out* of `Lighting` into a new zone, so a `Lighting`-only
comparison reports −0.010 while the frame gets 0.043 longer. **`Frame` is the number that
decides this card.** And the mask does not live purely under `render.edgeMsaa`: with
`--no-edge-msaa`, `--debug-view edges` still renders the classification because the
`debugView == 7u` branch calls `samplesAgree` unconditionally, so a mask needs a gate of
`edgeMsaa || debugView == Edges` — one more piece of state than "it lives under an existing row".

Also established: **the 0.05 ms noise floor is pessimistic for `Lighting` specifically** — six
invocations spanned 0.017 ms total. The documented bimodal ~5% behaviour appeared in `GBuffer`,
`SSAO` and `SSR` as expected and in neither `Lighting` nor `Frame`.

Final state: tree reverted and clean, release rebuilt from it, goldens 13 of 13, unit suite
1017/1017. No `VK_ERROR_DEVICE_LOST` occurred.

**Deferred, with a destination.** `--sync-validation` reports a wall of
`SYNC-HAZARD-READ_AFTER_WRITE` against `depth_pyramid.comp`, `ssao.comp`, `ssao_blur.comp`,
`lighting_rt.frag`, `ssr_rt.comp` and `composite.frag` — pre-existing, unrelated, and all of the
form "`SHADER_STORAGE_READ` on a `COMBINED_IMAGE_SAMPLER`", which reads like layer
misattribution but has never been classified. Opened as
[bug-sync-validation-reports-a-wall-of-hazards-nobody-has-read](../backlog/bug-sync-validation-reports-a-wall-of-hazards-nobody-has-read.md).

Reference updated: `rendering.md`'s edge-detect section now carries the declined optimisation,
its numbers, and the two traps — `Frame` not `Lighting`, and the debug-view gate.
