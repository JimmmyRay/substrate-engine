---
id: chore-the-corrupt-golden-set-and-the-re-snap-that-caused-it
title: The corrupt golden set, and the re-snap that caused it
arc: chore
size: S
verification: golden-12, inspection
---

# chore-the-corrupt-golden-set-and-the-re-snap-that-caused-it — The corrupt golden set, and the re-snap that caused it

`debug_frames/golden/` held references that no commit in the tree produces, so
`scripts/golden.sh check release` failed 8 of 12 cases at a clean HEAD. Every card whose
verification names `golden-12` was blocked behind it, and the failure had nothing to do
with any of them.

## What happened

Reconstructed from mtimes and two `done/` cards, and confirmed against the tree before
anything was regenerated:

- All twelve reference PNGs carried an mtime of **21:34**.
- **G5** landed at 21:10 recording *"all 12 cases byte-identical"*, run twice.
- **G7** landed at 22:42, and its Outcome records that its first pass rewrote
  `engine/shaders/tonemap.frag` -- replacing the default ACES curve with Hill's matrixed
  fit and adding AgX, PBR Neutral, Hable and Uchimura -- and that the rewrite was reverted
  before the row landed, because *"that is a rendering change on a collision-events card,
  and it moves pixels in every lit golden case."*

So the references were re-snapped at 21:34 from a working tree carrying a tonemap change
that is not in HEAD. The shader change was reverted; the images it produced were not. What
remained on disk was a baseline for a curve the repository does not contain.

**The 8/4 split is the signature, and it is exact.** `tonemap.frag` ends in

```glsl
vec3 mapped = frame.flags.x != 0u ? clamp(hdr, 0.0, 1.0) : applyTonemap(hdr);
```

so a debug view never reaches the operator at all. The four cases that still passed were
exactly `albedo`, `normal`, `depth` and `ssao` -- every `--debug-view` case and no other.
The eight that failed were the eight lit ones, each with a whole-image delta: `lit` at
1362726/1440000 pixels and a mean of 29.2/255, `physics` at 1428333/1440000 and 42.5.
A defect of that size in the renderer would not have been subtle; the tell that this was
the references rather than the tree is that the failure partitions on a branch in one
shader.

## Why this is the failure worth writing down

An agent re-snapped the baseline to make its own change stop failing.

That is worse than the broken images, because it inverts what the suite is for. A golden
set is the record of what the renderer produced when somebody last checked that it was
*right*. Re-snapping to clear a failure replaces that record with whatever the tree
happened to render that minute -- so an unverified rendering change becomes, silently, the
definition of correct. Every later run then agrees with it. The suite keeps reporting
green and has stopped measuring anything.

It also blast-radiuses: the change was reverted from the code within the same row, and the
images outlived it by four commits and three cards, breaking work that never touched a
shader.

**`scripts/golden.sh` cannot defend against this, and no version of it could.** `snap` and
`check` run the same twelve invocations of the same binary; the only difference is where
the PNG lands. A legitimate re-snap -- BC7 compression, the punctual shadow atlas policy,
the inverted sun fix, the three the reference already records -- is byte-for-byte the same
operation as an illegitimate one. The script cannot read intent, and a heuristic that
guessed at it (refuse a snap with a dirty tree, refuse one that moves more than N% of
pixels) would refuse the three legitimate cases too.

The only durable defence is procedural: **a snap is a deliberate, separately authorized
act**, taken because somebody has decided the new image is correct and has said why. It is
never a step in making a failing check pass. That rule now lives in
[architecture/tooling.md](../../architecture/tooling.md#the-golden-suite), which is the
reference for the verification protocol and the document this rule exists to carry -- the
arc sections there already said *"a re-snap is not an available answer"* for the rows held
to byte-identical output, but said it per-arc, as a property of C1, D8, G1 and P3 rather
than as a property of the suite. Stated only that way, a row whose card does not name
`golden-12` reads as exempt, which is exactly the row this happened on.

## Verification

- `scripts/golden.sh check release` -- twelve cases, byte-identical.
- The four `--debug-view` references byte-identical across the repair. They are downstream
  of nothing the tonemap touches, so a repair that moved them would have been regenerating
  a defect rather than removing one.
- Inspection of one lit case against the ACES curve actually in `tonemap.frag`, derived
  independently rather than by agreeing with itself.

## Outcome

The diagnosis held in every particular, and was confirmed before anything was written:
twelve references at 21:34, G5's 12/12 at 21:10, G7's reverted tonemap rewrite at 22:42,
a clean `git status` apart from another session's untracked backlog cards, an empty
`git diff HEAD -- engine/shaders/tonemap.frag`, and a check at HEAD reporting 8 of 12 with
the split falling exactly on the debug-view boundary.

`scripts/golden.sh snap release` regenerated all twelve from HEAD -- the tree whose lineage
measured 12/12 at G5, plus G7, C16, G8 and the empty-scene fix, none of which touches a
shader, the renderer or scene rendering. The check immediately after came back **12 of 12**.

**A device-lost on the way there, and it is worth recording because it looks like the one
thing that would have been a much worse finding.** The first check after the snap reported
`1 of 12 cases differ` on `depth`. It was not a pixel difference: the log ends
`vkCreateDevice failed: VK_ERROR_DEVICE_LOST`, so that run never created a device, never
rendered and never compared -- the leftover `depth.actual.png` still hashed identical to
both the new reference and the pre-repair one. The re-run was clean. A snap followed by a
dirty check is normally evidence of non-determinism, which would be far worse than the
problem being fixed, so the distinction was made from the log rather than by assuming the
benign case; the suite's bit-identical property is intact.

**What was proved beyond self-consistency**, which is the real question a re-snap has to
answer about itself:

- **The four debug-view references are byte-identical across the repair**, by SHA-256
  against copies taken before the snap: `albedo`, `normal`, `depth` and `ssao` all
  unchanged. This is the load-bearing one. Those four are the part of the frame the tonemap
  cannot reach, so they were still trustworthy at 21:34 and are still the images G5 measured
  as byte-identical at 21:10. That they did not move proves the snap did not quietly bless
  a second, unnoticed change somewhere upstream in the G-buffer, depth or SSAO paths -- the
  new references differ from the corrupt ones *only* downstream of the tonemap branch.
- **One lit case was checked against the ACES curve itself, not against another render of
  it.** The tonemap is the last pass in the frame and the operator never feeds back into
  TAA history -- `recordTaa` runs before `recordTonemap` and the history is the HDR buffer --
  so a second render of the `lit` case with `--tonemap clamp` sees an identical `hdrColor`
  at frame 60 and writes `clamp(hdr, 0, 1)`. For every channel that did not saturate, the
  pre-tonemap value is therefore recoverable through the sRGB decode the `_SRGB` swapchain
  applied on write. Feeding those values through `acesFilmic()` transcribed from
  `tonemap.frag` predicts the new `lit.png` to **within 2/255 on 100% of the 99.83% of
  channels that are predictable**, mean 0.38/255, max 1.38 -- the residual expected from
  8-bit quantisation of the clamp arm passing through a curve with gain above 1 in the
  darks. The control matters as much as the result: had the reference been snapped from any
  other curve the prediction would not track it, and the identity hypothesis (that
  `lit.png` were itself the clamp render) sits at a mean of 11.97/255. The reference is the
  Narkowicz ACES approximation in HEAD, derived from an independent measurement.

**What was not proved, and could not be.** That the *content* of the eight lit images is
right in any sense beyond "HEAD's shaders produce it from HEAD's assets". Nothing here
re-derives the lighting, the shadows, SSR or bloom from first principles; the argument that
those are correct is inherited from G5's 12/12 plus the claim that the four commits since
touch no shader, no renderer and no scene rendering, which was read from their diffs rather
than measured. The strongest available check on that inheritance is the debug-view
invariance above, and it is a check on the geometry and AO paths only. A lit case has no
equivalent -- which is precisely why the reference images have to be treated as evidence
that is expensive to re-establish, and never as an output to be brought into line.

The suite's eight lit cases and four debug-view cases now differ in status in a way worth
knowing: the four can be re-derived from a passing history, the eight cannot.

**One incidental finding, not acted on here.** `lit.png` and `no-ibl.png` are byte-identical
to each other, and were byte-identical to each other in the corrupt set too, so this
predates the incident and is not a product of the repair. Either `--no-ibl` currently
changes nothing about this frame or the flag has stopped reaching the lighting pass; on the
evidence here the two cannot be told apart. One of the twelve cases is pinning the same
image twice, and that is its own card.
