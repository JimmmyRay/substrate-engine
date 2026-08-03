---
id: C11
title: Occlusion culling
arc: C
size: L
verification: golden-12, validation, trace
---

# C11 — Occlusion culling

Two-pass Hi-Z occlusion culling: `depth_pyramid.comp` min-reduces this frame's depth into a mip chain, and `cull.comp` runs twice -- once over what was visible last frame, once over everything against the pyramid the first pass produced. ~~**Needs C9**~~ **C9 landed and was never the gate.** `--no-occlusion` and `render.occlusionCulling` toggle it. **All twelve golden cases stay byte-identical with it on**, which is the property the shape was chosen for.

**This row was `LOD and occlusion culling` and is now the occlusion half alone.** The LOD half
is [C17](C17-mesh-lod-chains-and-the-simplifier-that-generates-them.md), and the split is the
point: the occlusion work landed, was measured and has been guarding the golden set for
months, while the LOD half was gated on content that still does not exist. One card cannot be
both done and blocked, and describing a shipped feature as blocked is the failure the blocked
column exists to prevent.

## The barrier bug C11 left, and how C10's testing found it

Adding `SceneLoader` meant re-running validation, and it caught a regression the earlier
C11 verification had missed: the phase 0 cull dispatch binds the Hi-Z pyramid, and on frame
one the image was still `UNDEFINED` while the descriptor declared `GENERAL`. The transition
now happens before the first cull rather than inside the pyramid build.

Fixing that exposed a second one, and it is the more useful finding: the new transition
named `VK_ACCESS_2_SHADER_SAMPLED_READ_BIT`, and sync validation still reported a hazard at
every dispatch. **A combined image sampler read in a compute shader is attributed to
`SHADER_STORAGE_READ`, not `SHADER_SAMPLED_READ`** -- so a barrier naming only the sampled
form covers none of it. Naming `VK_ACCESS_2_SHADER_READ_BIT` took the hazard count from 45
per frame back to the documented 18. This is the same attribution quirk
`recordCull` already documents for `vkCmdFillBuffer` and `ALL_TRANSFER`, in a second place,
and it is worth knowing it generalises: **when sync validation reports a barrier that
"looks right and orders nothing", suspect the access mask before the stage.**

## C11's occlusion half, as landed — and what it measured

The two-pass shape was chosen over the cheaper single-pass one for the reason argued
below, and it delivered the property it was chosen for: **twelve of twelve golden cases
are byte-identical with occlusion culling on.** That was verified in two steps, and the
first is the one worth copying:

1. **The structure, with the test disabled.** Splitting one G-buffer draw into two batches
   reorders rasterisation even when it drops nothing, and the whole byte-identical
   guarantee rests on nothing being order-dependent. So the two-pass structure landed with
   `occlusionEnabled = 0` first, and golden was run against *that*: 12/12. Draw order was
   not the risk it looked like, and knowing it independently meant every later failure had
   one candidate cause instead of two.
2. **Then the test.** 8 of 12 failed immediately, and both causes were mine.

**The two bugs, because neither announced itself.**

- **The pyramid mapped onto the wrong screen.** Mip 0 is `floorPow2(extent)` -- 1024 wide
  for a 1920-wide image -- and it was built with `texelFetch(p * 2)`, which walks 2048
  source texels. Pyramid UV and screen UV therefore disagreed by about 7%, so every box was
  tested against depth from somewhere slightly to its right. The base texel now comes from
  the *size ratio*, which reduces to `p * 2` for every level after the first.
- **`pointSampler` is linear, and its `maxLod` is zero.** The name is a misnomer -- it is
  created as the *fullscreen* sampler with `VK_FILTER_LINEAR` -- and like most samplers
  here it leaves `maxLod` at the default. Both are quietly fatal to a Hi-Z test: filtering
  averages a near surface with a far one and yields a depth belonging to neither, and an
  unset `maxLod` clamps every `textureLod(..., level)` to **mip 0**, so the entire level
  selection was decorative and each box was tested against a single full-resolution texel.
  That is what the 1844 wrongly-culled pixels were. There is now a `hizSampler`: nearest,
  nearest mipmap, `VK_LOD_CLAMP_NONE`.

  **This is worth a second look elsewhere.** `pointSampler` is what `ssao.comp` reads depth
  through, under a name that says it does not filter.

**And three missing barriers, which did not corrupt an image -- they lost the device.**
After the sampler fix the images matched, but golden began failing intermittently with
`vkCreateDevice failed: VK_ERROR_DEVICE_LOST`, on a *different* case each run. Sync
validation named all three: the pyramid's opening layout transition was sourced at
`TOP_OF_PIPE` while the phase 0 cull dispatch still had it bound; the phase 1 dispatch
wrote `instanceData` while phase 0's `vkCmdDrawIndexedIndirect` was still reading commands
out of it; and the same dispatch overwrote the visibility buffer phase 0 had read.
Different *ranges* of a buffer are not a barrier. **A missing barrier here does not look
like a rendering bug at all -- it looks like flaky hardware**, and the only reason it was
attributable was that the failure moved between cases while the images stayed identical.

**What it costs, and what it gains here.** Measured from the trace over 717 frames at 4x,
release, three runs per arm:

| | occlusion on | off |
|---|---|---|
| `GBuffer` | 0.478 | 0.480 |
| `GBufferLate` | 0.037 | 0.037 |
| `HiZ` | 0.048 | 0.048 |
| `CullHiZ` | 0.008 | 0.007 |
| `Frame` | **3.209** | **3.238** |

**It gains nothing in Sponza, and that is the honest result.** The pyramid and the second
dispatch cost about 0.09 ms; `GBuffer` does not move, because Sponza is one open atrium
seen from a camera with almost nothing behind a wall, and its scene is a hundred-odd large
draws rather than the thousands of small ones this technique exists to thin. The feature is
correct and it is unexercised, which are different failures from each other and only one of
them is fixable here.

It is left **on** by default anyway, for one reason: with it on, every golden run re-proves
the image-preserving property. Off, the code rots. The 0.09 ms is the price of that check
until there is a scene that shows the other side, and `--no-occlusion` is how a measurement
gets the other arm.

## Why it is two passes and not one

The row said "folded into the existing `cull.comp`", which describes a **single-pass** Hi-Z
test: build a depth pyramid from last frame's depth, and in the cull dispatch reject a
command whose screen-space box is behind it. That is the cheap version and it collides
head-on with the property this renderer is built around.

**Occlusion culling that removes anything currently drawn changes the image, and the golden
suite requires byte-identical output.** So a single-pass test has exactly two outcomes in
these twelve scenes: it culls nothing, in which case the feature is untested and costs a
dispatch; or it culls something, in which case all twelve references must be regenerated
and the new ones bake in whatever disocclusion artifacts the last-frame pyramid produces.
Neither is a landing.

The version that does not force that choice is **two-pass**, and it is worth the extra
work precisely because it is *exactly* image-preserving by construction:

1. Draw the commands that were visible last frame.
2. Build the depth pyramid from **that** depth — the current frame's, not the previous
   one's.
3. Test everything against it, and draw whatever passed and was not already drawn.

Nothing visible is ever dropped, because anything that fails the last-frame test is
re-tested against real current-frame depth before being discarded. The golden references
stand unchanged, which is the only way this row lands without a separate argument about
what the reference images mean.

It is not free: a persistent per-command visibility bit, a mip-chained depth pyramid with a
min-reduction sampler, a second cull dispatch, and the G-buffer pass splitting into two
draws. It also needs one thing checked rather than assumed -- **whether splitting the draw
into two batches changes depth-tie resolution between coplanar surfaces**, since the whole
byte-identical guarantee rests on nothing being order-dependent, and this reorders draws
even though it does not drop any.

## The LOD half left, and why it is not a corner of this row

Screen-coverage selection is perhaps thirty lines in `cull.comp`, and every one of them
would be dead: no asset in the tree authors an LOD chain and glTF carries none, so the
selection would be a parameter that always resolves to LOD 0. The enabling work is a mesh
simplifier, which is a row of its own — it is
[C17](C17-mesh-lod-chains-and-the-simplifier-that-generates-them.md), and the argument for
the split is there rather than repeated here.

**C12 was a row rather than a delegation, and that decision held.** Everything before it is engine-shaped: a game with no AI still wants
handles, queries, saves and LODs. C12 is the first row that a genre could make wrong, and if a
target exists by then it should shape it rather than be retrofitted to it.

---

## Verification

All three were run and passed:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- Zero validation errors with layers on, in every capture.
- `scripts/baseline.py` over several runs per arm. Quote `Lighting` and `Frame`; `GBuffer`
  settles into one of two whole-run states about 5% apart and a single run cannot tell them
  apart.

## Reference

`architecture/rendering.md` for the pass.

## Outcome

Measured at 717 frames, release, three runs per arm: `GBuffer` 0.478 against 0.480 ms, `Frame`
3.209 against 3.238. **No gain in Sponza and roughly 0.09 ms of cost** -- Sponza has almost no
occluded geometry, so the result is what it should be, and the row is kept for the scenes that
do. Its testing also found three missing barriers that were producing intermittent
`VK_ERROR_DEVICE_LOST`.

**Closed by splitting rather than by finishing.** The blocker was rechecked against the tree
rather than reasoned from, which is what the blocked column is for, and unlike the three
gates `arcs.md` struck through this one was real: no simplifier among the submodules, no
`lod` field anywhere in `SceneTypes.h`, `SceneData.{h,cpp}` or `GltfScene.h`, no selection
logic in `cull.comp`, and every `.gltf` in both asset trees single-LOD. What that recheck
actually established is that the blocker never applied to this row's shipped half. The
occlusion work is landed, measured and re-proving its image-preserving property on every
golden run; it was sitting in `blocked/` only because it shared a card with work that had
not started. C17 carries that work, and carries the blocker with it.
