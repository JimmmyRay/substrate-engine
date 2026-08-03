---
id: D4
title: Vulkan boilerplate, at the Rule of Threes
arc: D
size: M
verification: golden-12, validation
---

# D4 — Vulkan boilerplate, at the Rule of Threes

All three findings. `Renderer::createLayout` takes the descriptor-set list **once** and creates the layout, counts the sets and verifies the shaders against that same list -- all 21 pipeline layouts converted, zero hand-written `setLayoutCount` values left. The 82 dead `!= VK_NULL_HANDLE` destroy guards are gone and each `vkDestroy*` now sits directly above its own `= VK_NULL_HANDLE`, which is what found the one handle that had a destroy and no reset. `bufferBarrier` is the counterpart `transitionImage` never had, and it took all six scaffolds; `colorAttachment` gained the LOAD overload it was missing and went from two callers to nine. **Zero standard-validation errors over 60 frames**, and sync validation reports only the documented `SHADER_SAMPLED_READ` over-report class and nothing else

## D4, and the guard it needs

Counted in [`Renderer.cpp`](../../../engine/gfx/Renderer.cpp): **19** `vkUpdateDescriptorSets`
sites, **27** `vkAllocateDescriptorSets`, **20** `VkPipelineLayoutCreateInfo`, **96**
`!= VK_NULL_HANDLE` comparisons, 6 buffer-barrier scaffolds, 11 hand-written dispatch
round-ups. Volume alone would not justify a row, and the counts are the least durable thing
here — they were 19/27/20/99 when this was written and every one of them moves with the next
pass added. Three specifics are what justify it:

1. **Fourteen pipeline-layout sites spell the descriptor-set list twice** — once as a
   `VkDescriptorSetLayout[]`, once as a brace-list to `verifyShaderBindings` — and nothing
   checks that the two agree. That is a defect class kept out by hand.
2. **The null guards are dead code.** Every `vkDestroy*` accepts `VK_NULL_HANDLE` as a
   documented no-op, so 99 comparisons are noise, and the noise hides the `= VK_NULL_HANDLE`
   reset beside them, which is the part that matters and is the part that gets forgotten.
3. **Two asymmetries where the extraction exists and half the callers cannot reach it.**
   [`transitionImage`](../../../engine/gfx/Resources.h#L80) exists for images and has no buffer
   counterpart, so the 14-line scaffold appears six times — and the comment at
   [`Renderer.cpp:3450`](../../../engine/gfx/Renderer.cpp#L3450) says it is "a lambda rather than
   four copies of eleven lines", which fixed one function and left five sites standing.
   `colorAttachment()` was extracted for the CLEAR case at two callers; the LOAD case has
   seven and never was. **An extraction that reaches some of its callers is the finding**,
   not the raw count.

## What landed, and why it stopped at nine

`createLayout(pass, shaders, sets, pushConstants, constantCount)` collapses the three
spellings into one list. The canonical case it fixes was `decal`: a
`VkDescriptorSetLayout[]` of three, a hand-written `setLayoutCount = 3`, and the same three
layouts written out again as a brace-list to `verifyShaderBindings` -- three statements of
one fact, none of them checked against the others.

**All 21 landed, but the conversion is not mechanical and one went wrong on the way.** The
`shadow` layout carries a push-constant range, and the first pass at converting it dropped
that range: it still compiled, and the failure would have been a validation error at draw
time rather than anything the build would catch. It was caught by re-reading the block
being deleted, which is not a check that scales.

The guard that got the other twelve through: **delete the old create-info block and add the
new call in the same edit, and read the block you are deleting for a push-constant range
before you delete it.** Six of the twenty-one carry one and would have failed silently.

Two cases were not uniform and are worth knowing about before anyone touches this again:

- **`ssr` had a conditional count**, `setLayoutCount = rtActive ? 6 : 3` against a six-entry
  array, with both lists spelled out again for verification. Four statements of one fact,
  and the count was the one nothing checked. It is now one list per branch, and the branch
  is the thing that actually differs.
- **`ibl` registers its set layout in `layoutBindings` only across the verification**,
  because the layout is local to the bake and destroyed at the end of the function. That
  register/erase pair now wraps the `createLayout` call rather than the bare verify.

Verified: the golden set is byte-identical across all twelve cases, and a 30-frame run with
validation layers on and `--fog` reports nothing. **The `decal` layout remains weakly
verified** -- no scene in the tree binds it -- which is the same coverage gap D8 found in
`decal.frag`, and the pipeline it belongs to is the one to be careful with next.

## The other two findings, and what each one turned up

**The null guards (finding 2).** All 82 `if (x != VK_NULL_HANDLE) vkDestroy...(x)` deleted;
every `vkDestroy*` now stands directly above its own `x = VK_NULL_HANDLE`, instead of a
block of destroys followed by a block of resets. That pairing is the point rather than the
line count, and it earned itself immediately: `Renderer::shutdown` destroyed
`computeImageSetLayout` and never cleared it -- the *only* handle in the file in that state,
and the exact defect 82 lines of careful-looking guard were sitting on top of. Benign today
because shutdown runs once; a double destroy the moment anything re-initialises.

**The two asymmetries (finding 3).** `bufferBarrier` now sits beside `transitionImage` in
[`gfx/Resources.h`](../../../engine/gfx/Resources.h) with the same argument order, and took all
six scaffolds -- including the two that barrier a sub-range of the instance buffer, which is
why it carries trailing `offset`/`size` defaults exactly as the image version carries
`baseMip`/`mipCount`. Without them the helper would have reached four of six callers, which
is the shape of the finding rather than a fix for it. `colorAttachment` gained a one-argument
overload that loads instead of clearing: seven passes had spelled those five lines out by
hand, and the CLEAR helper they were sitting next to is now defined in terms of the LOAD one.

Verified again after both: twelve golden cases byte-identical, **zero standard-validation
errors across 60 frames**, and sync validation reporting only the combined-image-sampler
class [`limitations.md`](../../architecture/limitations.md) already documents -- all image views,
no buffers, which is what a change to buffer barriers has to show.

**The guard, stated in the row rather than assumed.** D4 extracts into private methods on
`Renderer` and into the file-local anonymous namespace that already holds `colorAttachment`
and `setViewportScissor` — the two rungs the [CLAUDE.md](../../../CLAUDE.md) scope table selects for
callers confined to one class and one file. It does **not** introduce:

- a `RenderPass` base class, or any `virtual void execute()`
- a render graph with declared reads/writes and automatic barriers
- a `ResourceManager` or `TextureCache` owning GPU resources behind handles
- a `RenderDevice` / `IRenderer` wrapping `VkDevice`

Passes stay methods and keep recording `vkCmd*` inline; the engine's base-class count is
unchanged. **A second graphics backend remains the only trigger that reopens this**, and the
risk specific to D4 is that it becomes the thing that quietly pre-builds one. A reviewer's
test: if the extraction knows the *name* of a pass, it has gone too far.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- Zero validation errors with layers on, in every capture.

## Reference

[architecture/rendering.md](../../architecture/rendering.md).

## Outcome

Recorded above, under *D4, and the guard it needs*.
