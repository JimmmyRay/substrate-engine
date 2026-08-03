---
id: C33
title: A pass chain records without presenting
arc: C
size: L
verification: golden-11, trace, validation
---

# C33 — A pass chain records without presenting

Third of C25's four, and the one that delivers the capability. Afterwards `drawFrame` records
the chain once per registered view, each ending in a tonemap into that view's own image, and
presents only the one bound for the swapchain.

**The seam is already there and is narrower than it looks.** Everything from
[`Renderer.cpp:6982`](../../../engine/gfx/Renderer.cpp#L6982) to `:7068` — instance upload,
cull, skinning, shadows, G-buffer, Hi-Z, decals, SSAO, velocity, lighting, forward, particles,
SSR, fog, bloom, TAA — touches **no swapchain object at all**. `recordTonemap` at `:7069` is
the first consumer of `imageIndex`, and it reaches it through one accessor pair,
`composeImage`/`composeView` ([`Renderer.h:1474-1479`](../../../engine/gfx/Renderer.h#L1474)),
whose header comment already argues the shape: *"One test in one place -- the emptiness of
`presentTarget` -- rather than a flag each pass has to agree with."* Four call sites consume
it. A view that names its own destination is a third answer from that same pair.

`recordPresent` already early-outs on `presentTarget.image == VK_NULL_HANDLE` (c6125), and the
`PRESENT_SRC_KHR` transition at c7141 is the only other swapchain write in the tail.

**Render serially through one target set** rather than allocating a set per view. Each view
runs to completion — cull with its matrix into slot 0, then every pass, then tonemap into its
own image — before the next begins, with a barrier between. That is C25's own suggestion, and
it is what keeps the memory cost one destination image per view instead of seventeen. C31 is
still required: the targets have to be addressable as a set before they can be reused as one.

**Two things C32 deliberately left here, and the reason is this card's own trace clause.**
C32 made the uniform block, the light buffer, the shadow-matrix buffer and the frame
descriptor set per view, and moved `prevViewProj`, `visibleInstances` and `visibleTriangles`
onto the `View`. It did **not** touch two items its own inventory listed:

- **`cullViewProj` is still `kCullViews` = 26 entries, with the camera at 0.** Growing it to
  27 is a 27th dispatch in *every* frame, including the ones with one view in them — which
  C32's verification forbade and this card's does not, because this is the row that gives the
  27th list something to record. Growing it also means `kOcclusionView` and
  `kCullCommandLists` move, and both are read back from `cullStats` when the overlay reports
  visible instances.
- **The TAA jitter phase is still `framesSubmitted % kTaaJitterCount`.** `framesSubmitted` is
  the public frame counter and is genuinely per frame, so the per-view quantity is the phase
  rather than the count. With TAA off for a non-primary view this may need nothing at all —
  but a second view that is *jittered* while its TAA is off is a view rendering off-centre
  for no reason, so decide it explicitly rather than by inheritance.

C32 also verified something this row can rely on rather than re-establish: with the primary
view pointed at uniform block **1** instead of 0, the golden set is byte-identical across all
eleven cases and validation is clean. The second block is allocated, written and bound
correctly; what is untested is two of them in one command buffer, which is this row.

Two consequences to state rather than discover:

- **TAA is off for a non-primary view in this row.** History is per view, the set is shared,
  and a shared history smears one view into the other. C31 gives each view its own history if
  it is later worth the memory; nothing needs it yet.
- **A view costs a full chain.** Two views is roughly two frames of GPU work, which is the
  honest cost of a mirror and the reason a view will want to declare a resolution scale
  before anything ships four of them.

Expected to be wrong about: whether serial reuse survives the barriers. Every pass reads the
target the previous one wrote, so re-running the chain into the same images needs the last
pass of view A to have finished before the first pass of view B begins — which may cost more
than the memory it saves.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. Every case registers no extra view, so a
  moved pixel means the one-view path changed rather than gained a sibling.
- `trace`: `Frame` and `Lighting` from `scripts/baseline.py`, several runs per arm. **A
  one-view frame must not get slower for holding the machinery for two**, and a two-view frame
  should cost about two.
- Zero validation errors with layers on, including a capture with two chains recorded into one
  command buffer. Layout transitions between the two are what this is really checking.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — the frame structure, which
describes acquire-record-present as one sequence.

## Outcome

`recordViewChain` is everything from the cull of list 0 to the tonemap; `drawFrame` runs it
once per registered view with `recordViewBarrier` between, secondary views first and the
presenting one last. `Renderer::createView(camera)` registers one and `viewImage` hands back
what it drew. The seam was exactly where the card said: `composeImage`/`composeView` gained a
third answer — `view.destination` first, then `presentTarget`, then the swapchain — and no
pass learned about views at all.

**The card's "expected to be wrong about" was right, and it is the number worth carrying.**
Serial reuse does survive the barriers, but not for free: **one view 3.237 ms, two views
7.287 ms — 2.25x, not 2.00x.** Two things account for the 0.28: the barrier between chains is
a full stall by construction, and the second chain's phase-0 occlusion guess starts from the
*other* view's visibility buffer, so its phase 1 admits more than a private one would.
Nothing is ever wrongly dropped — phase 1 tests against this view's own depth — it is a worse
first guess and nothing more. A private visibility buffer per view is the fix if a caller
ever cares; it is a buffer, not a target set, so it is cheap.

**The defect the golden set caught is the one worth recording, because the reasoning that
produced it was plausible.** Moving the whole chain into `recordViewChain` moved `recordCull`
with it — and `recordShadows`/`recordPunctualShadows` draw straight out of cull lists 1..25.
With the cull now recorded *after* them, both shadow passes drew **last frame's command
lists**. It is a one-frame staleness in a shadow map, in a scene where the camera is
stationary at frame 60, and `no-rt` was the only case that could see it: every other case
traces shadows instead of rasterising them. `1 of 11 cases differ` on a suite of eleven, from
a change that compiled clean and looked like a pure extraction.

The fix is the split the code now states: `CullViews::Scene` fills every list once, before
the shadow passes; `CullViews::Camera` re-fills list 0 for a chain, and is **not recorded at
all in a one-view frame** — which is why `Cull` in the trace still means what it meant and
the published baseline column did not have to move.

Three decisions the card left open, and what they resolved to:

- **A view renders at the primary's extent.** Serial reuse of one target set means one
  extent, by construction. Its destination is that size and a resize rebuilds it via
  `createViewTargets`, which exists because there were four call sites that had to do both
  halves and a fifth that did only the first would leave a mirror sampling a destroyed image.
- **The light ranking and the shadow atlas are the primary's**, copied into each secondary
  view's blocks rather than recomputed. `updateLights` ranks against a camera and
  `recordPunctualShadows` renders that assignment once for the frame; a second ranking would
  put matrices in a view's buffer describing layers the atlas does not hold, and the view
  would sample the wrong light's depth. This is C31's own argument arriving one level down.
- **`kCullViews` did not have to grow**, which C32's card handed to this row expecting it
  would. Serial reuse means each chain re-culls into list 0 rather than owning a list of its
  own, so the 27th entry the C25 note asked for is unnecessary — and the one-view frame keeps
  exactly 26 dispatches. The TAA jitter phase also needed nothing: `View::taaActive` is false
  for a secondary view, so it is neither jittered nor resolved.

## Verification results

- `scripts/golden.sh check release` — **all 11 cases match**, byte-identical, exit 0. Two
  runs hit `vkCreateDevice failed: VK_ERROR_DEVICE_LOST` before rendering anything and were
  re-run once, as the protocol says to; both then passed. Worth noting that
  `bug-a-golden-case-can-fail-because-the-binary-vanished` paid for itself here — both were
  reported as `HARNESS`, stopped the suite and kept their logs, instead of being counted as
  differing cases.
- `validation`: **0 validation errors**, two chains recorded into one command buffer, 300
  frames with layers on and `--resize-every 60` — five swapchain rebuilds, so five round
  trips through `createViewTargets`/`destroyViewTargets` with a live secondary view and its
  destination image. Layout transitions between the two chains are what this was really
  checking and they are clean.
- `trace`, `scripts/baseline.py --samples 4 --runs 3`, both arms from the **same binary**:
  one view `GPU frame` **3.237**, `wall` 3.298, `CPU busy` 0.148, VRAM 505.0; two views
  `GPU frame` **7.287**, `wall` 7.359, `CPU busy` 0.267, VRAM 511.3. The per-pass zones are
  unmoved either way (`GBuffer` 0.481, `Lighting` 1.849/1.851, `SSAO` 0.152/0.151). The
  one-view frame did not get slower for holding the machinery for two — 3.237 against 3.202
  published, inside the documented spread. VRAM +6.3 MiB is one 1600x900 RGBA8 destination
  (5.76 MiB) and its allocation overhead, which is the design's claim about marginal cost
  stated as a measurement.
- `./test.sh debug` — **1007 tests, 103 suites, all passed**.
- The two-view arm was driven by a temporary probe calling `createView` from `drawFrame`
  behind an environment variable. It is not in the tree: C34 is the caller, and a switch that
  existed only to measure this row would outlive its reason.

## Deferred

- A **private visibility buffer per view** would remove most of the 0.28 ms over a clean
  doubling. Not opened as a card: it is one buffer per view and a line in `recordCull`, but
  nothing has yet paid the 2.25x to complain about it, and a measurement with no caller is
  the thing this project's ordering argument warns about. The number is recorded in
  [rendering.md](../../architecture/rendering.md) so the next reader of a two-view frame time
  finds the explanation rather than re-deriving it.
