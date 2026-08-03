---
id: G5
title: Shader variants
arc: G
size: M-L
verification: golden-12, validation, trace
---

# G5 — Shader variants

M-L

## Shader variants — the deferred registry, and its trigger

**The record loop already knows how to do this, and that is the finding that sizes the row.**

`Renderer::updateInstances` builds indirect commands in **two passes, unmasked then
masked**, so that each group is a contiguous range. `Renderer::drawShadowIndirect` then
consumes **four** such ranges — the cross product of (unmasked, masked) and (static,
skinned) — as *"one draw per (pipeline, vertex buffer) pair"*, ordered by pipeline so the
expensive state change happens twice rather than four times, with lazy binding because
binding unconditionally measured as most of a 0.1 ms regression on the punctual atlas pass.

Variants generalise that from two groups to N:

- A `ShaderVariant` array of `{name, gbuffer vert/frag, forward frag, shadow frag,
  constants, cull, blend}` resolving to a `VkPipeline` created **lazily, on first use**.
  Not the cross-product — the 512-variant warning that motivated the original deferral is
  about eager enumeration and still stands.
- The command builder groups by variant instead of by mask, and keeps a
  `{variant, firstCommand, count}` range list. The draw loop binds and issues one indirect
  draw per range, preserving the lazy-bind and pipeline-ordering behaviour already there.

**Two consequences worth stating before the row is built.**

*Instancing survives, by the argument the code already makes.* A merged run shares a
primitive, one primitive has one material, and one material has one variant — so a run was
already all-one-variant, exactly as it was already all-masked or all-unmasked.

*Grouping fragments runs, and that cost grows with variant count.* `updateInstances`
records that a merge requires slots to be **adjacent**, so splitting the walk breaks runs
that would otherwise form across a group boundary. On Sponza this is worth nothing — its
103 primitives are all distinct and nothing merges at all — but on an instance-heavy scene,
N variant groups fragment more than two mask groups do. This is a measurement to take when
the row lands, not a reason to avoid it.

**The contract, and two systems that already cover it.** Game GLSL honours a contract that
exists implicitly in `gbuffer.frag` today — write albedo, normal, ORM and emissive; read
`instance.glsl`, the material buffer and the bindless array — made explicit as an includable
`engine/shaders/gbuffer_contract.glsl`. Then: `pollShaderReload` rebuilds every pipeline on any
shader mtime change, so a game shader hot-reloads once its directory joins the watch; and
the SPIR-V reflection check aborts in Debug on a descriptor-type or `constant_id` mismatch,
which is precisely the safety net game-authored GLSL needs and precisely the failure a game
developer would otherwise see as a black screen.

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- Zero validation errors with layers on, in every capture.
- Per-pass GPU cost from `scripts/baseline.py` trace medians, several runs
  per arm -- never the `GPU @` log line.

## Reference

[architecture/rendering.md](../../architecture/rendering.md).

## Outcome

The row was right that the record loop already knew how to do this. `updateInstances`
went from a two-group walk to an N-group one by taking the body it already had, wrapping
it in a lambda and calling it once per variant; the deformed half, which was a second
copy of the same walk, became the same lambda with a flag. Net, the builder is shorter
than it was and grouping is one concept instead of two.

**What landed.** `gfx::ShaderVariant` — `{name, gbuffer vert/frag, shadow frag, forward
frag, constants, cull, blend}` — with `Renderer::addShaderVariant` returning the index a
`GpuMaterial::shader` field stores. Variant 0 is the engine's own triple and is a member
initialiser rather than something `init()` registers, which is what makes a
zero-initialised material name the default and fixes a game's first index at 1. Pipelines
are created per `(variant, pass)` on first use and nulled wholesale by `destroyPipelines`,
so a feature toggle and a hot reload rebuild a game's shaders on exactly the engine's
path. `VariantRange` names each group; `drawSceneIndirect` walks the list, binds a
pipeline and a vertex buffer lazily, and issues one indirect draw per range.
`engine/shaders/gbuffer_contract.glsl` is the fixture, and `gbuffer.frag` is its first
consumer rather than an exception to it.

**Two things the row did not predict.**

*The range list wants a different order from the command buffer.* Commands must stay
static-then-skinned, because that split is what lets a pass bind one vertex buffer per
half. Ordering the *walk* by variant — which is the pipeline-major behaviour the card
asked to preserve — therefore cannot be read off a running total, so a range carries
`first` explicitly and the two half-lists are `std::inplace_merge`d by variant. That one
line is the whole of it, and without it a scene with N variants pays 2N-1 pipeline binds
instead of N.

*The obvious builder is O(slots x variants registered), not O(slots x variants used).*
A game declaring forty variants for a project and placing two in a level would have
walked the instance table forty times per rebuild — which is the opposite of what "lazy"
was supposed to buy. One O(slots) pass marks which variants any live instance uses, and
the sweep skips the rest. Registering a variant a level never places now costs nothing on
the CPU as well as nothing on the GPU.

**`GpuMaterial` gained `shader` and `params`, which G4 deferred to this row.** 80 bytes to
96, `pad` reused for `shader`, and `kSceneCacheVersion` bumped — though the layout digest
would have caught it regardless. `shader` is read by the CPU only: by the time a fragment
runs, the answer is the pipeline it is running in.

**What was deliberately not done.** A variant's vertex stage reaches the G-buffer pass and
nothing else — the shadow and velocity passes keep the engine's. A variant that only
shades differently is unaffected; one that *displaces* geometry in its own `gbuffer.vert`
would cast an undisplaced shadow and write an undisplaced motion vector. Making a vertex
shader reach every geometry pass is a different row, and the constraint is stated in the
contract header rather than left to be discovered. The shadow pipeline also ignores
`ShaderVariant::cullMode` on purpose, for the parity argument `createPipelines` already
made against the traced path.

**The demo exercises it, and that needed a new decision.** G5b recorded that the demo
ships no shaders and *should not*, because one it used would change every golden image.
That is true of a shader **named after an engine shader** — the case G5b actually tested,
where the game's file wins the lookup. A file with its own name is additive, so
`game/demo/shaders/hologram.frag` exists, the F-key cube's material names it, and all
twelve goldens are byte-identical because no scene in the suite selects it. It is the
first thing in the tree to use G5b's second shader tree at all.

**Verification.**

- `scripts/golden.sh check release` — **all 12 cases byte-identical**, run twice: once
  after the main change and once after the two follow-ups (`setScene` invalidating the
  variant cache, and the used-variant pre-pass). That is the claim that routing every
  geometry pass through a variant table, and rewriting `gbuffer.frag` onto an includable
  contract, changed no pixel.
- `./test.sh` in four configurations, each its own invocation: debug, release, asan,
  tsan — **641 tests passing in every one**.
- Validation layers, debug, headless, `--frames 150`, three configurations —
  defaults; `--msaa 1 --no-rt --taa`; and `skin.gltf`, which is the one scene with a
  deformed half and therefore the one that exercises the merged range list. **Zero
  `[ERROR]`, `[CRITICAL]` or VUID lines** in any of them.
- The reflection check was verified against deliberate breakage on *game* GLSL, which is
  the arm this row adds: moving `hologram.frag`'s constant to `constant_id = 9` aborts in
  Debug with *"hologram: hologram.frag declares constant_id 9, but the pipeline supplies
  only 9 constants"*, at the first frame that draws it. Reverted.
- Trace medians, `scripts/baseline.py --samples 4 --runs 3 --frames 400`, 717 frames per
  arm, three arms after the change against one before it:

  | zone | before | after (3 arms) |
  |---|---|---|
  | `Lighting` | 1.843 | 1.837 / 1.838 / 1.833 |
  | `GBuffer` | 0.481 | 0.480 / 0.480 / 0.478 |
  | `Frame` | 3.200 | 3.248 / 3.272 / 3.243 |
  | CPU busy | 0.115 | 0.111 / 0.109 / 0.112 |

  `Lighting` and `GBuffer` are flat or a hair down, and CPU busy is down — which is the
  number that would have moved if the draw loop had got more expensive. `Frame` reads
  ~1.5% high, and its spread across the three after-arms (0.029 ms) is most of that
  distance; nothing in the GPU zones accounts for it and the CPU is doing less work, so
  it is read as run-to-run drift rather than as a cost. Sponza registers one variant and
  uses none of it, so there is nothing here that *could* be slower by construction.

**The fragmentation measurement the row asked for is not taken**, and could not be
honestly: it wants an instance-heavy scene using several variants, and no asset in this
tree has one. What the code does instead is bound the cost and say so — the builder walks
the table once per variant *in use*, and `VariantRange`'s comment states that merges
across a group boundary are what grouping gives up. The measurement belongs to the first
scene that has something to measure.
