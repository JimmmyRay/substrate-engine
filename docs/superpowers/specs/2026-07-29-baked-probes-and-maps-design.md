# RT-baked cubemap probes, replayed on hardware without ray tracing

> **Status: designed, not scheduled.** Shelved 2026-07-29 against a future need for frame time on
> the non-RT path. Nothing here has been built.
>
> **Revisit when any of these is true:**
> - The non-RT path needs frame time back, and `baseline.py` shows `Shadows` + the punctual atlas
>   as a meaningful share of `Frame`. Stage A deletes almost all of it for static content.
> - A scene wants more than ~4 static point lights and hits the `kMaxShadowLayers = 24` overflow
>   report. Stage A converts that ceiling from frame-time-bound to memory-bound.
> - Indirect light stops being acceptable as an authored constant. That is Stage B, and it is the
>   riskier half.
>
> **Do not revisit for:** better shadow *quality* on the RT path. Traced shadows measure +0.180 ms
> and are clean; nothing here improves them.

## Context

**The purpose, stated once and governing everything below: the build machine has ray tracing and
the target machine may not.** A bake at build time spends unlimited rays on hardware that has
them, and hands the result to a runtime that has none. The bake is a build step — nothing bakes at
load time or at runtime, ever, and there is no re-bake mechanism.

## How it runs

**After the binary is built, launch it with `--bake`.** The engine loads the game exactly as
normal — then restricts itself to static scene and static light geometry while it builds every probe
and map, writes them beside the scene, and exits.

"Exactly as normal" is load-bearing rather than convenient: **the light rig lives in C++, not in the
scene file.** `DemoGame::configure` authors `sunDirection`, `sunColor` and `sunIntensity`, and
`placeLights()` in `init` builds the point lights when the glTF declares none
([DemoGame.cpp:515](../../../game/demo/DemoGame.cpp#L515)). A baker that parsed only the glTF would
bake the wrong lighting. Running the game's `configure`/`init` is the only way to obtain what is
being baked.

**One concept does not exist yet.** `Light.h` has no static/dynamic flag and `renderer.lights` is a
flat `std::vector<GpuLight>`, so there is nothing for the bake to filter on. That distinction has to
be added, authored where the game authors its lights — and it is the same flag the contract below
turns on.

---

## What the bake produces

Two families, and the second is the larger win.

### 1. Baked maps — the shadows the runtime stops rendering

Two facts about the current non-RT path:

- `recordShadows` runs **every frame with no cache**
  ([Renderer.cpp:5507](../../../engine/gfx/Renderer.cpp#L5507)): a full scene depth pass into the
  4096² sun map, for a map that is **invariant** when a static sun shines on static geometry.
- The punctual atlas's `kMaxShadowLayers = 24` ceiling exists for exactly one stated reason — *"each
  layer is a scene re-render"*. Both the **0.270 ms for 7 layers**
  ([rendering.md:190](../../architecture/rendering.md#L190)) and the ~4-point-light practical limit
  are consequences of that cost, not of memory.

**Ship those layers pre-rendered and the cost is zero.** The static sun pass disappears; the atlas
ceiling stops being frame-time-bound and becomes memory-bound — 512² D16 is 0.5 MB a layer, so
dozens of static point lights become affordable where four are today. The runtime does the same
projection-and-compare it does now, against layers it *loaded* rather than layers it *rendered*, so
`punctualShadow()` and `shadowFactor()` need no change at all.

Dynamic casters still need a runtime pass; the point is that the static contribution is free and no
longer competes for layers.

**The optional upside, flagged honestly.** Because the bake has RT and unlimited time, it can trace
many rays per texel and store *filtered visibility with a penumbra width* rather than a depth —
real soft shadows that raster PCF can only fake, and no depth bias at all, since there is no depth
to quantise. That changes the lookup, where pre-rendered depth keeps it byte-identical. Take the
identical-lookup version first; the soft version is a second step with its own A/B.

### 2. Baked probes — the indirect light that is currently zero

The split-sum IBL chain was removed for being scene-blind indoors; the traced hemisphere gather that
replaced it was removed for Monte Carlo grain. What stands there is one authored constant —
[pbr.glsl:40](../../../engine/shaders/pbr.glsl#L40), `0.038, 0.032, 0.025` in the demo. Both
removals name a bake as the successor ([ibl.glsl:26](../../../engine/shaders/ibl.glsl#L26),
[DemoGame.cpp:489](../../../game/demo/DemoGame.cpp#L489)). This is the only genuine void in the
renderer.

---

## The construction

Sparse cube regions, 16–32 m, **overlapping**. Each holds a pair of RT-traced cubemaps at its
centre:

| | Contents | Size at 64² faces |
|---|---|---|
| **Radiance cube** | What the scene looks like from here — outgoing radiance of every visible surface, RT-traced with unlimited bounces | 196 KB RGBA16F |
| **Distance cube** | Distance to that surface per direction | 49 KB R16 |

At 8 m stride on Sponza (≈30 × 14 × 18 m) that is 4 × 2 × 3 = **24 probes, ~6 MB total.**

### These are marched, not interpolated — and that is the whole design

The pair is not a value stored at a point that needs interpolating between neighbours. It is a
**compressed proxy of the geometry visible from that point**, and the runtime queries it by
marching: given a direction from the shading point, walk the distance faces to find what is
actually there, then read radiance at that hit. That is parallax-*corrected*, so it stays valid
metres away from the cube's centre.

The reason radiance transfers correctly is that **diffuse emission is view-independent**: the
radiance the probe recorded leaving a wall is the same radiance that wall sends to a shading point
somewhere else. So a cube baked with full RT can be *replayed* by a card that cannot trace.

### What sets the spacing — coverage, not gradient

This is the crux, and it is worth stating precisely because the two regimes give answers an order
of magnitude apart:

- If a probe's value were **sampled at its centre and interpolated**, spacing would be set by how
  fast irradiance varies in space — the aperture and occluder scale, which in Sponza is metres.
- Because it is **marched**, spacing is set by whether every surface is **visible to at least one
  probe**. In open geometry that is tens of metres. In a compartmented interior it is much less.

Sponza is semi-open: the nave is visible from most of itself, the bays behind the columns are not.
So the failure mode is **disocclusion — surfaces no probe could see** — not blur and not leaking.

**Which is exactly what the overlap is for.** A surface hidden from one cube is visible to another,
and the blend must therefore be **coverage-weighted**: the distance cube already knows whether it
can see the point, so the same depth comparison that would have done leak rejection instead answers
*does this probe hold data for you*. A probe that cannot see the point contributes zero weight
rather than a wrong value.

### Overlap buys independence

Overlapping domains with a blended solution is a partition of unity, and it makes each cube
**independently bakeable**: no shared lattice, no boundary agreement, no seam to reconcile. Each
cube traces its own rays knowing nothing about its neighbours, and the blend makes the join
continuous. That gives the build step embarrassingly-parallel bakes across cubes.

### Sparsity rescues the cube array

A device limit, measured on the development GPU:

```
NVIDIA GeForce RTX 3060 Ti:  maxImageArrayLayers = 2048
```

Vulkan counts a cube as six layers, so 24 probes is **144 layers** — and two cube arrays
(radiance, distance) is 288. Comfortably inside the limit, which means a real `samplerCubeArray`
with **hardware seamless filtering across faces**: no octahedral fold, no border fixup, none of the
seam-debugging that a dense grid's atlas packing would have needed. It is the *density* of a fine
grid that forces octahedral; sparse cubes do not have that problem. Headroom is ~340 probes before
the limit binds, which is the number the scale disposition has to state.

---

## The contract

> **The bake is a build step.** The runtime loads a sidecar and samples it; that is its entire
> relationship with the volume. Nothing bakes at load or at runtime, there is no staleness
> recovery, and no runtime fallback regenerates anything.
>
> **Baked light is therefore static light** — baked against the scene's static geometry and its
> static light rig. A light authored *dynamic* is excluded from the bake and contributes direct
> lighting only, additively, at runtime, taking a runtime atlas layer as it does today. Dynamic
> geometry receives baked light and contributes none.

That flag is the one new concept the scheme needs, and it earns its place twice: it tells the bake
what to bake, and it tells `updateLights` which lights still need a runtime atlas layer — so the
layer budget is spent only on lights that actually move.

The sidecar's hash exists to **detect and report** a mismatch, never to trigger a bake: a stale
sidecar is a build error surfaced at load, not a condition the runtime repairs.

---

## Where it plugs in — two seams, both already conventions

**Indirect light.** `constantAmbient` has exactly three call sites —
[lighting_body.glsl:226](../../../engine/shaders/lighting_body.glsl#L226),
[forward.frag:94](../../../engine/shaders/forward.frag#L94),
[raytrace.glsl:232](../../../engine/shaders/raytrace.glsl#L232). Replacing that one function makes
the deferred pass, the forward pass and reflection hits agree by construction — the property the
last round of RT work fought for and won. The authored constant stays as the fallback when no
sidecar exists.

**Punctual occlusion.** The `params.w` sentinel is *already* the convention for "this light has no
shadow source", on both the atlas path (`firstLayer < 0` in `punctualShadow`) and the traced path.
So the branch is one line in a shape the codebase already uses:

```glsl
radiance *= (int(light.params.w) >= 0)
          ? punctualShadow(P, N, L, dist, int(light.params.w), lightType)  // atlas: real shape
          : probeOcclusion(P, N, L, dist);                                 // marched proxy
```

Never both, so no double-shadowing — and a light that today lights through a wall stops doing so.

---

## The two real risks

**1. Disocclusion holes.** Surfaces no probe can see get no indirect light and no occlusion. This is
the failure mode, it is deterministic and inspectable, and it is what probe placement has to solve.
Unlike the grain that killed the last attempt, a hole is a bug rather than a floor: you can capture
the coverage map, find the unseen surface, and add or move a cube.

**2. The march cost, which is genuinely unknown.** A dense grid would have been one texture fetch.
Marching a distance cube is a loop, per query. For indirect light that is per pixel; for
`probeOcclusion` it is per overflow light per pixel. On the non-RT path it competes with a 24-layer
atlas that re-renders the scene per layer, so it is not obviously worse — and not obviously cheap.
**This has to be measured before the design is committed to, not asserted.**

---

## What is already in place

- **The static *geometry* tier is the bake's validity domain.**
  [AccelStruct.h:108](../../../engine/gfx/AccelStruct.h#L108) — `staticBlas` is one BLAS with
  transforms baked in, stable for the scene's life. The light flag above is the only new concept;
  geometry already has one.
- **The estimator exists.** `shadeRayHit`
  ([raytrace.glsl:186](../../../engine/shaders/raytrace.glsl#L186)) resolves a hit to geometry and
  material and evaluates the full BRDF with traced shadows. A cube face is that, per texel.
- **Multi-bounce falls out of the `constantAmbient` seam.** Because `shadeRayHit` calls it, pointing
  it at the previous iteration's cubes gives bounce 2, 3, 4… inside the bake loop, at no runtime
  cost.
- **The descriptor slots are vacant and named.**
  [ibl.glsl:48](../../../engine/shaders/ibl.glsl#L48) says bindings 0 and 1 "held the irradiance and
  prefiltered cubes and are vacant" — yet
  [Renderer.cpp:1620](../../../engine/gfx/Renderer.cpp#L1620) still allocates, bakes and writes both
  cubes into them. **Built and unread.** The two cube arrays take those bindings with no renumbering
  and no layout change, and `irradiance.comp`/`prefilter.comp` and their images get deleted on the
  way past — removal, not accumulation.
- **`ENABLE_IBL` is a dead gate.** `features.glsl` declares constant_id 2 and no shader reads it, so
  `golden.sh`'s `no-ibl` case pins nothing today.
- **`.ktx2` is the sidecar precedent** — sibling file beside the glTF, found by
  `GltfScene::ktx2CachePath`, absent-is-normal at load
  ([manifest.py:352](../../../scripts/manifest.py#L352)), demanded for a release by
  `--require-cache`. ROADMAP **C14** already describes `build_release.sh` baking before it stages.
- **The baker is the engine binary with a flag.**
  [Config.h:28](../../../engine/core/Config.h#L28) already defines the category — *"per-invocation
  overrides with no key at all: `--headless`, `--locked`, the capture block"*. **C15 is not a
  prerequisite**; the baker that links the engine is the engine.
- **Emissive materials become light sources for free.** `shadeRayHit` already adds
  `m.emissiveFactor`, and commit 9382217 made the loader's parsed strength take effect — so the
  demo's orb bleeds colour onto the floor with no extra code. The attributable win to verify
  against.

---

## Sequencing — cheapest decisive experiment first

**Stage A: the baked maps, which need no marching and no probe placement.** `--bake` writes the sun
map and the static atlas layers; `setScene()` uploads them; `updateLights` assigns runtime layers
only to dynamic lights. The lookup shaders are untouched, so this is verifiable against the current
image as a *byte-identical* result at zero frame cost — the strongest possible A/B, and it delivers
the ceiling increase on its own. **This is the lowest-risk, highest-certainty half and should land
first.**

**Stage B0: measure probe coverage, before writing any probe runtime code.** Place cubes at a
candidate stride, trace outward, and report what fraction of static surface area is visible to
≥1 probe, and to ≥2. This is a bake-only question — it needs the ray generation and nothing else —
and it decides the stride, the cube size, and whether the whole scheme fits Sponza's arcade at all.
If coverage at 16 m is poor and only recovers near 4 m, the marched-proxy argument collapses back to
the dense-grid regime and the design changes shape. **Nothing downstream is worth building before
this number exists.**

**Stage B1: the probe cubes.** Bake shader, sidecar, runtime load, the march and the two lookups.
Answers the second open question — the march cost — against `scripts/baseline.py`.

**Stage C: wire it into the build.** `manifest.py` learns the sidecars so they ship in a package and
`--require-cache` can demand them; `build_release.sh` runs `--bake` before it stages, per C14's
shape.

### Implementation notes

- `--bake [path]` in `Config`'s category-2 developer block, beside `--capture`. Implies
  `--headless`; loads the game normally, bakes maps and probes from static geometry and static
  lights, writes, exits.
- A `static`/`dynamic` flag on the authored light, plumbed through `GameSetup`/`renderer.lights`
  into `GpuLight`. Read by the bake to select what it bakes, and by `updateLights` to select which
  lights still need a runtime atlas layer.
- The baked sun map and atlas layers uploaded in `setScene()` into the existing `shadowMap` and
  `punctualShadowMap` images, so `shadowFactor()` and `punctualShadow()` are unchanged.
- `probe_bake.comp` — one thread per cube-face texel, including `raytrace.glsl` at `RT_SCENE_SET`,
  calling `shadeRayHit`, writing radiance and distance. `recordProbeBake()` borrows the env-bake
  shape at [Renderer.cpp:1600](../../../engine/gfx/Renderer.cpp#L1600) — local pipelines, dispatch
  through `uploader->endImmediate`, scaffolding destroyed — but runs only under the flag, never on a
  normal launch. Iterated for bounces.
- Readback through the existing `captureStaging` path. Sidecar: header (cube centres, extents, face
  resolution, ray and bounce counts, and a hash of the light rig plus the scene's buffers) then the
  two payloads.
- `GltfScene::probeCachePath()` beside `ktx2CachePath()` — sibling lookup, absent is normal.
- `probeRadiance` / `probeDistance` as named `GpuImage` cube arrays, uploaded in `setScene()`, bound
  at ibl set bindings 0 and 1, added to the `--capture-target` table.
- `probes.glsl` — the march, the coverage-weighted blend, `probeIrradiance()` replacing
  `constantAmbient`, and `probeOcclusion()` for the `params.w < 0` branch. Its own spec constant,
  for the reason `ENABLE_PUNCTUAL_SHADOWS` got one: the cost is per light that reaches the pixel.
- Delete `irradianceCube`, `prefilteredCube`, `irradiance.comp`, `prefilter.comp`; repoint or rename
  `ENABLE_IBL`.
- **Scale disposition — Generalized:** cube placement sized from `GltfScene::boundsMin/Max` and a
  stride setting, with the ~340-probe cube-array ceiling stated, degradation by widening the stride,
  and a report when the ceiling binds. The pattern `lightBudget` and the particle pool follow.

---

## Settled, so it is not re-argued

Points reached during design that cost real thought. Recorded so a future reader inherits the
conclusion rather than the argument.

- **Lightmaps are closed, and not for the obvious reason.** `Vertex` is 48 bytes with one UV set
  ([GltfScene.h:26](../../../engine/scene/GltfScene.h#L26), mirrored as `RtVertex` and held at 48 by
  `scalar` layout); glTF carries `TEXCOORD_0` only. A lightmap needs a second non-overlapping
  parameterization — an unwrapper dependency, a wider `Vertex`, an atlas transform through the
  128-byte `GpuInstance`, and plumbing through `gbuffer.frag`, `forward.frag` **and** `shadeRayHit`
  or a lightmapped floor disagrees with its own reflection. Nothing in this design needs any of it,
  because everything here is either volumetric or in light space.

- **Probe spacing has two regimes, and the answer differs by an order of magnitude.** If a probe's
  value is *sampled at its centre and interpolated*, spacing is set by how fast irradiance varies —
  metres, because that is the aperture and occluder scale (Sponza's arcade openings ~2–4 m, columns
  ~0.5 m). If the probe is *marched* as a geometry proxy, spacing is set by whether every surface is
  **visible to at least one probe** — tens of metres in open geometry, much less in a warren. The
  marched regime is why 16–32 m cubes are viable here and why an interpolated grid at that spacing
  would not be. Do not carry a spacing number from one regime into the other.

- **Parallax is not the objection people reach for first.** A cubemap sampled at its centre suffers
  parallax error that angular resolution cannot fix. Marching the distance faces corrects it, and
  diffuse emission being view-independent is what makes the recorded radiance transferable to a
  shading point elsewhere. The residual failure is **disocclusion** — surfaces no probe could see —
  which is what the overlap and the coverage-weighted blend exist to answer.

- **Density is what forces octahedral packing, not cubemaps being wrong.** `maxImageArrayLayers` is
  2048 on the development GPU and Vulkan counts a cube as six layers, so ~340 cubes is the ceiling.
  Sparse cubes fit comfortably and get hardware seamless filtering; a metre-scale grid would not fit
  and would need an octahedral atlas with border fixup. The sparsity and the cubemaps are the same
  decision.

- **Two prior indirect-light features were removed, and this is the third attempt.** Split-sum IBL
  went for being scene-blind indoors; the traced hemisphere gather went for Monte Carlo grain, under
  a standing directive to prefer removing features over stacking noise mitigation. What makes this
  attempt different in kind is that its failure mode — a coverage hole — is deterministic and
  inspectable. A hole is a bug with a location. Grain was a floor.

- **Stage A carries almost none of that risk.** It re-renders nothing and changes no lookup, so its
  correctness test is an equality check rather than a judgement call.

## Verification

- **The trap this feature invites.** A missing sidecar renders a plausible image with no indirect
  light — "looks correct but is worthless", exactly what CLAUDE.md warns about for `run.sh tsan`.
  Log at load whether probes were found and what hash they carried, and make `golden.sh` fail rather
  than proceed when a case expects them and they are absent.
- **Stage A's A/B is the strongest available: byte-identical.** Baked depth layers should produce
  the *same image* as rendered ones, at zero frame cost. `golden.sh check` passing unchanged while
  `baseline.py` shows the shadow passes gone is the whole proof. If the image moves, the bake
  disagrees with the raster pass and that is a bug with an exact location.
- **The ceiling A/B:** `--no-rt` with more static point lights than 24 layers can hold. Today the
  overflow lights through walls and the log says so; with baked layers it should be shadowed, and
  the overflow report should stop firing.
- **The coverage map as a first-class output** — `--capture-target` on a per-surface coverage
  buffer, so an unlit region is diagnosable as "no probe sees this" rather than mysterious.
- `scripts/golden.sh snap` before, `check` after — shadowed-bay pixels are what should move; nothing
  in `albedo`/`normal`/`depth` should.
- `scripts/baseline.py` on `Lighting` and `Frame`, several runs per arm. Two separate claims: the
  indirect march is per pixel, and `probeOcclusion` is per overflow light per pixel. Neither is
  assumed free.
- Re-run the bake twice and diff the sidecars byte-for-byte: it must be deterministic, or the golden
  suite loses its footing.
