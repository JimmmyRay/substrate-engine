---
id: P3
title: Orthographic camera, and one depth linearization
arc: P
size: M
verification: golden-11, validation, tests-hosted, trace
---

# P3 — Orthographic camera, and one depth linearization

`Camera::Projection{Perspective, Orthographic}` and a reverse-Z ortho matrix. Three copies of `nearPlane / depth` collapse into `viewDistance()` in `frame.glsl`. **Depends on nothing and can be pulled forward past anything, including ahead of P1**

## P3, and the three shaders that assume one matrix

[`Camera::projection`](../../../engine/scene/Camera.cpp#L55) builds an infinite reverse-Z perspective
by hand and the class has no other mode. Three shaders hardcode the linearization that only that
matrix satisfies — `viewDistance = nearPlane / depth`:

- `ssr_body.glsl:116`
- `fog.comp:74`
- `particle_simulate.comp:112`

Three occurrences is the Rule of Threes, and the extraction is the one D8 already made for
`worldFromDepth`: it goes in [`frame.glsl`](../../../engine/shaders/frame.glsl), which owns
`invViewProj` and is the narrowest scope every consumer reaches. The helper takes two pushed
coefficients rather than one `nearPlane`, so both projections go through the same expression.

**So P3 finishes D8's job.** D8's own comment lists the copies it collapsed and this expression
was not among them, because at the time there was only one projection and the formula was
correct. That is worth recording as the general case: the C arc observed that consistency work
gets done when a capability row is forced through it, and this is another instance rather than a
coincidence.

**Four things do not break, and are listed so nobody re-derives them:**

- `worldFromDepth` ([`frame.glsl:73`](../../../engine/shaders/frame.glsl#L73)) goes through
  `invViewProj` and is projection-agnostic. SSAO, lighting, decals and fog reconstruct correctly
  under ortho with no change.
- **TAA jitter is already correct.** It is applied as a clip-space translation
  post-multiplied onto the finished view-projection
  ([`Renderer.cpp:4748-4761`](../../../engine/gfx/Renderer.cpp#L4748-L4761)) rather than by perturbing
  the projection, so it composes with any projection. That comment is load-bearing and should
  gain a line saying so.
- **Culling is already correct**, and `cull.comp` says so in advance: it transforms eight
  corners and tests the six clip-space inequalities rather than extracting planes, and its
  header states *"the orthographic cascade matrices go through it unchanged."*
- The sun's shadow already fits an orthographic box independent of the camera, so an ortho
  camera changes nothing about shadows.

**Two things do break, and both are found defects rather than tasks:**

- **The matrix must be reverse-Z, hand-built.** A plain `glm::ortho` is forward-Z, which would
  invert every `FAR_DEPTH` comparison — `FAR_DEPTH = 0.0` at
  [`frame.glsl:60`](../../../engine/shaders/frame.glsl#L60), tested in `ssao.comp` and
  [`lighting_body.glsl:134`](../../../engine/shaders/lighting_body.glsl#L134) — and fight both the
  reverse-Z depth clear and the `GREATER` compare ops. Reaching for the library function is the
  obvious move and it is the wrong one.
- **The sky ray fans out from an eye that does not exist.**
  [`lighting_body.glsl:51`](../../../engine/shaders/lighting_body.glsl#L51) computes
  `viewRay` as `invViewProj * vec4(ndc, FAR_DEPTH, 1)` minus the camera position. Under a
  parallel projection every ray is parallel and there is no common eye point, so this produces a
  smeared skybox. It needs the projection's forward direction instead. Nothing currently exercises
  it, which is why it is written here rather than found later.

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- `scripts/golden.sh` -- **eleven** cases, byte-identical. The card was written when the suite
  had twelve; `no-ibl` was retired for pinning a byte-for-byte copy of `lit`.
- Zero validation errors with layers on, in every capture.

Added while it was in flight, because the row turned out to have a CPU-side surface worth
pinning and a hot path worth watching:

- `./test.sh debug` and `./test.sh asan`.
- `scripts/baseline.py`, for `Lighting` and `Frame`.

## Reference

[architecture/rendering.md](../../architecture/rendering.md).

## Outcome

**Landed as described, with one correction to the plan and one to the defect.**

`Camera::Projection{Perspective, Orthographic}` is a branch inside `Camera::projection()`, not
a second class — near maps to 1 and `orthoFar` to 0, exactly as the perspective branch does, so
nothing downstream has to know which one it got. `frameBounds()` now sizes `orthoHeight` and
`orthoFar` from the scene beside the move speed and near plane it already derived, so a camera
switched to `Orthographic` after framing needs no numbers picked by hand. All four "does not
break" claims held on inspection and none of them needed a line of code.

**The plan's correction: two pushed coefficients cannot exist.** The card specified a helper
taking two, and that is not a matter of packing — it is arithmetic. A perspective depth inverts
to a multiple of `1/depth`; a parallel one inverts to a combination of `1` and `depth`. Spanning
both needs three basis functions, so no expression carrying two numbers is exact for both
families. What landed is the general rational form over **four** coefficients, which is the
projection's z and w rows solved for view-space z:

```
distance = (depth * p[3][3] - p[3][2]) / (depth * p[2][3] - p[2][2])
```

`Camera::depthLinear()` reads them off the matrix rather than rebuilding them from `nearPlane`
and `orthoFar`, so a projection and its inverse cannot drift apart — a test asserts exactly
that, by moving the far plane and requiring the coefficients to move with it. Under perspective
`p[3][3]` and `p[2][2]` are exactly 0 and `p[2][3]` is exactly -1, so the numerator folds to
`-near`, the denominator to `-depth`, and IEEE division of two negated operands is bit-identical
to the original `near / depth`. That is the whole reason the golden set could stay byte-identical
through this, and `EXPECT_EQ` rather than `EXPECT_FLOAT_EQ` is what pins it.

**Four coefficients did not fit a push constant, and that turned out to be the better answer.**
A `vec4` needs 16-byte alignment in a push block; the frame UBO is where a projection property
belongs anyway, and `ssr_body.glsl`'s own comment on `nearPlane` said as much — *"it is not
otherwise in the frame UBO"*. So `FrameUniforms` gained `depthLinear`, and `SsrPush`, `FogPush`
and `ParticleSimPush` each lost a `nearPlane`, along with `Renderer::cameraNear` and the three
lines that copied it. **Removing a float from the middle of a push block is where this row came
closest to a silent bug**: `ParticleSimPush` puts a `vec2` after it, and std430 aligns those to
8, so the shader would have inserted a hole the C++ struct did not. `collisionThickness` moved
into the vacated slot to keep the alignment, and `SsrPush` gained the explicit `pad` its
comment used to say was free.

**The sky defect is real, and it is not a smear.** Captured with the fix disabled: under a
parallel projection the old `viewRay` draws a *complete perspective sky* — blue above, ground
below, and a horizon line straight across the middle of a frame whose geometry has no field of
view to see one. The unprojection does not degenerate as it does under the infinite perspective
matrix (there `world.w` is analytically 0); under a parallel matrix `world.w` is exactly 1, so
it cleanly produces the wrong thing — a fan from a camera position the projection's rays do not
pass through. With the fix the whole sky is one colour, which is what "every ray is parallel"
means. It is the only branch on projection mode in the renderer, on `flags.w`, which was the
spare word in the flags vector.

**Deliberately deferred.** `fog.comp` builds its march direction the same eye-fan way and is
still perspective-only; it is commented in place and left, because fog is off by default, no
ortho path reaches it, and the row that turns one on is the one that should own the fix. Nothing
in the engine *sets* `projectionMode` — a game reaches it through `Engine::camera()`, and no CLI
flag or setting was added, which stays inside what this row is for.

**Verification.** Golden set **11 of 11 byte-identical**, run once before the change and twice
after. `./test.sh debug` and `./test.sh asan` both 684 green, including seven new `CameraTest`
cases for the two projections, the linearization and its bit-exactness. Validation with
`--validation on --sync-validation` and fog forced on: **800 reports, all of them
`SYNC-HAZARD-READ_AFTER_WRITE` of the `SHADER_STORAGE_READ` shape**
[limitations.md](../../architecture/limitations.md) already documents as this SDK's
over-report, and **zero validation errors of any other kind**. An orthographic capture — the
default flipped locally and reverted — runs clean with **zero** reports at `--validation on`.
Trace, 4x, 717 frames over three runs: `Lighting` 1.843 ms, `Frame` 3.308 ms. Neither zone is
touched by this change; the only hot path that is, is the SSR march's two extra multiplies and
subtractions per depth sample, and `SSR`'s 0.444 ms median sits inside the bimodal spread that
zone is documented to have.
