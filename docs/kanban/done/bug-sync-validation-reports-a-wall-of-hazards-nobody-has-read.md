---
id: bug-sync-validation-reports-a-wall-of-hazards-nobody-has-read
title: Sync validation reports a wall of hazards nobody has read
arc: bug
size: M
verification: validation, golden, inspection
---

# bug-sync-validation-reports-a-wall-of-hazards-nobody-has-read — Sync validation reports a wall of hazards nobody has read

`--sync-validation` reports a wall of `SYNC-HAZARD-READ_AFTER_WRITE` against
`depth_pyramid.comp`, `ssao.comp`, `ssao_blur.comp`, `lighting_rt.frag`, `ssr_rt.comp` and
`composite.frag`. It is pre-existing and nobody has been through it.

**Every one seen so far has the same shape**: a `SHADER_STORAGE_READ` access reported on what
the shader declares as a `COMBINED_IMAGE_SAMPLER`. That reads like the layer attributing the
wrong access type rather than a real ordering defect — a sampled read reported as a storage read
is not a hazard the barriers could have prevented, because the read is not the access the layer
names.

**But "reads like" is exactly the position this card exists to leave.** A wall of warnings
nobody has classified is indistinguishable from a wall of warnings with one real hazard in it,
and the engine's own standard is zero validation errors in every capture — `validation` is a
verification token cards are held to. A permanently noisy `--sync-validation` means that token
covers the standard layers only, silently.

The work is to go through them, classify each as layer misattribution or real, fix what is real,
and **write down the ones that are not** so the next reader does not repeat this. If they are
all misattribution, the finding is that `--sync-validation` cannot currently be used as a gate
and the reason, stated in `tooling.md` beside the flag.

Two things worth checking early, because they would change the shape of the answer: whether the
same hazards appear under a current SDK (a layer bug fixed upstream is the cheapest possible
outcome), and whether any of the six shaders genuinely binds the same image as both a storage
image and a sampled image within a frame, which would make the layer right and the shape
argument wrong.

**It is 2760 messages at 4x**, and the same 2760 with the same per-shader distribution at
`--msaa 1` — measured while executing
[chore-emissive-costs-four-samples-to-be-read-once](../done/chore-emissive-costs-four-samples-to-be-read-once.md),
which used that invariance to prove its own attachment change had not caused any of them. At 4x
the hazard is reported uniformly for **every** G-buffer binding 0-5, 120 each, with
byte-identical `prior_usage: SYNC_IMAGE_LAYOUT_TRANSITION` text. A count that does not vary with
sample count or with which binding actually changed is itself evidence about what the layer is
reporting.

**Provenance.** Observed at `e969d87` while executing
[chore-the-edge-classifier-runs-on-every-pixel-that-does-not-need-it](../done/chore-the-edge-classifier-runs-on-every-pixel-that-does-not-need-it.md),
in Debug, on Sponza, across `--debug-view edges`, `--msaa 1`, `--msaa 8` and `--no-edge-msaa`.
Counted since, per the paragraph above; not classified.

## Verification

- `validation`: `./run.sh --sync-validation` with the standard layers on, in every capture. The
  contract is **zero unclassified reports** — either zero reports, or every remaining one named
  on this card and in the reference with the argument for why it is not a hazard.
- `golden`: thirteen cases, byte-identical. A barrier added to silence a real hazard must not
  move a pixel; if one moves, the hazard was real and the image was wrong, which is the finding.
- `inspection`: record the count before and after, and the per-shader breakdown.

## Reference update

[tooling.md](../../architecture/tooling.md), the `### Synchronization validation` section, which
is where a reader goes to find out whether the flag can be trusted.

## Outcome

**The shape argument was right about most of the wall and wrong about all of it, and that is the
finding.** Of 19 reports per frame, 13 were pure layer misattribution, **5 were real**, and 1 was
a third thing. The misattribution is what hid the real ones: because the layer calls every
sampled read a *storage* read, a genuine hazard is textually indistinguishable from a false one,
and fixing a real one does not reduce the count.

**The layer, dated.** Installed: `vulkan-validationlayers 1.3.204.1`, built 2022-04-07, against a
1.3.280 loader. At tag `v1.3.238`, `GetSyncStageAccessIndexsByDescriptorSet` still reads
`// TODO: sampled_read` and returns `storage_read` for every non-writable, non-uniform-buffer
descriptor; `v1.3.239` has the `SAMPLED_IMAGE || COMBINED_IMAGE_SAMPLER || UNIFORM_TEXEL_BUFFER →
sampled_read` branch, and current `main` still has it. **It is an unimplemented feature, not a
bug, and the line is 1.3.239** — `limitations.md` said "after 1.3.204", which was imprecise.

**No shader binds the same image as both storage and sampled**, checked by `spirv-dis` over the
built modules rather than assumed. Every reported binding but one is `OpTypeSampledImage` touched
only by `OpImageFetch`/`OpImageSample*`. Producer/consumer pairs — `ssaoRaw`, the bloom mips, the
depth pyramid levels — are separate passes with correct barriers and produce no reports at all.

**Class B, the two real ones**, both destination-scope bugs where the barrier does not name the
reading shader's stage:

- `recordGbufferRead` named `VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT` alone, and it is the only
  barrier moving the G-buffer to its read layouts — but `recordSsr` binds the same
  `view.gbufferSet` to a **compute** dispatch with nothing re-barriering in between. Four reports
  a frame, `ssr_rt.comp` bindings 0-3.
- `recordBloom` transitioned `hdrTarget` with `dstStage = COMPUTE_SHADER` only, for
  `bloom_threshold.comp`; `recordTonemap` then samples the same image in **fragment** and
  deliberately does not re-barrier, its comment saying "bloom already moved it" — true of the
  layout, false of the stage scope. One report a frame.

The engine already knew the pattern: `recordTaa` names `FRAGMENT | COMPUTE` on exactly the
analogous transition. These two were the odd ones out. **The fix is three barriers in
`Renderer.cpp`, 25 insertions and 5 deletions, every one a pure widening of a destination scope**
— which can only add ordering — and the goldens are **13 of 13 byte-identical**, which is the
expected result: these were missing ordering the NVIDIA driver's de-facto serialisation was
covering.

**Class C, the one that is neither.** `lighting_rt.frag` set 4 binding 0 is a genuine
`STORAGE_IMAGE` — the shadow mask — reported only when `render.rtShadowMask` is off. The layer
describes it accurately in every field except reachability: the `OpImageRead` sits under
`OpBranchConditional` on `SpecId 7`, false on that path, and the layer analyses unspecialized
SPIR-V. `recordShadowMask`'s transition now grants the declared read anyway, as `recordSsao`'s
disabled path already does for the same reason. It is the only change that moves the count.

Counts, 120-frame Sponza Debug captures: default 4x **2280 → 2160**, and **zero class-B in all
four arms** — default, `--msaa 1`, `--rt-shadow-mask`, and that plus `render.ssrScale=0.5`. Every
remaining report now shows the reader's own stage in `write_barriers`, which is the proof the fix
reached the right barrier rather than merely quieting something. Non-sync validation errors: 0 in
every arm. `./test.sh release` 1019/1019.

**`--sync-validation` cannot be a gate today, and the reason is now precise rather than
atmospheric.** Installing layers ≥ 1.3.239 is what would change it; Ubuntu 22.04's package is
pinned at 1.3.204.1, and the LunarG tarball or a PPA is the route. **Until then the usable rule is
not the count but a per-message test**: take the stage out of `usage:` and check whether
`write_barriers:` contains `SYNC_<that stage>_SHADER_SAMPLED_READ`. Present means misattribution;
absent means real. That two-line filter is what found both bugs.

**Four things contradicting the card, and one of them cost real money.** `limitations.md` already
carried "Synchronization validation over-reports on older SDKs" — with the count 1140 and the
instruction *"Do not silence them by widening the barriers... the baseline to compare against is
1140."* **That instruction is what let two real hazards sit in the wall**, for an unknown number
of cards. It was also stale: it listed `ssr.comp`, omitted `depth_pyramid.comp`, and stated 1140
as an absolute when the count is per-frame constant and scales with `--frames` (1140 was a
60-frame run; the same tree gives 2280 at 120). Separately, the card's **2760 is not reproducible
at HEAD** — default is 2280 (19/frame), `--rt-shadow-mask` 2520, plus `ssrScale=0.5` 2640, and
nothing reached 23/frame; its provenance is `e969d87`, before the SSR work landed. The claimed
`--msaa 1` invariance holds numerically but the 1x reports come from *different* pipelines
(`lighting1x_rt.frag`, `ssr1x_rt.comp`), so it was never the proof it read as — and it held for
two of the five real hazards too. And "the two new pipelines" is one, `shadowmask.frag`, reachable
only under `--rt-shadow-mask` since the row defaults to false.

Reference updated: `limitations.md`'s section now carries the exact fix version, the installed
version, the per-frame count, the refreshed shader list, both real hazards, and the
stage-comparison rule replacing the struck-through advice that concealed them; `tooling.md`'s
`### Synchronization validation` says plainly that it cannot be a gate, what would make it one,
and how to read a message instead of a count.
