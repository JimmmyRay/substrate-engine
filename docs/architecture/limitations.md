# Caveats and limitations

Everything the engine does not do, does only under stated conditions, or does by a
deliberate approximation. Organised by how likely it is to matter.

Every item here is also stated in the code where it applies. This document exists so the
list can be read in one place rather than assembled one file at a time.

---

## Rendering

### The importance metric is an approximation, and says so

Light and shadow-atlas budgets rank by luminance over squared distance, which models
neither a spot's cone nor a light's range. A spot aimed away from the camera scores the
same as one aimed at it, and a light whose falloff does not reach the viewer is not
zeroed.

Both are deliberate: the light illuminating the wall you are looking at is often not near
you, and discounting it by the angle to the *viewer* would drop exactly the wrong one. The
honest version needs each light's screen-space extent, which is clustered assignment — see
[below](#many-lights-scaling-is-delegated).

`lightImportance()`'s header states all of this, and the tests pin it.

### Zero-thickness geometry shadows itself

The shadow pass culls **front** faces, which is what removes most acne before any bias. A
surface with no back face has nothing to move the depth sample onto.

### Alpha-masked geometry casts the shadow of its whole quad under ray tracing

Everything in the acceleration structure is opaque, so alpha-masked foliage casts a
rectangular shadow, where the raster `shadow.frag` path cuts the silhouette out. That is
part of why ray-traced shadows complement the cascades rather than replace them.

The cascade pass cannot be skipped when ray tracing is on regardless, because volumetric
fog reads it — and now because the ray-traced ambient pass takes its bounce sun visibility
from it too.

### Ray-traced reflections and sky visibility are single-bounce

A ray hit is now shaded from its own geometry and material — `shadeRayHit` in
`raytrace.glsl` reads the vertex, index and material buffers by device address and
evaluates the BRDF there. What it cannot do is bounce again: inline ray query has no
recursion, so the environment term *at the hit* is the unoccluded split-sum lookup. A
reflection of a surface that is itself in deep shadow is therefore slightly too bright.

Indirect light from small intense emitters is understated. The ambient pass caps each
ray's contribution (`clampFirefly`, luminance 8) because four rays cannot resolve a bright
emitter subtending a tiny solid angle — the unbiased estimator is right on average and
visibly wrong in every individual pixel. The cap trades that variance for a known bias.

The ambient pass also runs at **half resolution** and takes the bounce's sun visibility
from the cascades rather than a second ray. Both are cost decisions with measurements
behind them (6.8 ms → 2.1 ms on Sponza at four rays); both are stated where they are made.

### Transparency sorts per object

Back to front by primitive centroid along the view direction. **Interpenetrating blended
geometry is still wrong**, which is the standard trade and the reason it is a sort rather
than a solution.

Blended surfaces also write no motion vectors, so under TAA they reproject against the
geometry behind them.

### TAA's motion correction is per instance, not per vertex

A character walking across a room reprojects correctly. An arm swinging within a
stationary body does not, because `prevInstances[]` is a per-*instance* transform.

The honest fix is double-buffered deformed vertices, which doubles the skinned vertex
buffer to correct a residual the neighbourhood clamp already fails safe on.

**Practical consequence:** `--characters 5 --taa` submits five velocity draws that all
correct nothing. A physics scene is where the pass demonstrably works.

### Skinned instances bypass frustum culling

Their world bounds are a per-frame quantity nothing computes, and a bind-pose box that is
wrong in the small direction makes a character vanish. An infinite box is the honest
answer; the cost is one command per character.

### Skinned and blended geometry gets no LOD either

Two different reasons that land in the same place. A skinned command draws out of the
buffer `skinning.comp` wrote, whose contents are its bind-pose vertices shifted by
`vertexOffset` — a simplified level indexes vertices the dispatch was never asked to
deform, so it would draw a character out of somebody else's pose. A blended one never
reaches `cull.comp` at all: the forward pass builds its own commands on the CPU, because
depth order is the whole point of it. **Trigger for the first:** a per-frame skinned bounds
computation, which is the same missing thing the row above names — LOD and frustum culling
want the same box.

### Cloth is placed once and cannot be moved afterwards (C19)

A soft body has no rigid transform to push down a node hierarchy, so a `FABRIC_` mesh's
placement is baked into its vertices at load and its instance transform is identity from
then on. **Animating a cloth's parent node does nothing** — the fabric stays where it was
authored, and the pins are what it hangs from. Moving cloth means moving what it is pinned
to, which is a body, and there is nothing today that attaches a pinned vertex to one.

### Cloth inherits every restriction a deformed instance has

No frustum culling and no LOD, for the reasons the two rows above give, and cloth adds
nothing to the argument: it draws out of the same buffer with the same infinite box.

Adjacent and deliberately not taken: the solver computes a real AABB every step, so cloth is
the first deformed instance that *could* have finite bounds and be culled. **Trigger:**
deciding what a skinned character's bounds are, which is the same missing computation the two
rows above name and is a different card.

### Cloth has no self-collision, no wind and no tearing, and is not appendable

Three omissions and one refusal. Self-collision, wind and tearing are scope this row named
and declined; Jolt supports the first, and the trigger for it is an asset where a curtain
passes through itself visibly rather than a wish for completeness.

The fourth is structural: `GltfScene::appendModel` refuses a document carrying cloth, for the
same reason it refuses a skinned or morphed one. `clothOffset` is the third offset into a
scene-wide array that function does not extend, so an appended cloth would take its inverse
masses from whatever the base scene had at that offset.

### A partial pin weight is heavy, not slow (C19)

`_PIN_WEIGHT` between 0 and 0.999 maps to an inverse mass between 1 and 0, and the intuitive
reading — that a heavier vertex moves less — is **false for a vertex in an inextensible
sheet**, which is every vertex of a cloth. A position-based constraint splits its correction
in proportion to the two inverse masses, so a heavier vertex receives less of the correction
pulling it back to its neighbour and sags marginally *further*. Measured over 30 steps: a
weight of 0.9 drops 126 um where a free vertex drops 94.

What the weight actually controls is the threshold: at or above 0.999 a vertex is nailed
down, and anywhere below it the vertex moves. Authoring a "half pin" to slow fabric down does
not work and there is no setting that does.

### LOD does nothing in Sponza, and the threshold's margin is thin

The default coverage threshold is **1/4096 of the viewport**, and it was measured rather
than chosen: at anything above ~0.00045 the golden suite's own reference camera selects a
coarser level for the potted plants at the far end of the atrium, and the reference set is
required to stay byte-identical. So the shipped value is the largest round fraction with
headroom under the nearest reference-camera call — a factor of **1.8**, which is thinner
than it looks comfortable to state and is the honest consequence of a reference scene that
contains objects at every scale in one frame.

What follows is that **the feature is inert in the repository's scenes**. Sponza is a
hundred large architectural draws in a thirty-unit atrium; there is nowhere to stand that
makes a chained mesh both visible and small, because the camera hits the end wall at twelve
units and every viewpoint outside the building sees a closed box. Forcing selection with
`--set render.lodThreshold=<f>` measures what it would buy: 20% fewer triangles drawn moves `GBuffer`
0.481 → 0.474 ms and `Frame` 3.384 → 3.339 ms, which is a percent of a frame that is not
triangle-bound. **This is C11's result again and for C11's reason** — the technique is
correct and the scene does not exercise it. **Trigger:** a scene with thousands of small
distinct meshes, which is what both rows are actually waiting for.

### Decals are albedo only

Projecting a normal map means blending octahedral coordinates, which is meaningless across
the encoding's fold.

`renderer.decals` is empty by default and filled from the game's `GameSetup`, because the
repository has no decal content. It is still content that does not travel with its scene,
which the code admits — but it is at least authored where the rest of the game's content
decisions are, rather than in a config file a user can edit (S1).

### Volumetric fog does not self-shadow correctly

It tests the cascades directly rather than calling `shadowFactor`, which offsets along a
surface normal — and a point in mid-air has none.

### The environment is procedural

No HDR asset exists in the repository and adding one brings a licence with it. `sky.comp`
generates the environment analytically. The rest of the IBL chain would accept an equirect
HDR unchanged; only that one shader is replaced.

**This is a feature for the golden suite**, because it is bit-identical between runs. Do
not "fix" it without reading the reasoning.

### Half resolution: taken for SSAO, measured and declined for SSR

SSAO now runs at half resolution with a 3x3 blur, **0.770 -> 0.150 ms** at 4x MSAA over 1195
frames. Occlusion is an integral over a hemisphere of world-space radius, so its detail is
bounded by the occluding geometry rather than the pixel grid. What it costs is contact detail
finer than two full-resolution pixels.

~~**SSR was tried at half resolution and reverted.** The zone did not move: **0.478 ms against
0.477**. It is not ray-bound — the rest of the zone is the full-resolution composite draw, two
`hdrTarget` layout transitions and the `ssrTarget` barriers, none of which shrink with the
compute grid. Anyone revisiting it should attack the composite, not the trace.~~

**Superseded, and it was wrong to reason from after `ssr_rt.comp` landed.** Measured with zones
inside `recordSsr`, the SSR zone is **93% trace dispatch** — 0.528 ms of 0.570 — with the
composite a fixed 0.038 ms and both barrier groups totalling 2 microseconds; even the `--no-rt`
march arm is 86% dispatch. The old entry's advice pointed the next reader at 6.7% of the zone.
The likely explanation for the original reading is that it predates the ray-query variant, which
traces a real ray and shades the hit, and is 0.29 ms more than the depth march.

`render.ssrScale` now exists, defaults to 1.0, and takes `SSR` to 0.286 ms and `Frame` down 6.7%
at 0.5 — see rendering.md for the numbers and the quality argument. What remains a stated limit
is **0.25**, which is an escape hatch and not a recommendation: it nearly doubles the moved
pixels on `mirror.gltf` and takes the count above 128/255 from 598 to 1452, where the reflected
content's stepping becomes the dominant edge in the reflection.

The particle pass is still fill-bound at about fifteen to one against its sort, and has no
low-resolution target.

### Resolving the emissive attachment cannot save what it looks like it saves

`gEmissive` is 4 bytes of a 20-byte-per-sample G-buffer, fetched and added unmodified once at
the end of shading — so resolving it to a single-sampled target looks like it should take a
fifth of the G-buffer out of the sample-count multiplication. **It cannot, and the reason is
structural rather than a measurement.** A hardware resolve attachment resolves *from* a
multisampled one, and a render pass instance cannot mix sample counts, so the multisampled
image must still exist and must still be written. The change therefore **adds** a
single-sampled image rather than removing a multisampled one: measured **+6.3 MiB at 4x and at
8x**, widening the 4x-vs-1x gap from 92.1 to 98.4 MiB. Lazily-allocated transient attachment
memory is the mechanism that would deliver it, and this NVIDIA desktop driver does not offer it.

The bandwidth half is unavailable for a second reason. On an immediate-mode GPU the
multisampled samples are written during rasterisation regardless of `storeOp`, and NVIDIA's
colour compression already collapses the fully-covered case — `GBuffer` measured 0.478-0.480 ms
in **both** arms. The read side does drop from N fetches to one, but only on
`samplesAgree == false` pixels, which is the silhouette minority, so `Lighting` did not move
either (2.796 against 2.792, inside its own spread). What the resolve costs is consistent and
outside the noise: **`GBufferLate` +0.013 ms** at 4x and at 8x, in every run.

It was built, measured and reverted. It is bit-honest — validation was clean at 1x, 2x, 4x and
8x, and the golden set held with `emissive` moving 96 pixels of 1,440,000 by exactly 1/255 along
the emissive sphere's rim, which is the coverage-weighted silhouette a resolve is *more* correct
about than broadcasting sample 0. Ninety-six pixels below the suite's own tolerance is not worth
6.3 MiB and a resolve in every frame.

One trap for anyone rebuilding it: the resolved texel is **already** coverage-weighted, so the
emissive add has to move out of the per-sample loop. Adding it inside and dividing by
`SAMPLE_COUNT` applies coverage twice and halves emissive at a half-covered silhouette.

### SSAO's occlusion is not available at a ray hit, and the gap is 4.5%

`ssao.comp` compared depths with the inequality reversed for as long as the pass existed. It
returned ~0 on open surfaces and stayed bright only at silhouettes — an edge detector, mean
**0.294** over a Sponza frame with an open floor reading **0.021**. Corrected, the same buffer
means **0.958** with that floor at **1.000**.

That number is why the ambient term stopped being occluded by SSAO once: the world darkened
3.5x where its reflection did not, and the supporting measurement — "a real half-metre
hemisphere trace returns ~1.0 where SSAO returns 0.3" — was read as SSAO over-occluding by
construction. It was not over-occluding, it was inverted, and the hemisphere trace had been
right all along.

SSAO is applied to the ambient term again. The genuine limitation that remains is the
asymmetry: SSAO is screen-space and a ray hit has no screen position, so `shadeRayHit` uses
the baked occlusion texture instead and a reflected crease is fractionally brighter than the
world crease beside it. Measured on `reflect.gltf` at an ambient high enough to matter, worst
region against the no-SSAO reference: **world 0.955, mirror 1.000** — a 4.5% divergence where
the inverted version gave 63%. The forward pass has no `ssaoMap` binding at all, so blended
surfaces take baked occlusion only, for the same reason and at the same scale.

SSAO also still reaches only about half a metre, so it says nothing about a vault twenty units
up. It shapes creases; it is not a substitute for indirect light.

---

## Scale limits

Each of these is a *stated* limit that reports when it binds, not a silent truncation. See
[principles.md](principles.md#3-designing-for-scale) for the disposition each carries.

| Limit | Behaviour past it |
|---|---|
| Light count | Nobody states one (C40, D21). Ranks by importance and keeps the top N for **one** frame, then grows the light buffer to what the view wanted and shades them all |
| Punctual shadow atlas layers | First fit by importance — first fit rather than stop, because a point needing six may not fit where a spot needing one still does. The rest are counted and reported |
| Particle count | Nobody states one (C40, D21). A spawn past the pool is counted and reported for the step it happens in; `Engine::growParticles` then resizes the pool and the renderer's buffers together |
| Particle pool at 65,536 | Hard. The sort key packs a quantised distance and a slot index into 32 bits; past it the honest answer is a 64-bit key or a radix sort |
| Body count | Nobody states one (C40, D21). `PhysicsWorld::grow` rebuilds the Jolt system at double the size and carries every body, cloth and character across; `refusedBodies` now counts only what Jolt itself would not build |
| `maxStepsPerFrame` | Remaining whole steps are dropped, counted and reported — the spiral of death answered by a stated policy |
| `kMaxOverlayQuads` (4096) | Excess dropped, warns once per run |
| `SpatialIndex` traversal stack (64) | A tree deeper than this would truncate a query. Asserted against in the suite: 20,000 spread instances build to depth 17 |
| Resident images (`gfx::ImageTable`) | The device's own `maxPerStageDescriptorSampledImages` against `maxDescriptorSetSampledImages`, whichever binds first -- not an engine constant. A `load` past it is refused and says so, and the invalid handle it returns draws the font atlas rather than nothing, because every allocated slot is written |
| Debug line vertices (16384) | Same |
| `kMaxInstancesPerCommand` (64) | A culling decision, not a submission one: a command is the unit the cull can switch off |
| `GpuProfiler` zones (64) | Warns once on overflow |
| `scopef` name pool (4096) | Capped, with the contract in the header |

### A light only a secondary view can see illuminates but does not occlude

Every view ranks its own lights (C38): `updateLights` culls and ranks against the matrix and
position it is handed, so a view looking elsewhere shades what *it* can see. **The atlas
assignment is the one half that stays the primary's**, and it has to: `recordPunctualShadows`
renders one assignment for the whole frame, before any secondary chain runs, so a second
assignment would put matrices in that view's buffer describing layers the atlas does not hold —
and the view would sample the wrong light's depth, which is silent and reads as a shadow bug.

So a secondary view looks its lights up in what the primary decided, keyed by source index. A
light the primary also ranked keeps its correct layer. **A light only this view ranked gets
`params.w = -1` and casts no shadow** — it illuminates without occluding.

That is not a new failure mode. It is exactly what the atlas already does to a light that does
not fit, and the reasoning is the same one stated a few lines above it in `updateLights`: dropping
the light changes the image far more than losing its shadow does.

**The trigger for paying more is a per-view atlas**, and it is expensive enough to want measuring
first: up to 24 layer re-renders per view, which on C38's numbers — one view 3.317 ms, four
full-extent views 13.985 — is another frame again. Two cheaper shapes exist if it ever matters: a
shared atlas with per-view layer ranges, so a light shadowed in two views occupies two layers and
the cap becomes how many views can have shadowed lights at all; or ranking per view while
shadowing only what the primary shadows, which is what happens today.

### ~~Many-lights scaling is delegated~~ — tiled assignment since C35

The culling structure this entry said was not built now exists: 16x16 screen tiles with per-tile
depth bounds, on by default, byte-equivalent with the escape hatch off. See rendering.md, "Tiled
light assignment". What remains true, and is the useful half of the old entry:

**A cap is still a cap.** Tiling narrows which lights a *pixel* iterates; it does not remove
the light buffer's own capacity, and above 1024 lights the pass refuses to assign and falls back to the
flat loop rather than truncating. The importance *approximation* the budget ranks by is
unchanged — tiling was described here as what would replace it with a measurement of on-screen
contribution, and it does not: it culls per tile at shading time and never revisits which lights
were admitted to the frame.

**And `stress.gltf` was the wrong scene to have named.** Its forty lights all reach the whole
visible ground, so there is no residue to cull and `Lighting` does not move on it at all. The
scene that demonstrates the feature is the same one with a shorter authored range, where it takes
61% of `Lighting`. A scene has to have many lights *whose reach does not cover the screen* — which
is not the same claim, and is the one worth carrying forward.

**A radiance threshold is not the cheap version of it, and the measurement says so.**
`render.lightCutoff` exists and ships at `0.0f` — see rendering.md for the numbers. It has no
bound on the error it causes, because it compares arriving radiance while what gets dropped is
a product containing `distributionGGX`, which peaks near 1e5 at the roughness floor; a light
under 0.1 radiance moves a Sponza pixel by 143/255. Its safe ceiling varies by more than an
order of magnitude between scenes, in the direction opposite to the obvious prediction: it buys
16% on `stress.gltf` with nothing visible, because those forty lights carry authored ranges the
exact early-out already sheds, and breaks on Sponza between 0.01 and 0.05, because Sponza's
auto-placed lights have no falloff window. **A non-zero default is per-scene tuning with an
unbounded failure mode**, which is a reason to build the culling structure rather than a
substitute for it.

### ~~There is no octree, and no CPU spatial index~~ — `scene::SpatialIndex` since C9

The renderer still has none and still wants none: a tree accelerates a CPU testing objects one
at a time, the GPU tests all of them at once in 0.010 ms, and building one for the renderer
would have been a structure with no caller.

What the delegation said — that it is worth having for **picking, physics broadphase and audio
occlusion**, all subsystems — is what C9 built, on the subsystem side. See
[systems.md](systems.md#the-spatial-index). It is a broadphase over instance *boxes*, so a
caller that needs a surface still owns its narrow phase, and the inspector still selects from a
list rather than by clicking in the viewport.

### Multi-threaded command recording is delegated

After indirect submission, recording cost scales with passes rather than draws, so
threading it buys far less than it would have before.

---

## Platforms and packaging

### The Windows build is not verified on a Windows GPU

It is cross-compiled from Linux with MinGW-w64 and is **compile- and link-verified**. One
check runs on every build: the import table carries only system DLLs. That is what caught
the `-static` flag applying to games and not to the test binary, and it is worth keeping —
but it inspects a file, and no Windows code is ever executed.

**Nothing runs the Windows build.** The unit suite is compiled for Windows and not run; the
installer is produced and not installed. The image that builds it
(`docker/windows.Dockerfile`) contains no way to execute a PE at all.

This used to be answered by wine, which ran the suite and the installer on every build. That
was removed deliberately: wine is not Windows, so a pass was a proxy rather than a result,
and the prefix it wrote inside `build/` carried a `dosdevices/z:` symlink to `/` that turned
every editor and indexer walking the repository into one walking the entire filesystem. The
cost was concrete and daily; the signal was indicative at best. See
[`docker/release.sh`](../../docker/release.sh) for the constraint that replaces it.

What that leaves unverified — assume broken until someone runs it on a Windows machine:

| Area | Status |
|---|---|
| **The unit suite on Windows semantics** | Built, never run. `Logger`'s `%zu` handling, `localtime_s`, the `Profiler` writer thread, `fs::path`, `Resources`' two-tree lookup, Jolt and miniaudio are all unexercised as Windows code |
| **That the installer installs** | The NSIS package is produced but never executed. Whether `engine/assets` and `game/<name>/assets` keep their depth — which the composite scenes' relative references depend on, and which a flattened layout silently breaks — rests on the staged tree alone |
| Swapchain formats, present modes, fullscreen-exclusive, alt-tab, device-lost | Needs a real ICD and a display driver |
| Which `VkTimeDomainEXT`s the driver advertises | Windows drivers offer `QUERY_PERFORMANCE_COUNTER` rather than `CLOCK_MONOTONIC`, so `GpuProfiler` falls back to frame-relative zones. The fallback is safe; whether it is taken is unknown |
| DPI scaling | `glfwGetWindowContentScale` unexercised |
| XInput gamepads, and the rebind round trip against a real `%APPDATA%` | No device, no real profile directory. Since C26 this includes *which slot* a pad lands in and whether two pads stay separated — the thing local co-op rests on |
| WASAPI end to end | miniaudio picks its Windows backend at runtime; only the null backend has ever been exercised |
| RenderDoc's `GetModuleHandleA("renderdoc.dll")` | Never loaded on Windows |
| Console VT rendering | Legacy conhost only |

The top two rows are the ones that changed. Both were checked before and are not now, and
they are the first things to restore if a Windows machine — or a Windows CI runner, which is
the cheaper answer and gives a real result rather than a proxy — becomes available.

Whatever runs it, `--frames N` is mandatory for any automated Windows run, since nothing on
Windows will deliver the signal that would otherwise stop it. A run that reached a software
rasteriser would be **no signal at all**, and the engine already says so at startup: a
software rasteriser producing a frame is worse than failing, because it looks like success.

### MinGW does not run `thread_local` destructors, so profiler slots are not recycled

`Profiler`'s per-thread slots are released by a `thread_local` guard's destructor at thread
exit. Statically linked winpthreads does not register those through `__cxa_thread_atexit`,
so on Windows the slot is never marked free and the next thread takes a new one — observed
directly as `first=2, second=3` where Linux gives the same id twice.

Nothing is incorrect as a result; the registry grows with each thread rather than reusing
rows, which is a slow leak and a noisier trace in a build that spawns job threads. The Linux
property is still required and still checked, and
`ProfilerTest.ThreadSlotsAreRecycledRatherThanAccumulated` skips on Windows naming this
rather than being deleted. The fix, when it is worth opening the profiler for, is `FlsAlloc`
with a destructor callback, which does run.

## Simulation and determinism

### Determinism holds for the same binary on the same machine — and no further

Six properties were audited. Five hold:

| Property | Status |
|---|---|
| A fixed step with a fixed call order | Holds. One `FixedClock`; animation, particles, physics and audio all advance on it |
| No entropy inside the step | Holds, and was paid for: the particle path refuses a GPU dead list with `atomicAdd`, and no simulation translation unit reads a clock |
| A fixed worker-thread count | Holds. `physics.workerThreads` defaults to **0** for this reason, not for performance |
| State addressable by a stable id | Holds. `InstanceId` is index plus generation |
| Input separable from the frame it was sampled on | Holds. Actions resolve once per frame |
| **Bit-identical across machines** | **Fails, deliberately** |

Jolt's `CROSS_PLATFORM_DETERMINISTIC` is **off**, because what the golden suite needs is
that the same binary produces the same frame on the same machine, and Jolt gives that
unconditionally for a fixed step, thread count and call order.

Anything needing two machines to agree on the result of the same step — network prediction
and reconciliation, most obviously — needs that switch on, and it carries a cost Jolt's
own documentation states. **Flipping it is the first thing such work should do, not the
last**: doing it afterwards means discovering the problem from a desync rather than from a
table. Expect it to move the golden set, since it changes how the solver rounds.

### TAA makes the frame a function of the last several

Which is why it is off by default. It converges to a bit-exact period-8 cycle, so a golden
image at a fixed frame still works — but a golden image taken at an arbitrary frame does
not.

### The realtime clock makes frame N no longer a function of N

`--realtime` and `--locked` select it, it is the **default**, and the engine says so at
startup:
golden images and per-pass measurements taken that way are not comparable with locked
ones. Anything that needs frame 60 to be the same frame 60 passes `--locked` and does not
inherit the value — `scripts/golden.sh` and `substrate bench` both do.

The default used to be `locked`, which made every capture reproducible and made the engine
simulate at the *frame rate*: several hundred FPS with vsync off is roughly ten times real
time. Determinism is worth a flag on two tools; it is not worth a demo that runs at 10x.

---

## Physics

### ~~Kinematic bodies exist and nothing drives one~~ — the demo drives one

**Closed by G9**, which is the row that was always going to close it: "a lift, a door, a
platform on a spline" is a game's decision and the demo is the game. `game/demo/DemoWorld.cpp`
builds a `ColliderMotion::Kinematic` slab and slides it on the fixed step, and what that
found is worth keeping — **the verb is not `setBodyTransform`.** The scene sweep already
pushes a node's world transform into any kinematic body attached to it, so a game that
called the physics verb directly would have the node overwrite it on the same frame.
Moving the *node* moves the body and the mesh riding on it.

### ~~Contacts are drawn and never delivered~~ — delivered since G7

`PhysicsWorld::contacts()` is the last step's manifolds, with a point, a normal and a
closing speed read before the solver resolved them. The paragraph that stood here said
there was no `ContactListener`; there has been one since G7, and the drift was found by G9
writing against it. `--physics-contacts` still draws them.

### A body is pushed and placed; it is not spun, and no force is continuous

P7 added `addImpulse`, `setLinearVelocity`, `linearVelocity` and a dynamic body's
`setBodyTransform`. What it deliberately did not add, and what each one waits on:

| Not there | Trigger |
|---|---|
| `addImpulse(id, impulse, point)` — an off-centre push, which imparts spin | A caller that wants a hit to make something tumble. Jolt already has the overload; it is one line and a doc comment |
| `addAngularImpulse`, `setAngularVelocity`, `angularVelocity` | The same caller, from the other side. Adding the pair speculatively would double a surface with no consumer |
| `addForce` / `addTorque` — accumulated across a step rather than applied at an instant | A caller integrating something continuous: thrust, wind, a magnet. An impulse is `force * step` and a game with a fixed step can spell it, which is why the narrower verb landed first |
| A `freedom` other than `All` and `Plane2D` | `EAllowedDOFs` is six bits and the enum exposes two combinations. A third named plane would be a second convention for gravity, the orthographic camera and the sprite layer to disagree with; a game that wants one rotates its world. **Re-examined by D18 and kept**: Jolt's enum is a bitmask and an XZ plane is as expressible as an XY one, so the refusal was never Jolt's constraint — it is this engine's, and the row it collided with was navigation's hardcoded +Y rather than this one. `NavBuildParams::up` is where that moved |
| A body driving a sprite | A `BodyId` and a `SpriteId` are two handles into two dense tables and binding them is a game's loop today. Trigger: the third game that writes it — or P5/P6 needing it, at which point it is an `Attachments` field on a scene node rather than a third table |

### Walking and rendering at once has no automated cover

A headless golden run presses nothing, so the physics case pins that the character stays
where it was placed rather than sinking, drifting or jittering. The unit suite covers
walking, stopping and jumping. The combination is untested.

---

## Navigation

### Clearance and overhangs are not modelled, and `agentHeight` is not a parameter

`scene::NavMesh` is a triangle navmesh: the scene's static mesh colliders, filtered to what
an agent could stand on, welded, searched with A\* and pulled straight with the funnel. A
voxel field would know that a ceiling is 1.2 m above a floor and refuse to walk a 1.8 m agent
under it; this has no idea, and accepting an `agentHeight` would be a promise it could not
keep. **Trigger for the voxel row: a game whose levels have headroom that matters** — a crawl
space, a low arch, a mezzanine an agent must not path under.

Radius erosion has the same shape. `NavBuildParams::agentRadius` insets the funnel's portals
rather than shrinking the walkable region, so it keeps a path off the walls it passes
*through* a portal and does nothing about a wall it merely passes *beside*.

### A prop is cut out of the floor only if its collider stands on it

`bake` cuts the walkable surface where geometry too steep to walk reaches above it and
touches or crosses it — which is what makes a column an obstacle to a route rather than a
decoration on top of one. Three things follow, and the first is the one that bites:

- **A prop with no collider is not an obstacle.** The bake reads static *mesh* colliders and
  nothing else, so a pillar the artist modelled and nobody authored collision for is invisible
  to routing exactly as it is invisible to the solver.
- **A prop that floats does not cut.** Standing on is contact: a crate hovering a centimetre
  above the floor cuts nothing, and so does one whose collider is a box or a capsule rather
  than a mesh — those carry no triangles and the bake has never taken them.
- **A cut costs bake time and triangles.** `game/battle_arena`'s arena is 3508 collider
  triangles and bakes 3632 navmesh triangles in 16.2 ms, of which the cut is 12.6. Sponza
  authors no colliders and bakes no navmesh, so nothing in the golden set pays it.

The hole itself is exact: the pieces are convex and split by arithmetic, so there is no cell
size to tune and no rasterisation error.

---

## Audio

### ~~There is no one-shot API~~ — `playAt` since G7

`AudioEngine::playAt(desc, position)` forces `loop` off and `autoplay` on and lets
`update()` retire the voice; everything else about the desc is read exactly as `create`
reads it. The trigger this row named — *nothing in the engine yet fires an event for it to
serve* — was the contact stream, and it fired. The demo plays one per impact above 1 m/s
with the volume scaled by the closing speed.

### Nothing models a space

A low-pass on a blocked line is occlusion. Reverb, early reflections and an acoustic
material per surface are a different feature. The line is drawn at
`PhysicsWorld::segmentBlocked` returning a **boolean** rather than a surface, which is the
narrowest place to draw it.

### ~~Both repository audio assets are ambience beds~~ — two generated assets take the other side

Neither is downloaded. `audio/impact.wav` is synthesised and committed;
`audio/fire_crackle.wav` is cut from the recording beside it by
`substrate fetch-assets`, so it stays out of the repository along with its
source. `audio/impact.wav` is a quarter-second thud (G7) and `audio/fire_crackle.wav` is
a four-second seamless loop (G9); between them the *decode* side of S5.2's crossover is
taken by a one-shot and by a looping source without an author writing `"load": "decode"`
anywhere. The demo's four braziers play the crackle, which is also the first demonstration
that four sources on one file decode it once.

---

## Assets and loading

### fastgltf rejects any file whose *node* carries morph weights

Not ignores — rejects, before a mesh is read, with "missing something or has invalid
data". `external/fastgltf/src/fastgltf.cpp` inverts the test on the parse result, so the *success* case
returns `InvalidGltf`. Upstream main carries the same code as of the pinned `v0.9.0-27`.

Mesh weights parse fine, so the loader's node-override branch is written, correct and
**unreachable** until the submodule moves. Recorded because the symptom — a file that
morphs in every other viewer and fails here before anything loads — points at the wrong
half of the system.

### A Mixamo export has no tangents, and `normalize(vec3(0))` is a NaN

The loader generates a perpendicular fallback — arbitrary rather than reconstructed, which
is right for a mesh with no normal map and **stated as wrong for one that has both**.

### BC7 is lossy

The lit frame differs from the uncompressed render by a mean of 1.88/255. The golden set
encodes the compressed result.

### Framing a lone character needs a scene, not a special case

`Camera::frameBounds` aims down the longest horizontal axis from a quarter of its length,
which is right for an interior and puts the camera inside the hip of a lone 1.8 m figure.
`scripts/make_composite_scene.py` grafts a floor, a backdrop and a control block onto a
file this repository cannot ship. **A test asset that needs an environment is not a
workaround for the framing heuristic; it is what a character scene is.**

---

## The game API

Everything in this section was found by **G9** and **G12**, which are a game built entirely
out of the engine's public calls and then driven through it, and are the only things that
could have found any of it: the golden set and the unit suite prove that nothing broke, and
neither can say whether the surface is any good to write against.

### ~~A game cannot make a second copy of a mesh it made~~ — closed by G14

`Engine::addInstance(model, material, transform, motion)` is the verb. The seven fields a
game used to copy out of the `Primitive` by hand are copied by the engine, which knows them.

**The half that mattered turned out to be the second half**, and the entry above named it
without weighting it: `InstanceTable::create` cannot tell the renderer. A slot past the
renderer's instance capacity is a `memcpy` past the end of a mapped staging range, and a new
slot is invisible to `staticTierStale` — which walks the slots the acceleration structure
*baked* — so an instance a game created is in every raster pass and in no ray. The private
helper this replaced did neither and worked on headroom.

`Renderer::instancesGrew()` is what `addInstance` calls, and it exists rather than reusing
`setInstances` because the correct-looking answer is a five-fold regression: `setInstances`
rebuilds the acceleration structure on the spot, `addInstance` is called in a loop, and
sixteen rebuilds instead of five took the demo's `Game::init` from 64 ms to 317. The rebuild
is deferred to the next `rebuildAccelIfStale`, which runs once a frame — so a batch of any
size costs one.

*What remains* is that five rebuilds is still five: `Engine::createMesh` triggers one per
call, and that is
[its own card](../kanban/backlog/chore-the-acceleration-structure-is-rebuilt-once-per-mesh-a-game-adds.md).

### `GameSetup::decals` cannot be used by a game whose scene can be overridden

`configure` runs before the scene is loaded — that is what it is for — so a game filling
`setup.decals` is placing marks in whatever scene the command line eventually names. For
the demo that is eleven golden cases. `gfx::decalAt` from `init` is the door C3 built and
is the one a game should use; `GameSetup::decals` has been an empty vector since it was
added and this is why.

A decal's `textureIndex` is also a slot in the **scene's** bindless array, which only a
glTF writes to — `e.images()` is the overlay's array and a different thing — so a game
authoring content in code cannot supply decal art at all. The demo tints texture 0 nearly
black.

### A punctual light cannot decline its shadow

`updateLights` assigns the 24-layer atlas **first-fit in light order**: a point takes six
layers, a spot takes one, and a light that does not fit illuminates without occluding and
logs an error. There is no `castsShadows` — the flag and the `substrate_light` extra that
carried it both went with the shadow rewrite. So the only levers a game has over which of
its lights shadow are the *type* and the *order* it pushes them in, and a scene wanting
five point lights has no way to say which four matter.

The demo's four braziers are spot lights partly for this reason: four points would have
needed 24 layers on top of the 18 the demo's own set already spends.

### ~~`Engine::addModel` brings geometry, colliders and a rig — not emitters, sounds or lights~~ — all of it since C21

An appended model contributes vertices, indices, materials, textures, render instances,
colliders, its rig (C22, merged into `SceneAnimator` with its skins renumbered) and its
lights, audio sources and emitters. C41 made that the only route into the world: there is no
`GameSetup::scene`, so anything the entry above said had to be in the file the game named
is now in a file the game imports.

The binding walk is still written twice — `initPhysics` for a `--scene` document and
`Engine::addModel` for an import, with the same arithmetic in both. That is the Rule of
Threes taken at its word, and it is
[its own card](../kanban/backlog/chore-one-collider-walk-instead-of-two.md).

### A game still has to ask whether its scene was overridden

`--scene` loads a document into the world before `Game::init`, and `Engine::sceneOverridden()`
is how a game asks whether that happened. `game/demo` gates its whole world on it, because
`scripts/golden.sh` runs thirteen cases through whichever binary the build directory holds and
Sponza dropped into `mirror.gltf` moves every baseline.

It should not exist. A game does not want its world replaceable from a command line, and
`game/battle_arena` no longer checks — it composes its arena unconditionally. What keeps the
question alive is the harness, not the API, and the fix is a viewer that the golden suite runs
instead of a game:
[its own card](../kanban/backlog/chore-the-golden-suite-runs-a-viewer-not-whichever-game-the-build-holds.md).

### Nothing inside a loaded model can be addressed by name

`GltfScene` keeps placements and primitives; the node *names* the file carried do not
survive the flatten, and Sponza is one unnamed node holding one mesh regardless. So "put a
brazier in each of the four hanging bowls" is not expressible — a game placing content into
a file it did not author derives positions from `boundsMin`/`boundsMax`, and a bounding box
does not know where the room is. The demo's braziers are fractions of the extent, tuned by
looking at the result.

### Resizing the particle pool is two calls, and the second is easy to miss

`ParticleSystem::create` shares the capacity `setEmitters` derived, and `setEmitters` is
the only thing that resizes it — but the renderer sizes its GPU buffers in
`Renderer::setParticles`. A game adding emitters after load calls both, in that order, or
emits into a pool the device has no storage for. `Engine::loadScene` makes the same pair.

### A character has no ground state until it has been swept, and `fixedUpdate` runs first

`characterOnGround` reads Jolt's own `GetGroundState`, which for a `CharacterVirtual` that
has never been stepped is *in the air*. `Game::fixedUpdate` runs ahead of `simulate`, so a
game driving anything off that predicate is told its character is falling on step zero — and
a locomotion machine believes it. G12's demo skips the step; the general shape is that the
first `fixedUpdate` sees a physics world that has not run yet, and nothing at either call
site says so.

C20 had to answer the same fact from inside: a new character starts with its coyote window
already spent, because "has never been swept" and "just walked off a ledge" are the same
ground state and only one of them should be able to jump.

### A game cannot ask a character how fast it can go

`ColliderDesc::moveSpeed` goes in and there is no accessor to read it back, so a game
normalising `characterSpeed` into the 0..1 a state machine's thresholds are written against
has to hard-code the number the *scene file* authored. The demo divides by 4.0 for a
character the glTF gave 3.2 m/s, which works and is a coincidence nobody wrote down until
now. Trigger: a second character with a different top speed, at which point the divisor has
to come from the character rather than from the source.

### An action's magnitude is a property of the binding and is documented nowhere

`InputMap::value` returns how far an axis was pushed and 1.0 for a key, and
`setCharacterInput` multiplies the vector it is given by `moveSpeed` **without normalising**
— so a half-length request is half the speed. Both halves are correct and neither is stated
at its call site, which is how the demo shipped for four rows with a `walk` state that could
not be reached: `moveDirection` normalised, every request was full travel, and the band
between the walk and run thresholds was empty. Written down here because the *combination*
is the surface, and neither declaration is wrong on its own.

### An input edge has as many latches as it has consumers

`setCharacterInput` latches the jump and the solver consumes it, which is what stops a tap
between two steps being lost. An animation trigger is a second consumer with a lifetime of
its own, so a game driving both keeps its own flag and clears it in the step that used it.
There is no shared "this frame's edges" a subsystem can subscribe to, and there should not
be — but a game with three consumers of one press will write the same flag three times.

**C20 removed the jump's copy of that flag and did not remove the general problem.** The
demo's second latch was not merely bookkeeping: it fired the animation on `flag &&
characterOnGround(...)`, which is the controller's own decision re-derived from outside, and
it stopped being right the moment a coyote window and a buffer got between the press and the
launch. `characterJumped` reports what the solver did instead. Every *other* press in a game
still needs its own latch, and the shape to copy is that one: where a subsystem consumed an
edge, ask that subsystem what it did rather than reconstructing it.

### Pads are separated, but nothing assigns them to players

C26 gave every joystick slot its own state and made an action resolve against a
`PlayerDevices` rather than against a merged pad, so two players are expressible. **Who
holds which pad is still entirely the game's to decide, and there is no join flow.** A game
that wants "press A to join" polls `padCount`/`gamepadConnected(pad)` itself, calls
`setPlayerCount` and `setPlayerDevices`, and owns the result; the engine offers no lobby, no
assignment UI and no persistence of an assignment across runs.

Two consequences follow, and neither is a bug:

- **Hot-plug does not reassign anything.** GLFW renumbers nothing when a pad is unplugged,
  so a player's bitmask keeps naming a slot that has gone quiet and the actions read zero.
  That is the correct behaviour for a paused-and-reconnected controller and the wrong one
  for a controller that came back in a different slot, and the engine cannot tell those
  apart — only the game knows whether the same person picked it up.
- **The default is one player holding everything.** `PlayerDevices{true, kAllPads}` is the
  single-player case, and it is deliberately the shape that makes a game which never
  mentions players behave as it did before. A game that calls `setPlayerCount(2)` and then
  forgets `setPlayerDevices` gets a second player holding nothing, which reads as a dead
  controller rather than as an error.

The rebind round trip is per binding and not per player: the binding table is shared, so
rebinding `Jump` moves it for everyone. Split bindings would be a second table, and nothing
has asked for one.

### A headless loop steps a world it did not build

C27 made the simulation device-free by extracting `scene::Simulation` — the ten subsystems a
step moves, and the one written copy of the order it moves them in. `Engine` holds one and
`Engine::simulate` delegates to it in a line, so `substrate-sim` and the drawn engine advance
through the same code. **What did not come with it is the world build.**

`Engine::initPhysics` walks the collider table and, while it walks, creates a scene node per
driven body, binds each audio source to the body it rides, pairs rigs to controllers and
builds the cloth. All of that lives in `engine/Engine.cpp`, which is not hosted — so
`tools/sim.cpp` has a dozen lines of its own that create bodies and characters and nothing
else. A scene whose behaviour depends on a sound following a body, or on a rig driving a
controller, will not reproduce under `substrate-sim`.

That is a real divergence and it is the next thing to fix if anyone leans on this: the
answer is `Simulation::build(const SceneData&)` with `initPhysics` calling it, not a second
walk kept in step by inspection.

**There is also no `Game` in a headless run, and there cannot be one today.** `Game::init`,
`frameUpdate` and `fixedUpdate` all take an `Engine&`, and `Engine.h` reaches the renderer —
so a game class cannot compile into a hosted target at all. The card that opened this expected
a scaffolded game to build as a device-free target; it cannot, and saying so is more useful
than a `Game` split nobody has asked for. A server that wants game logic drives `Simulation`
itself, which is what `tools/sim.cpp` demonstrates.

The camera, the input map, the scene loader, the spatial index and the instance table stayed
in `Engine`. None is moved by a step. A `Simulation` that owned the camera would be inviting a
headless loop to decide what is on screen.

### The engine does not know which character is a player, and will not be told

G17 retired `playerCharacter()`, `playerNode()` and `setPlayerCharacter` rather than making
them plural. `Engine::authoredCharacters()` reports what the collider walk found and stops
there; a game holds its own players. **This is a refusal, not a gap** — the engine read its own
player slot nowhere, so a plural version would have been a table the engine maintained for a
reader it does not have.

Three things follow that a game has to do for itself, and none of them is on an arc:

- **Nothing pairs a rig to a player.** `Engine::locomotion().pair(character, rig)` is the call
  and the game makes it; a character a file authored arrives with a collider and no skin, so
  its `rig` is invalid until something says otherwise.
- **A character a game made is not in `authoredCharacters()`**, and a streamed scene adds
  nothing to it — `applyPendingScene` does not re-walk colliders, the same C10 limitation that
  leaves it the rig the first scene brought.
- **The handles do not outlive their model.** Nothing clears a game's player list when the
  model its character came from is removed, because nothing in the engine knows the list
  exists.

`scene::Camera` is still driven by input player 0 and there is still one **presenting** camera,
which is the one place "player" survives in the engine — and it survives as a default argument
rather than as a stored identity.

**The engine now renders more than one view**, which is the half of this that was a limitation
and is not any more: `e.views().create(e.images())` returns a handle to a view with its own
camera, and `views().image(id)` is what it drew, as an ordinary `ImageId` a sprite, a UI quad
or a material can sample. What that does *not* yet give a game is split-screen, because the
second view lands in an image rather than in half the window — a game composites it itself, at
whatever size and place it likes, which is the same answer a mirror and a minimap want. Three
things it shares with the presenting view rather than owning, each with its own trigger, are in
[rendering.md](rendering.md#more-than-one-view): the target extent, the light ranking and the
shadow atlas.

### A character's tuning is fixed at `createCharacter`

`ColliderDesc`'s eleven character rows are copied into the slot and there is no setter for any
of them, so a power-up that lengthens the coyote window, a hardcore mode that removes it, or a
surface that changes acceleration are all out of reach — the only way to change one is to
destroy the character and make another. Nothing needed it yet, which is why there is no
`setCharacterTuning` taking a struct nobody has asked for.

It has one visible cost today. **The negative arms for both timing windows are in
`tests/PhysicsTests.cpp` rather than in `scripts/locomotion.sh`**, because "the same scripted
run with the window set to zero" is not something a command line can ask for. The scripted
pair moves the *press* outside the window instead, which is a stronger statement about the
window's size and a weaker one about the feature's existence, so the hosted arms carry the
other half. Trigger: a second thing that wants to change a character mid-run.

### Picking is a physics query, so it misses anything with no collider

`Engine::cursorRay` plus `PhysicsWorld::raycast` reaches exactly what a scene authored
colliders for. A prop the document did not mark cannot be picked, however plainly it is on
screen. The alternative is reading the depth buffer back, which needs a fence, a frame of
latency and a staging copy per query -- and would answer with a *point* rather than with the
body a game then wants to do something to.

### The scene's fallback texture was never written, for as long as it existed

`GltfScene::buildDescriptors` created the 1x1 image its own comment calls "opaque white" and
never uploaded a texel, so it sat in `UNDEFINED` -- transparent black on this driver. Nothing
saw it: `sampleOr` short-circuits on a negative index and never touches the image, and the
only other reader is a material whose image *failed to decode*, which no scene in this tree
has. A decal naming `fallbackTextureSlot()` discarded every fragment, which is how it was
found. Fixed rather than documented; the entry stays as the record of a promise that was
untested for the whole time it was made.

### A blended surface casts no shadow and is absent from traced reflections

`AccelStruct::build` skips `kInstanceBlended`, so a translucent surface is not in the TLAS at
all: it occludes no ray and no reflection ray finds it. That matches the raster path, which
has always skipped blended instances when filling the sun's cascade — the two disagreeing is
what this closes. A 10 m intersection-highlight sphere around a character rendered as an
opaque black disc on the floor under ray queries and as nothing at all with `--no-ray-query`;
the same sphere cast no shadow-map shadow either way.

The refusal it carries is real: a game that wants stained glass to tint the floor, or a
window frame to be visible in a mirror, cannot get either. The alternative is a TLAS instance
with `VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE` and an any-hit shader that reads alpha, which is
a shader binding table and a second pipeline type — refused for the same reason the traced
shadows are `rayQuery` and not `vkCmdTraceRays`.

**Nothing in the golden suite covers it.** Sponza reports `0 blended` instances and so does
the demo, so the 13 cases were byte-identical across this change because none of them has a
blended surface to leave out, not because leaving it out preserves them.

### Creating a morphed mesh rebuilds every acceleration structure in the scene

`Renderer::setSkinCharacters` reads the delta array, the weight total and one output range per
deformed instance all as they stand when it is called, so `Engine::createMesh` calls it
again for a mesh that deforms — which tears down and rebuilds the deformed vertex buffer,
the delta buffer, both BLAS tiers and the TLAS. That is on top of the device drain and the
pipeline rebuild every `createMesh` already costs. One banner is invisible; a game building
twenty morphed props at load pays twenty. Trigger: a stated count, or a game that creates
one during play rather than in `init`. Found by **G11**.

### A morphed mesh made in code is never frustum-culled, and its bounds are advisory

A deformed instance gets an infinite culling box, because the bind-pose box is wrong the
moment the animation leaves it and a box wrong in the small direction makes an object
vanish. So `MeshData::localMin`/`localMax` on a morphed mesh reach the spatial index and
the inspector and never a draw decision: the object is drawn from every view every frame.
Correct and cheap for one banner; the caller who makes it matter is a scene with many.

### Morph deltas are never reclaimed, like material slots

`unloadModel` gives back vertex and index ranges and returns texture slots to the free
list. It does not shrink `morphData`, and cannot: an instance carries `morphOffset`, so
reclaiming a run out of the middle would renumber every later primitive under instances
that still index them — the same argument placements and material slots already make. A
retired character likewise keeps its weight block. So a long-lived process cycling morphed
meshes grows monotonically in three arrays at once. Found by **G11**.

### A streamed scene keeps the rig the first one brought

`Engine::applyPendingScene` rebuilds the instance table, the scene buffers and the spatial
index, and does **not** re-init `SceneAnimator`. So a background-loaded scene animates
against the previous scene's clips and skins, and any character a game made for the old
scene's geometry would go on being uploaded every frame — `applyPendingScene` retires the
ones it knows about for that reason. Nothing in C10's row noticed because nothing had
streamed a scene with a rig in it. Found by **G11**.

---

## Interface

### Panels cannot be dragged or resized

Fifteen lines of `hot`/`active` on a title bar. Absent because nothing had two panels — and
the inspector is now the second, placed relative to the first and skipped when there is no
room for it. The position a drag would persist is the first retained state this design
does not already have.

### Two widgets with the same caption in one container are one widget

The escape hatch is a `##` suffix rather than an id stack. The stack is the next rung and
nothing has needed it.

### No combo box, tree, colour picker, tabs or text selection

The last of those belongs in `input::TextInput`, which has a cursor and not a range,
rather than being faked in the drawing.

### The theme is a struct and is not in the config file

A colour scheme nobody has asked to change is a config section nobody will read.

### Only integer UI scaling

The embedded font is a bitmap sampled NEAREST, so integer magnification is exact and a
fractional one would be blurry. A blurry bitmap font is worse than a small one; a TTF via
`render.debugFont` re-bakes at any size.

### The inspector edits position only

Rotation and scale are read-only. Making them editable means owning a TRS decomposition
per selected object, seeded on selection and written back whole — a real design with a
real answer, and not one anything has asked for. See
[systems.md](systems.md#the-inspector) for why the current write is exact.

### A control released and re-pressed inside one frame reports no new edge

Stated in the code. The press edge carries a flag for exactly this reason; within a single
frame it cannot help.

---

## Tooling

### Synchronization validation over-reports on older SDKs

Sync validation gained SPIR-V access classification in **SDK 1.3.239** — not "after 1.3.204",
which is what this said before somebody bisected the tags. At `v1.3.238`,
`GetSyncStageAccessIndexsByDescriptorSet` still reads `// TODO: sampled_read` and returns
`storage_read` for every non-writable, non-uniform-buffer descriptor; `v1.3.239` has the
`SAMPLED_IMAGE || COMBINED_IMAGE_SAMPLER || UNIFORM_TEXEL_BUFFER → sampled_read` branch. It is an
unimplemented feature, not a bug. **The layers installed here are 1.3.204.1**, dated 2022-04-07,
against a 1.3.280 loader — so nothing blocks a newer layer but the distribution package.

The consequence is that every `sampler2D` fetch is reported as `SHADER_STORAGE_READ` against
barriers that correctly grant `SHADER_SAMPLED_READ`. It is **18 reports per frame** in a default
capture, 21 with `--rt-shadow-mask`, 22 with that and `render.ssrScale=0.5` — the count is
per-frame constant and scales linearly with `--frames`, so quote it per frame. Across
`lighting_rt.frag`, `ssr_rt.comp`, `tonemap.frag`, `composite.frag`, `depth_pyramid.comp`,
`ssao.comp`, `ssao_blur.comp` and `bloom_threshold.comp`.

~~**Do not silence them by widening the barriers.** On this SDK the baseline to compare against
is 1140.~~ **That advice concealed two real hazards for an unknown number of cards, and this is
the entry's most useful line.** Because the layer calls every sampled read a storage read, a
genuine missing-stage hazard is *textually indistinguishable* from a false one, and a
count-based baseline hides it by construction. Both real ones were destination-scope bugs:
`recordGbufferRead` named `FRAGMENT_SHADER` alone while `recordSsr` binds the same
`view.gbufferSet` to a **compute** dispatch, and `recordBloom` transitioned `hdrTarget` with
`dstStage = COMPUTE_SHADER` only while `recordTonemap` samples it in **fragment** — its comment
saying "bloom already moved it", which is true of the layout and false of the stage scope.
`recordTaa` had named `FRAGMENT | COMPUTE` on the analogous transition all along.

**The rule that replaces the count**, and it is mechanical: read the stage out of the message's
`usage:` field and check whether `write_barriers:` contains `SYNC_<that stage>_SHADER_SAMPLED_READ`.
If it does, the barrier already grants the access that actually occurs and the report is
misattribution. If it does not, the reading stage was never made visible and the hazard is real.
That two-line filter is what found both.

One report is neither: `lighting_rt.frag`'s shadow mask at set 4 binding 0 is a real
`STORAGE_IMAGE`, and the layer describes it accurately in every field except reachability — the
`OpImageRead` sits under `OpBranchConditional` on `SpecId 7`, false on that path, and the layer
analyses unspecialized SPIR-V. Its barrier grants the declared read anyway, the way
`recordSsao`'s disabled path does.

### Swapchain resize ordering is guaranteed by construction, not by a diagnostic

A pending resize is consumed *before* the acquire, so the frame is abandoned only at a
point where the acquire either has not happened or has failed — and a failed acquire
signals nothing. That is what keeps `handleResize()` from destroying a semaphore with a
signal outstanding, since `vkDeviceWaitIdle` waits on queue work and the presentation
engine is not a queue.

The path is soak-tested: 200 frames across 95 swapchain recreates with validation on, zero
errors, plus a clean ASan run. **No validation layer checks this ordering**, though —
neither standard nor synchronization validation distinguishes the correct order from the
incorrect one. The guarantee rests on the specification argument, and a change to that
ordering will not be caught by any tool in this repository.

### `GBuffer` and `Bloom` are bimodal run to run

About 5% apart, per run rather than per frame. Quote `Lighting` and `Frame`. See
[tooling.md](tooling.md#the-bimodal-zones).

### The MSAA sweep in the profiling document predates Tier 1

Re-run it before quoting it in a decision.

### ASan needs `--no-ray-query`; TSan cannot run the renderer at all

Both are the proprietary NVIDIA driver failing inside `vkCreateDevice`. See
[tooling.md](tooling.md#what-each-configuration-can-and-cannot-do).

### Recording needs ffmpeg, and ends at a resize

`--record` shells out to ffmpeg — two encoders and a muxer — rather than linking libav.
Reaching for the tool that solves the problem is the right trade for a debugging aid, but
it does mean the feature is absent on a machine without it. `run.sh` refuses before
building; the engine refuses before opening a window.

A resize **ends the recording** rather than adapting to it. rawvideo carries no
dimensions, so a differently shaped frame would not fail — it would silently shear the
picture from that point on, and a recording that quietly becomes wrong is worse than one
that stops. What was captured up to the resize is still written.

That is no longer the end of the session, though, and it stopped being so with G8:
`Engine::startRecording` and `Engine::stopRecording` are public, so a game that binds a key
to them can simply start another. Before, a `record.enabled` key at startup was the only thing
that could put the three pieces together — the `Recorder`, the renderer's frame tee and the
audio tap — and a resize was therefore terminal for the run.

### Only the pacing arithmetic of the recorder is unit-tested

`framesOwedAt` is a free function precisely so the drift rule can be tested without an
encoder behind it — a recording whose clock slides is not a recording that *fails*, so
that is the half worth pinning. The queue, the pipes and the mux need ffmpeg and a
swapchain, and are covered by running it.

The drop path has no automated cover, but it is reached by hand on the **ASan** build,
which is slow enough to fill the four-frame queue: a 1.889 s run that refused 10 frames
still produced a 1.867 s file — under one frame short, where a broken carry-forward would
have been 19% short. Provoking that on a release build would mean a knob that exists only
for the test.

### The texture free list's release path is not unit-tested

It writes a descriptor and destroys an image, so the interesting half needs a device.
Extracting a slot allocator to reach the other half would be the Rule of Threes broken at
two. There is a Debug self-check at load instead.

---

## Deliberately not built

Each of these was considered, sized, and declined for a stated reason rather than
forgotten.

| Thing | Why not |
|---|---|
| **Terrain and water** | Content systems with their own authoring, LOD and simulation. No target scene needs them, and building either against an interior stone arcade would be shaped entirely by guesses |
| **Networking** | The gate is a target, and none exists. Transport is the easy row; "what is authoritative, what is derived, what goes on the wire" is the design problem, and every answer is a property of a game. See the determinism table above for the one engine-side prerequisite that fails today |
| **A game module boundary as a loadable `.so`** | **The in-tree boundary is built** — `engine/` is a library, `game/<name>/` is an executable, and `scripts/build.sh` produces nothing runnable so a leak across the line is a link error. What is still declined is a *dynamically loaded* one: the only thing that justifies a frozen C ABI is a developer who cannot recompile the engine, and that has not happened. See [principles.md](principles.md#engine-and-game-separation) |
| **A second in-tree game** | `game/` holds one. A boundary with one consumer is a guess rather than a boundary, which is why a scaffolded second is a row in the G arc rather than a claim here |
| **A material / pipeline registry** | Seven named `VkPipeline` members. A registry earns its place when pipelines are created from *data*; the trigger is a variant selected per **draw**, not per keypress |
| **A shader variant cache** | Comparing a feature key each frame and rebuilding is stricter than a map — one live variant per pipeline. Enumerating a cross-product of nine constants is 512 pipelines |
| **A render-target manager** | Named members. Revisit past ~15 targets |
| **entt** | It cannot own the instance table — paged storage is not flat, swap-and-pop is not slot-stable. It can sit beside it whenever gameplay needs it |
| **An octree** | See above — the renderer still has none, and `scene::SpatialIndex` is the subsystem-side answer the delegation named |
| **`gfx::ImageTable` growing into an image cache** | It is a `std::vector` of entries, a free list and a generation counter, and it is close enough to the `ResourceManager` [principles.md](principles.md) refuses by name that the four things it must not acquire are worth stating: **reference counting**, which replaces an explicit `destroy` with an implicit one; **a path-keyed cache**, so loading the same file twice loads it twice — deduplication is a cache, a cache needs invalidation, and invalidation is the system this is not; **a virtual `IImageLoader`**, where there is one `stbi_load` in the device half; and **eviction, residency policy or an async queue**, which is streaming and is C10. A change that adds any of the four has made it the refused thing and owes a new argument |
| **A blend tree** | Interrupting a fade drops the outgoing clip; the alternative is an unbounded stack of playbacks |
| **A particle manager, effect graph or emitter hierarchy** | An emitter is authoring data placed by a node walk. A second blend pipeline would also have cost the single global sort |
| **A physics component, body registry or collision-event bus** | A collider drives an instance through a flat list built once at startup |
| **A `Sound` / `SoundInstance` class, emitter component or effect chain** | A filter is a node miniaudio owns; a bus is a `ma_sound_group`. The only thing written here is what a glTF file may say and what should happen to it |
| **A widget base class, retained tree, event bus or layout solver** | See [systems.md](systems.md#user-interface) |
| **A `Renderer2D`, `SpriteBatch` or second render path** | The trigger is unchanged from the 3D case and is a second graphics **backend**. A sprite pass is a method recording `vkCmd*` inline like every other pass. See the P arc on [the board](../kanban/) |
| **A second physics library for 2D** | Jolt has `EAllowedDOFs`; confining a body to a plane is one field on `ColliderDesc`. Box2D beside Jolt would be two solvers to keep deterministic for a feature one of them already has |
| **Skeletal 2D (Spine, DragonBones)** | A solver plus an importer, on the far side of the line C7 moved by exactly one item — an event track is data already in the file. Trigger: an authored asset in that form |
| **GPU sprite sorting** | The particle system's bitonic sort would be the *second* occurrence, not the third. Trigger: a measurement showing a CPU sort outside the frame budget at a stated sprite count |
| **Non-integer presentation scale** | Declined *because* of the pixel-exact guarantee, which a fractional scale cannot satisfy by definition. A game wanting one renders at the window's own extent, which is the same code path with the scale at 1. **P2 built the path and the refusal is now a floor rather than an intention**: `presentLayout` divides down and pays the remainder in bars, and a window too small for even 1x crops the middle rather than shrinking |
| **Unifying the scene's texture array with `gfx::ImageTable`** | Two bindless image arrays exist and neither is wrong for what it holds. P1 gave the overlay's a free list, a generation and a growable descriptor set; it deliberately left the scene's alone, because `acquireTextureSlot` still has no caller outside `GltfScene.cpp` and merging them would be promotion with no caller asking for it. Trigger: a caller outside that file — at which point the generation that free list lacks stops being latent. **C10 moved that trigger closer without reaching it**: `unloadModel` now releases slots at runtime rather than only at load, so the release path is reachable from a keypress, and the free list it uses is still generation-less |
| **A capability registry, a `DebugCommand` table or an engine-owned debug menu** | G8 asked what a game still could not reach and answered it with three ordinary methods — `Renderer::cycleDebugView`, `InputMap::setDefaultBindings`, `Engine::startRecording`/`stopRecording` — rather than a table of named commands the engine could offer up to a UI. **The engine still binds no keys and draws no panel of its own**; a registry is what a debug menu needs, and a debug menu is the thing that would give the keyboard a second owner. Trigger: nothing on the current arcs. A game that wants a command palette builds it from the actions it already declares, which is where the names live |
| **A property registry for the inspector** | A function that names properties is smaller than a schema plus a type-erased setter plus a name-to-offset map. **The settings table is not this and does not overturn it** — see below. **G6 added the second inspected thing and it is a second function, as predicted**; the trigger is the third |
| **A ray-tracing pipeline** | **Still declined, and shading a ray hit did not change it.** `shadeRayHit` is a function called from compute shaders that were already running; a pipeline means new stages, a shader binding table, `vkCmdTraceRays` and a second pipeline type threaded through pipeline creation. Inline ray query bought the whole feature without any of that — the one thing it costs is recursion, hence single-bounce above |
| **Reflection-generated descriptor layouts** | Generating them would remove the mismatch by removing the ability to read what is bound |

### The settings table, and why it is not the registry that was refused

`engine/core/Settings.h` is a generated table of named, typed settings with a string door.
It looks enough like the property registry the inspector refused that the difference is
worth stating rather than left to be inferred, because the refusal above still stands.

The refusal is about **inspecting objects**, and its argument is a cost comparison with one
consumer: a schema plus a type-erased setter plus a name-to-offset map is three
abstractions bought to save writing `ui.slider("X", p.x, ...)`. It comes with a stated
trigger — *"the third thing worth inspecting"* — and **two** things are inspected today: an
instance, and, since G6, a scene node.

**Two is the coincidence the refusal predicted, not the trigger.** The prediction was that
the second one would be a second function, and `drawNodeInspector` is exactly that. What
the two actually share is a caption cache keyed on a revision and a selection index that
clamps — four lines each, and neither is a schema. What they do not share is every property
either of them names: an instance is a flat row of a table and a node is a tree, so the two
listings are built by different walks and no field is common to both. **The trigger is still
the third thing, and it is now one away.**

Settings are a different problem with a different consumer set:

| | Instance inspection | Settings |
|---|---|---|
| Consumers | One panel | JSON parse, the command line, a game, a dump, a panel |
| What a table saves | `ui.slider` calls | 34 assignments **and** the drift between two copies of a value |
| Persistence | None — an instance is not serialised by name | The name **is** the JSON key; it already exists as a string |
| Failure mode without it | More code | A config key that parses and silently does nothing |

The names were already there — in `substrate.json`, in the parser, in a flag, in a panel —
written by hand in four places. The table makes that one place, and `ui::drawSettings` is
the panel that used to be the fifth: it draws a widget per row from the same declaration
the parser reads. Nothing here weakens the inspector's argument — an instance's fields are
still named nowhere, which is exactly why a table for them would have to be invented — and
a second base class inside `engine/gfx/` is still the thing to stop.

---

## Declined, with the trigger that would reopen it

Refusals inherited from the retired arcs. Each names the condition under which it stops being
the right answer, which is what makes it a decision rather than an omission — and what stops
it being re-litigated from scratch every time somebody notices the gap.

## What stays declined, and its trigger — the runtime arc

Extending the table in
[architecture/limitations.md](limitations.md#deliberately-not-built) rather than
duplicating it. Entries below are new, or have a trigger this arc sharpens.

| Thing | Why not, and what would change it |
|---|---|
| **Clustered / tiled light assignment** | **Trigger sharpened by C8**: more lights *simultaneously inside one frustum* than the budget allows, measured after volume culling. Before C8 that number was unmeasurable, because the budget counted lights in the scene. `game/demo/assets/stress.gltf` remains the scene that would demonstrate it |
| **A second platform (Windows, macOS)** | Recorded here because it appears in no document today and is the largest single item between this engine and a shipped game. The engine is Linux, Vulkan, and two workarounds specific to the proprietary NVIDIA driver. Trigger: someone needs to run it elsewhere. Nothing in this arc makes it harder, and C1 makes it slightly easier by removing five ad-hoc index conventions |
| **`entt`, or any ECS** | Unchanged from `limitations.md` — it cannot own the instance table, and it can sit beside it. Trigger: gameplay state outgrows what a game holds in its own arrays. C1's typed handles are what such a registry would key on, so the arc moves toward it without committing |
| **Retargeting and IK** | C7 moves the animation line by one item, not three. An event track is data already in the file; retargeting and IK are solvers. Trigger unchanged: an animation set that does not match its skeleton, which no asset here has |
| **Networking** | Unchanged. The gate is a target. Note again that `CROSS_PLATFORM_DETERMINISTIC` is **off** and that flipping it is such work's *first* move, not its last — the determinism table in `limitations.md` explains why, and expects the golden set to move |
| **Scripting, `.so` hot reload** | Unchanged, and this arc does not weaken the argument: the only thing justifying a frozen ABI is a developer who cannot recompile the engine |
| **Terrain and water** | Unchanged, content-conditional |
| **Ray-tracing pipelines** | Unchanged. Ray query is an extension used inline in shaders that already exist |
| **An asset manager with reference counting** | C10 unloads models by explicit call, matching C1's create/destroy symmetry. Refcounting is what would replace an explicit `destroy` with an implicit one, and the arc's whole argument is that explicit is what was missing |
| **A general asset database or content pipeline** | C14 and C15 bake two kinds of thing, and the second one is not evidence of a third. Both are sidecars that a script writes and the engine may ignore, which is a pattern rather than a system. Trigger: a third content type needs a bake that is neither a scene nor a texture — the Rule of Threes, applied to the pipeline itself rather than to the code in it. **The trigger now has a named candidate**, which is why this row is no longer purely hypothetical — see below |
| **Rewriting the glTF in place** | The obvious way to make a scene load faster is to store it pre-chewed and stop shipping the original. `ktx2.py` already refused this once and the argument carries: a cache you can delete is a cache, and a rewritten source is a second asset format to keep in step with the first. C15 is a sidecar for the same reason, and the check that it stayed one is that deleting every `.sbin` reproduces the golden set exactly |
| **A render graph, a `RenderPass` base, or a device wrapper** | Already refused in [CLAUDE.md](../../CLAUDE.md); restated here because **D4 is the row that would produce one by accident**. Deduplicating twenty pipeline-layout blocks is one keystroke away from owning what a pass *is*, and the distance between the two is the whole rule. Trigger unchanged: a second graphics backend |
| **Deduplicating at two occurrences** | The audit that produced the D rows also found patterns at exactly two — `readEnum` in two extras parsers, the frame-0 trim loop in `Profiler.cpp`, `recordShadows` against `recordPunctualShadows`, `PI` in two shader headers — and **each is deliberately left alone**. Recorded so that a later sweep reads the absence as a decision rather than an oversight. Trigger: a third occurrence, which is the rule and not a judgement call |
| **CSG brush geometry, on the CPU or anywhere else** | **Declined by C18, which was weighed before it was started.** See below |

### CSG brush geometry, and why the union half of it is nothing

C18 asked for `engine/csg/` — half-space brushes clipped against each other into a
`scene::MeshData`, sized **L**, and the first row that would have taken the module count from
four to five. It was declined, and the argument is worth keeping because it is not the one the
card expected to lose on.

**Quake solved brushes on the CPU because a BSP tree, a PVS and a lightmap all consume the
*surfaces* of the union.** This engine has none of the three. It has a depth buffer, and two
interpenetrating opaque solids drawn through a depth buffer show exactly the boundary of their
union — so the union half of CSG does not buy the right picture, it buys the same picture with
fewer triangles. That is a number C17 already measured on this renderer: **20% fewer triangles
moves `GBuffer` 0.481 → 0.474 ms and `Frame` 3.384 → 3.339 ms**, and the room a game writes
instead is **148 triangles**.

What remains after the union is the subtraction, and it is rectangle arithmetic. An opening
that reaches an edge is not a subtraction at all — a doorway is two piers and a lintel, which
is three brushes and what a mapper builds. An opening strictly inside a face is eight
rectangles per side. **The robustness cost the card sized itself L for is created by the
clipper rather than by the geometry**: a near-degenerate vertex, a coplanar-face test and a
point-on-plane epsilon are all consequences of intersecting planes numerically, and a corner
an author typed has no tolerance to choose. T-junctions are the one real hazard and they are
answered by splitting the neighbouring face, which is four extra quads decided rather than a
tolerance tuned.

The offline shape — a step in `tools/` beside `substrate-bake`, which is where
[principles.md §9](principles.md#9-the-running-process-never-writes-a-file-a-later-run-reads-as-an-input)
puts geometry production — fails on the argument this document has already made twice, for
sprite sheets and for tilemaps: **the `.map` family agrees about less than Aseprite and
TexturePacker do, none of it is in the asset tree, and an engine-invented brush format would
be a fourth artefact for a bake pipeline held at three.** Blender and TrenchBroom both export
glTF, which is the format the engine reads.

What is given up, stated rather than elided: a boolean between solids that are not
axis-aligned; the overdraw of interior faces the union would have removed; and z-fighting
between exactly coincident coplanar faces, which is an authoring mistake with a visible
symptom rather than a correctness problem.

**Two triggers, either of which reopens it:**

- **An authored brush asset shipping in the tree** — a `.map`, or a `substrate_brush` extras
  block on a glTF node — at which point the format is *read* rather than invented and a solver
  has a file to be tested against. When that fires the row is a step in `tools/`, not
  `engine/csg/`: D9 put geometry production offline and a solve at load is on the wrong side
  of that line.
- **A game that must change solid geometry while it is running** — destructible walls, or a
  structure a player builds — where the shape is not known until play and no bake can serve
  it. C18's own card excluded that case by name. It is also the only version that needs a
  *general* mesh boolean rather than rectangle arithmetic, because it must be robust against
  inputs no author ever reviews, which makes it a dependency decision rather than a few
  hundred hand-written lines.

[`tests/BrushGeometryTests.cpp`](../../tests/BrushGeometryTests.cpp) is what a game writes
instead, compiled and run rather than sketched: a room of seven boxes and one window grid,
checked closed by an exact directed-edge match, checked exact by `memcmp` against the
coordinates its author typed, and stood up as eight static box colliders a crate falls onto.

### The third bake, and why it is one decision rather than two

The row above defers a content pipeline until "a third content type needs a bake that is
neither a scene nor a texture". **That candidate now exists on paper.**
[`specs/2026-07-29-baked-probes-and-maps-design.md`](../superpowers/specs/2026-07-29-baked-probes-and-maps-design.md)
designs RT-baked cubemap probes and shadow maps — built at build time on hardware that has
ray tracing, shipped beside the scene, replayed by a runtime that does not. It is marked
*designed, not scheduled*, and it was written without reference to C15.

That independence is the interesting part. It arrived at **the same contract C15 states**:
a sidecar beside the source, optional, invalidated by what it was built from, and deleting it
restores the original behaviour exactly. Two people reaching that shape separately is
evidence the shape is right. It is also `.ktx2`, `.sbin` and a third artefact — the Rule of
Threes, met on the pipeline itself rather than on the code inside it.

**The conclusion is not to build the database.** Neither bake wants a catalog, a dependency
graph or a content server, and nothing above changes that. The conclusion is narrower and
lands on C15's schedule rather than on this row's: **the format decisions C15 makes — the
header, the version field, what invalidation is keyed on, the `static_assert` on every struct
written, whether the writer is a script or a C++ tool that links the engine — should be made
once with the third case in view.** Made twice, they diverge, and a bake pipeline whose two
halves disagree about their own versioning is the thing that actually produces a content
system: not a decision to build one, but two sidecars that need a third component to keep
them in step.

This is the argument the Recommended order runs on, applied to a format
instead of a convention. A decision is cheap while it is still a decision.

---

## What stays declined, and its trigger — the 2D arc

Extending [`limitations.md`](limitations.md#deliberately-not-built) rather than
duplicating it.

| Thing | Why not, and what would change it |
|---|---|
| **Unifying the scene's image array with P1's** | The scene's array has no caller outside its own file, so nothing needs it yet, and doing it inside P1 is over-promotion. **Trigger: a second caller of `acquireTextureSlot`** — at which point the generation it lacks becomes a correctness problem rather than a latent one, and the merge and the fix are the same work. **P6 was the row that could have been that caller and deliberately was not**: a lit sprite needs an image in a set the G-buffer can reach, and binding P1's array as set 2 of the `gbuffer` and `shadow` layouts answers it without building the 2D content path on the one lifetime C1 never converted |
| **A `Renderer2D` or a second render path** | Trigger unchanged from the 3D case: a second graphics backend. **P4 landed as a method**, `Renderer::recordSprites`, and the engine still defines two base classes |
| **GPU sprite sorting** | ~~The bitonic sort next door would be the second occurrence, not the third. Trigger: a measurement showing the CPU sort inside the frame budget's noise floor at a stated sprite count~~ **The measurement exists and it declined the row rather than triggering it.** Ten thousand sprites cost 0.037 ms of CPU *including* the 640 KB upload, and none of it is the sort: a layer is the sort key and a position is not part of it, so `prepare()` sorts on create, destroy and reorder and does nothing at all on a frame where every sprite moved. A GPU sort would move work that is not being done. The upload has since been gated on `SpriteTable::revision()`, which takes the *other* part of that 0.037 ms away as well on any frame where nothing changed — so what is left to sort is smaller than the number that already declined the row. Trigger: a game whose sprite *set* changes shape every frame at a stated count — spawn-heavy bullet hell rather than a scrolling level |
| **A `visible` flag on a layer** | A layer is an order and a lifetime. Hiding one is a sort-key bit and a draw-count split, which is two lines — and two lines is what makes it worth waiting for a caller. Trigger: a game with a HUD layer it toggles, which is the first one anybody will write |
| **A per-sprite Z, or depth interaction with 3D geometry** | Sprites sit on the z = 0 plane and the CPU sort is the whole order. A depth test would buy interaction with geometry this pass has already declined by drawing after the tonemap, and a depth *write* is not available to a blended surface at all. Trigger: none — P6's lit sprite is the row that wants both, and it gets them by going through the G-buffer |
| **A sprite that blooms, fogs, or is reflected by SSR** | Same decision from the other side: the pass runs after the tonemap because that is the only place a texel reaches the swapchain unaltered, and everything downstream of the tonemap is downstream of those three. Trigger: none. P6 is the row |
| **A 2D scene graph** | G3's tree is the tree. Trigger: a 2D transform a G3 node cannot express |
| **Normal-mapped or deferred sprite lighting** | ~~P6 routes lit sprites through the real pipeline, which already does this. Trigger: a game wanting per-sprite normals *without* paying for the G-buffer~~ **P6 landed and the routing is exactly that**, so what is still refused is the narrow half: a lit sprite carries no *normal map*, because `sprite_lit.frag` writes the quad's geometric normal and `GpuMaterial::gameImage` names one image. Trigger unchanged — a game wanting per-sprite normals, at which point the material needs a second game-image slot rather than a second path |
| **A blended lit sprite** | P6 is a cutout: ALPHA_MODE `MASK`, so it writes depth and velocity and keeps TAA motion correction, which `InstanceTable::dynamicCount()` excludes blended instances from. A blend would draw forward, after lighting, and give all of that up. Trigger: a soft additive card — a glow or a smoke sheet — at which point `overlaySetLayout` joins the forward pipeline layout too and the variant names a `forwardFragment` |
| **A shared quad primitive across lit sprites** | Each `createLitSprite` is its own `createMesh`: four vertices and six indices, 216 bytes. One primitive with N instances would be cheaper and needs an "instance this primitive again" call the engine does not have — `GpuInstance::meta.y` is already a per-instance material index, so the mechanism is there when a caller needs it. Trigger: a stated count that makes 216 bytes a sprite matter |
| **Reclaiming a lit sprite's material slot** | One lit sprite is one material and material slots are never freed — see `GltfScene::unloadModel` for why the table is the one thing it does not reclaim. A scene's headroom is 64. Trigger: a game that spawns and retires lit sprites *during play*, which wants either a material free list or a `material` field on `LitSpriteDesc` naming one the caller already owns |
| **`SpriteTable` driving a lit sprite's animation** | `e.setLitSpriteUv(id, e.sprites().frameUv(sheet, cell))` is the whole hook and a game writes the one line. Wiring it inside would make a lit sprite also a `SpriteId`, which is the two-subsystems-in-one-struct answer P6 refused on its card. Trigger: the third game that writes that line |
| **Skeletal 2D (Spine, DragonBones)** | A solver plus an importer. C7 moved the animation line by exactly one item — an event track is data already in the file — and this is on the far side of it. Trigger: an authored asset in that form |
| **A second physics library for 2D** | Declined outright, and **P7 landed the alternative**: `ColliderFreedom::Plane2D` is one enum field that becomes Jolt's `EAllowedDOFs`, and the unit suite pins the plane coordinate to an exact equality over 300 steps. [`PhysicsWorld.h`](../../engine/physics/PhysicsWorld.h) states there is no second solver behind `Impl`. Adopting Box2D beside Jolt would be two solvers to keep deterministic, for a feature Jolt has |
| **Sprite atlas packing at build time** | An atlas is authored, and `ktx2.py`'s contract already covers compressing one. Trigger: the Rule of Threes on the bake pipeline itself, which the C arc is already tracking against a third content type |
| **A tilemap subsystem** | **Declined by P8, which was opened under an instruction to reconsider itself and did.** A tile is a sprite with a UV rect from a sheet, and `SpriteTable::frameUv` is public precisely so a caller can place one with no playback attached. The row's justification was a cost model — *"the difference between one draw per chunk and one per tile"* — and P4 made it moot by landing **one draw for all layers**: ten thousand sprites is 0.053 ms of GPU and 0.037 ms of CPU with **no sort**, since a layer is the sort key and a position is not part of it, and a screen of 16 px tiles at 1080p is 8,160. A tilemap is the case that gains most from the revision gate the upload has since acquired: a screen of tiles that did not change is now no upload at all, so the CPU half of that figure goes to zero for exactly the workload this row was about. What was left is chunking, which the game's own grid does better (1–2 bytes a cell against a `GpuSprite`'s 64) and which the engine could only own by choosing a tile id's width, a flag set and a layer count for every genre; and an authoring format, refused here already at smaller scale — Tiled's TMX and LDtk's JSON agree about less than Aseprite and TexturePacker do, neither is in the asset tree, and an engine-invented third would be a fourth artefact for a bake pipeline held at three. Collision is P7's `Plane2D` plus a run merge over the game's array. **Two triggers, either of which reopens it:** an *authored* tilemap asset shipping in the tree — a `.tmx`, a `.ldtk`, or a `substrate_tilemap` extras block — at which point the format is read rather than invented and a parser has a file to be tested against; or a game whose live sprite set exceeds **50,000**, at which point the answer is a view cull on `SpriteTable` (which has none — every live sprite is drawn) *before* it is a tilemap. [`tests/TilemapTests.cpp`](../../tests/TilemapTests.cpp) is what a game writes instead, compiled and run rather than sketched |
| **9-slice, tilemap autotiling, world-space text** | Each is a UV rect and some arithmetic over primitives P4 provides, which makes them game code until three games write them. Trigger: the rule, not a judgement |
| **Non-integer presentation scale** | Declined *because* of the guarantee: a fractional scale cannot be pixel-exact by definition. A game that wants one is a game that has opted out, and `V = window` already serves it. **Landed as a floor, not an intention** (P2) — see the row in `limitations.md`'s main table |
| **A stretch or aspect-fill presentation mode** | The only alternatives to bars are a fractional scale, refused above, and a crop of the world, which changes what the game shows rather than how it is presented. So there is one mode and it is stated in the header rather than selected. Trigger: nothing — a game that wants either has opted out of the guarantee and can render at `V = window` |
| **Render targets that survive a resize at a fixed virtual resolution** | `createRenderTargets` runs on every swapchain recreate and rebuilds targets whose size did not change, because only the presentation layout depends on the window. It is a handful of allocations on an event a human generates by dragging. Trigger: a measurement, or a platform where a resize is not human-paced |
| **Per-image sampler choice** | `pixelExact` swaps the overlay's whole image array between linear and nearest, and **P4 inherited that rather than working around it** — sprites take their textures from the same array, so a game that wants crisp sprites asks for `pixelExact` and gets a crisp UI with them. One decision for the array is right while the array holds one game's art; a UI icon and a sprite sheet wanting different filters in the same game is the second caller. Trigger: exactly that |
| **A sprite that is not axis-aligned to the texel grid** | Rotation, non-integer positions and non-integer sizes are all supported and none of them is pixel-exact, because none of them *can* be: a texel rotated by 30 degrees does not land on a texel. The guarantee is about the identity case — integer position, integer size, one world unit per texel — and `scripts/readback.sh`'s `sprite` case is what pins it. Trigger: none; this is arithmetic rather than a decision |
| **Per-frame hold times, ping-pong, and arbitrary frame lists in a sprite clip** | P5's clip is a contiguous run of cells at one rate, which is what every sheet in the asset tree is. The general form is one field — `SpriteClip` grows a `std::vector<uint32_t> frames` and `first`/`count` become its degenerate case — and it is not written because nothing authored needs it. Trigger: a sheet in the tree with a held cell |
| **A sprite sheet *file* format** | Slicing is stated where the sheet is created, in the four numbers a tool shows the artist. Aseprite's JSON and TexturePacker's agree about almost nothing, neither is in the asset tree, and a parser for a format no asset uses is a parser nobody can test. Trigger: an authored sheet description shipping with an asset |
| **A sprite animation state machine** | Declined because one already exists. `AnimationStateMachine` is generic over clip *indices* and would drive P5's clips unchanged; writing a second would be two state machines for one idea, which is the thing the shared `LoopMode`/`AnimationEvent`/`ClipPlayback` vocabulary exists to prevent. Trigger: a transition rule a skeleton's machine cannot express |

---

## What stays declined, and its trigger — the game arc

### Selectable subsystems: a game links what it names

**This section records a refusal that has since been reversed, and it is kept because the
measurements in it are still true and still the reason to be sceptical of the next version
of the idea.** G10 was opened to make a game link only the subsystems it names and closed by
declining the mechanism; `engine/Modules.h` and `substrate-guard layers` did it anyway, and
the four measurements below are what the design had to answer rather than ignore. What
changed is not the arithmetic — a game that draws one settings panel still carries most of
what the demo carries, because the linker flag in measurement 2 had already taken the large
share — but the *cost*, which turned out to be a null base class and a pointer per module
rather than the slot table the card imagined.

Two of the five things that "stay refused whatever the trigger" below are now built, and
both are answered on the terms they were refused on:

- **Registration in a module's header** was refused because "any transitive include would
  relink the dependency the registration exists to avoid". That is exactly right, which is
  why registration lives in the module's `.cpp` and never its header, and why
  `check_layers.sh` fails the build on a cluster file including a module. The guard is what
  makes the refusal unnecessary rather than what ignores it.
- **A virtual interface per subsystem** was refused as "indirection between the engine and
  its own parts", against the `Game` argument about one crossing at the edge. The
  distinction that survives is *where the seam is*: `Engine.cpp` is in every binary, so a
  call it makes by name is a subsystem every game links, and that makes it an edge rather
  than an interior. `Engine::physics()` is still a reference that exists and never a cast
  off a null pointer — the base class is the do-nothing implementation, so a game that
  links no physics runs against it and no call site tests for null. What stays refused, on
  the same terms as before, is a *common* base with a virtual `update()`.

The three triggers below were also the wrong shape: none of them fired. What actually
reopened it was the third one turned inside out — not a second engine-side consumer wanting
a slot table, but five subsystems that could not answer "which module is this in?" without
someone reading includes.

---

**The original refusal, as written, follows.**

**G10 was opened to make a game link only the subsystems it names, and closed by declining
the mechanism.** The want is real and the numbers below are the reason it is not worth what
it costs today. Everything here was measured on this machine at the commit that closed the
card, `Release`, stripped, so the comparisons are like for like.

The premise is true, and worth stating before the refusal so that nobody re-derives it:

| | Stripped `Release` binary |
|---|---|
| `game/demo` — loads Sponza, simulates, plays sound, animates, navigates | 6,054,400 B |
| A game straight out of `scripts/new_game.sh` — loads nothing, plays nothing, simulates nothing | 5,923,328 B |
| **What the whole demo game costs** | **131,072 B, 2.2%** |

So a game that draws one settings panel carries **97.8%** of what the full demo carries,
and roughly half of that is code it has no way to reach: Jolt is 1,811,979 B of symbol
bytes, miniaudio 895,737 B, fastgltf and simdjson together 554,777 B. That is the
observation G10 is built on and it survives.

**Four measurements declined the mechanism.**

1. **The row that would have established it is worth 0.64%.** G10 chose navmesh to prove
   the pattern, because `Engine.cpp.o` is the only object file that names it. Every
   `scene::NavMesh` symbol in the linked demo totals **37,877 B** — 0.64% of the scaffolded
   game above, and about 0.02% of a packaged one, where 253 MB of asset tree sets the scale.
2. **A build flag with no architecture at all is worth 21.7%.** Compiling with
   `-ffunction-sections -fdata-sections` and linking with `-Wl,--gc-sections` takes the
   demo from 6,054,400 B to **4,743,200 B** — **1,311,200 B**, thirty-four times the whole
   navmesh row — and costs 0.09 s of link time (4.11 s to 4.20 s). It reaches inside the
   dependencies the module table could only remove wholesale: Jolt 1.81 MB → 1.42 MB,
   miniaudio 0.90 → 0.62, fastgltf 0.33 → 0.20. It is not the same property — the linker
   drops functions nothing calls, where G10 wanted object files no game names — but it is a
   large part of the same *want*, and it is the thing to try first. **It has since landed
   in all four configurations** and is
   [documented with the build](tooling.md#the-linker-drops-what-nothing-calls); the figures
   in the table above therefore predate it, and the `Release` demo is 4,730,912 B today.
   The trigger below is measured against that smaller number, which is the whole reason
   this had to go in first.
3. **Carrying an unused subsystem costs nothing measurable at run time.** On the scaffolded
   game: bringing up audio, physics and navigation over an empty scene is ~1 ms of an 11 ms
   world build inside a 1.1 s launch, and turning both off with `--no-physics --set
   audio.enabled=false` moves it by about 1 ms. Per frame, stepping an empty physics world
   and updating an audio engine with no sources is **~4 µs of a 110 µs CPU frame** — inside
   run-to-run variance across three runs of 400 frames each arm. The cost of carrying what
   you do not use is bytes on disk, and unused text pages are never resident.
4. **The mechanism is not the indirection the `Game` argument blessed.** That argument is
   about *one crossing at the outermost edge*; module slots sit between the engine and its
   own parts, land injection points on `Engine`, `Scene` and `Renderer`, and turn
   `Engine::physics()` from a reference that exists into a cast off a pointer that is null
   in precisely the games the row was for.
   [principles.md](principles.md#one-crossing-at-the-edge-is-not-a-licence-for-many-inside)
   carries that argument in full, because it outlives this row.

**What did land** is the one part of the card's premise that was a defect rather than a
design: `Engine`'s constructor and destructor were inline in the header, so every
translation unit that built or destroyed an `Engine` — `Entry.cpp` included, whose eight
lines mention no subsystem — referenced `scene::PhysicsWorld`, `scene::AudioEngine`,
`scene::SceneLoader`, `core::Recorder` and `core::settings::Settings` by name. Both are out
of line now and `Entry.cpp.o` names no subsystem in either configuration. **It moves no
bytes**, and that fact is the fourth measurement: the coupling the card blamed is not what
pulls the subsystems in. `Engine.cpp.o` is, and only the mechanism above would change that.

**Three triggers, any one of which reopens it:**

- **A game in the tree that ships without a subsystem** — a 2D game with no physics, or a
  silent one — where the binary is a stated constraint rather than a preference. The number
  to beat is Jolt's 1.81 MB *after* the linker flag has taken its 0.39 MB, against a package
  measured in hundreds of megabytes. A platform with a real size ceiling is the honest
  version of this trigger.
- **A subsystem whose bring-up costs something on a game that does not use it.** The 4 µs
  and the 1 ms above are what make the runtime half of the argument; a subsystem that
  allocated, spawned a thread or opened a device unconditionally would break it. Audio is
  the closest: it is only free here because `--audio-null` and the null device path exist.
- **A second *engine-side* consumer wanting the same shape.** One slot table for one row is
  speculative generality; the Rule of Threes applies to injection points as much as to
  anything else.

**What stays refused whatever the trigger**, because these were wrong on their own terms
rather than by measurement: a `Component` base or a virtual `update()`; a templated slot map
keyed by type, which is a service locator; a `std::vector<Hook>` per phase, which makes frame
order depend on install order rather than on the visible sequence in `simulate`; registration
in a module's *header*, where any transitive include would relink the dependency the
registration exists to avoid; and a CMake option or `#ifdef` per subsystem, which buys a
build configuration nothing exercises.

---
