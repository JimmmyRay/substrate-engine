---
id: C35
title: Clustered light assignment
arc: C
size: L
verification: golden, trace, tests-4
---

# C35 — Clustered light assignment

The deferred lighting pass loops every light in the frame for every pixel, for every sample.
This card builds a cluster grid with a per-cluster light list, so the loop iterates the lights
that can reach the pixel rather than the lights in the view.

**A C row rather than a chore, and Phase 3's own milestone is the reason:** *"a scene with
several hundred lights holding frame time"* ([arcs.md](../arcs.md), Phase 3). It sits beside
**C8 — light volume culling**, which is in `done/` and is the half of this problem that culls
lights against the *view*. What is missing is the half that culls them against the *pixel*.

**The trigger fired, and it is measured.**
[limitations.md](../../architecture/limitations.md), "Many-lights scaling is delegated", names
`game/demo/assets/stress.gltf` as "the scene that would demonstrate it". It does — release, 4x,
three runs of 900 frames:

| scene | lights | meshes | `GBuffer` | `Lighting` | `Frame` |
|---|---|---|---|---|---|
| demo (Sponza + demo world) | ~9 | 103 prims | 0.477 | 2.790 | 5.377 |
| `stress.gltf` | **40** | 2 | 0.088 | **3.003** | 3.845 |

Forty lights over two meshes puts `Lighting` at **78% of the frame** while `GBuffer` collapses
to a fifth of Sponza's. The cost is tracking light count, not geometry, exactly as
`O(pixels × samples × lights)` predicts. Forty is not a large number for a real level.

**4x as a fixed baseline is what makes this urgent rather than eventual.** The sample count
multiplies the same term clustering removes, so committing to 4x doubles down on it.

Both enabling properties `limitations.md` lists still hold: the light list is a storage buffer
whose length the shader reads
([`lightCount` in `shadeSample`](../../../engine/shaders/lighting_body.glsl)), and
`render.lightBudget` makes the shading cap explicit data rather than a hidden constant. C8's
volume cull already narrows the list to lights whose volume reaches the view, so this builds on
a list that is already conservative rather than starting from the scene.

Cheapest useful shape first: a 2D tile grid with a per-tile light list built in compute. The
depth pyramid it needs for tile depth bounds already exists from `recordDepthPyramid`. Froxels
only if tiles are not enough — and the card should say which it landed, because "clustered" and
"tiled" are being used interchangeably here and the tree should not be.

Ships with `render.lightClusters`, defaulting on, following `render.culling`,
`render.occlusionCulling` and `render.meshLod`
([the `render.culling`, `occlusionCulling` and `meshLod` rows](../../../engine/core/Settings.h)) — all three exist as escape
hatches for optimisations that are faster *and* equivalent, which is how equivalence gets
proven rather than asserted.

**What I expect to be wrong about:** the size. L assumes a tile grid and a compute pass. If the
light list needs a different residency model to be indexable per tile, this becomes XL and
should be split rather than grown.

**Provenance.** Every figure above was measured at `37c2d44`, before the per-view refactor (C31, C32) landed. Re-run the arm before acting on a delta.

## Verification

- `scripts/baseline.py --config release --zones --samples 4 --runs 3 -- res:/stress.gltf` — the
  arm this card exists for. `Lighting` must fall substantially; a frame that does not move on
  forty lights means the assignment is not narrowing anything.
- The same on `res:/Sponza/glTF/Sponza.gltf`, which has too few lights to gain — it is there to
  show the machinery costs nothing when it cannot help.
- `scripts/golden.sh` — thirteen cases, **byte-identical**. Clustering is an equivalence, not an
  approximation; a moved pixel means the cluster test is not conservative.
- The same frame with `render.lightClusters` off must match it with clustering on. That is the
  check the escape hatch exists for, and it is stronger than the golden set because it isolates
  this one change.
- `./test.sh` in four configurations, each its own invocation — a compute pass with its own
  buffers.

## Reference update

[rendering.md](../../architecture/rendering.md), the lighting section, and
[limitations.md](../../architecture/limitations.md) — the "Many-lights scaling is delegated"
entry and the deferred-work row that names its trigger both stop being true and must be
retired rather than half-edited.

## Outcome

**Landed, and it is *tiled* — the card's own question, answered.** 16x16 screen tiles, one
workgroup per tile and one invocation per pixel, writing a fixed-stride bitmask the deferred loop
walks. The six planes are the tile's four screen-rectangle sides plus two depth planes from its
own min and max `gDepth` — one frustum slab fitted to the depth actually present. Clustered would
mean subdividing z on a fixed schedule, and **the addressing has no z coordinate at all**:
`(tile.y * tilesX + tile.x) * words`.

`recordLightClusters` sits in `recordViewChain` between `recordGbufferRead` and
`recordShadowMask`/`recordLighting`, with a write-after-read dependency in front of the dispatch —
the buffer is per view, so the previous frame's lighting draw is still reading that copy — and a
compute→fragment buffer barrier behind it. `frame.clusterParams.z` is zero exactly when assignment
is off, which is the one value both loops branch on; `render.lightClusters` is live-bound rather
than a specialisation constant, so `featureKey` correctly has nothing to do with it. `shadeRayHit`
is deliberately left on a flat loop, because a reflection hit is not in the tile's slab, and the
depth bounds come from `gDepth` per sample rather than the Hi-Z pyramid.

**The equivalence is exact, and it is exact for a reason worth keeping.** Iterating a fixed-stride
bitmask low-bit-first *is* ascending light index, so the float sum is bit-identical — no
compaction, no per-tile count, no atomic whose completion order is observable. `cmp` on frame 60,
assignment on against off, **0 differing bytes** on: Sponza + demo world at 4x; `stress.gltf` at
4x; stress at range 2, where 61% of the lighting work is removed and the image still does not
move; stress at range 2 with `render.rtShadowMask=true`, the only path exercising the modified
`shadowmask.frag`; the same under `--taa`, whose jittered `viewProj` no golden covers; the same at
`--msaa 1`, exercising `light_cluster1x.comp`; and at `lightBudget = 1024`, where the stride
exceeds the words in use.

**The card's pass criterion is not met, and the card's inference rule for that outcome is
wrong.** It says `Lighting` must fall substantially on `stress.gltf` and that a frame which does
not move "means the assignment is not narrowing anything". `Lighting` on `stress.gltf` does not
move at all — 3.034 on against 3.039 off — and the assignment is narrowing correctly. Changing
only the authored `range` on that same scene:

| `stress.gltf` range | `Lighting` on | `Lighting` off | `Frame` on | `Frame` off |
|---|---|---|---|---|
| 12.0 (as authored) | 3.034 | 3.039 | 3.990 | 3.934 |
| 6.0 | 1.694 | 1.769 | 2.451 | 2.485 |
| 2.0 | **0.229** | **0.593** | 0.770 | 1.088 |

Its forty lights carry `range: 12` on a ring of radius 5 over a 12x22 ground, so every point
within radius 7 of the origin is reached by all forty: there is no residue on screen to remove,
and that 3.0 ms is ray-traced shadows for lights that genuinely reach. **No assignment scheme can
remove those.** The deeper reason is that `lighting_body.glsl` already exits on
`dot(radiance, radiance) <= 0.0` *before* the shadow ray and before `shadeLight`, so an
out-of-range light already cost one `Light` load and a few flops — tiling removes that residue and
nothing else. The claim to carry forward is **many lights whose reach does not cover the screen**,
not many lights.

Sponza behaved exactly as the card predicted for the control arm: `Lighting` 2.817 on against
2.828 off, with the pass costing **0.057 ms**, about 1% of frame — the machinery being cheap where
it cannot help.

Verification: **13 of 13 byte-identical** (`cmp` against the baselines, not the suite's
tolerance-2 verdict). The first attempt died on `ssao` with `VK_ERROR_DEVICE_LOST`; re-ran once
per the rule and it was clean. `./test.sh` debug, release, asan and tsan — **1055 each, all
passed**, four separate invocations; since `test.sh` links only the hosted sources and never
compiles `Renderer.cpp`, `./build.sh asan` and `./build.sh tsan` were run separately to compile
the renderer under both, clean. **Zero standard validation errors** in every run. Sync validation
reports 1080 on Sponza and 810 on stress, **every one classified as misattribution** by
`tooling.md`'s documented stage filter — none names `light_cluster.comp`, the lighting draw or the
shadow-mask draw. An ASan draw run (`--no-ray-query`) was clean, with LeakSanitizer's 49 KB
entirely GLFW, libdbus and driver modules.

**Over-budget, observed rather than assumed.** At `lightBudget = 2000`:
`render.lightBudget is 2000, past the 1024 lights clustered assignment can index; running the
deferred loop over every light in the view instead.` The pass does not run, the frame renders, no
light is dropped. At exactly 1024 there is no warning and the pass runs — the ceiling is exact.

**The L held.** The residency worry the card named as its XL trigger resolved cleanly: a
fixed-stride bitmask needs no different residency model, which is also what buys the bit-exactness
above.

Four smaller contradictions. The card says eleven golden cases; there are thirteen. It says
`stress.gltf` has 40 lights — the frame shades **32**, the default budget of 31 point lights plus
the sun, since the scene was authored to exceed the budget. It cites `render.lightBudget` as a
settings row; it moved to `GameSetup::lightBudget`, and the new warning still spells the old key.
And `LightClusters` is in neither `TABLE_ZONES` in `scripts/baseline.py` nor the `GPU @` line, so
the pass is invisible outside `--zones` — left alone deliberately, because adding it would put the
tool's table out of step with the one published in `tooling.md`.

**Deferred, with a destination.** The pass is named *cluster* in nine places and *tile* in three,
which is the confusion this card's own text asked not to create — and inherited, since the card
specified `render.lightClusters` itself. `light_cluster.glsl`'s header already says "Tiled, not
clustered". Opened as
[chore-the-tile-pass-is-named-cluster-in-nine-places-and-tile-in-three](../backlog/chore-the-tile-pass-is-named-cluster-in-nine-places-and-tile-in-three.md),
with the symbol lists and the one real decision in it: `render.lightClusters` is a public settings
key.

Reference updated: `rendering.md` gains "Tiled light assignment — and it is tiled, not clustered",
which is the section `light_cluster.glsl`'s header was already pointing at and which did not
exist; `limitations.md`'s "Many-lights scaling is delegated" is struck and replaced with what
survives it — a cap is still a cap, the importance approximation is untouched, and `stress.gltf`
was the wrong scene to have named.
