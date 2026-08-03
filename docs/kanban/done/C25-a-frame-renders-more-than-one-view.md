---
id: C25
title: A frame renders more than one view
arc: C
size: XL
verification: golden-11, trace, validation
---

# C25 — A frame renders more than one view

Afterwards a frame can record the pass chain more than once, into a target that is not the
swapchain, and a game can ask for a view with its own viewport, matrices and history.
Today `Renderer::drawFrame(camera)` ([`Renderer.h:379`](../../../engine/gfx/Renderer.h#L379))
is acquire-record-present: it waits the fence, calls `vkAcquireNextImageKHR`, records every
pass, transitions to `PRESENT_SRC` and presents. There is no entry that records without
presenting and no parameter that says where the pixels go.

The state that would have to become per-view is per-frame today. Every render target is a
single named member sized from one `renderExtent` in `createRenderTargets` — one G-buffer set,
one `hdrTarget`, one `ssaoRaw`/`ssaoBlurred`, one `bloomChain`, one `ssrTarget`, one
`taaHistory[2]`. `prevViewProj`, `taaHistoryIndex` and `taaHistoryValid` are `Renderer`
members written at the bottom of `updateUniforms`
([`Renderer.cpp:5502`](../../../engine/gfx/Renderer.cpp#L5502)), so calling `drawFrame` twice
would acquire twice, present twice, and reproject the second view's TAA against the first
view's matrix.

**The culling machinery for this already exists and is already being paid for.**
`kCullViews = 2 + kMaxShadowLayers` is 26 ([`Renderer.h:121`](../../../engine/gfx/Renderer.h#L121)),
`cullViewProj[kCullViews]` is dispatched every frame, and all 26 slots are spent on fixed
roles — view 0 the camera, view 1 the sun, 2..25 the punctual atlas. A game can claim none of
them. That is the sharpest form of the finding: the renderer holds 26 view slots and offers a
game zero.

Without this there is no split-screen, no mirror, no security monitor, no portal, no minimap
that shows the world, no character-select model preview, and no second viewport for an editor.

**This is not a render graph and not a `RenderDevice`.** The pass methods stay methods and
keep recording `vkCmd*` inline; what changes is that they take a view instead of reading one
off the renderer. The prohibition in [CLAUDE.md](../../../CLAUDE.md) is against indirection we
write over Vulkan, not against a struct that says which viewport a pass is recording for.

Expected to be wrong about: the memory cost. Per-view G-buffers and TAA history at full
resolution is most of the frame's footprint doubled, so this likely needs views to declare a
scale, and the split-screen case may want to share targets by rendering serially into one set
rather than allocating two.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. Every golden case is one view, so a
  moved pixel means the single-view path changed rather than gained a sibling.
- Per-pass cost from `scripts/baseline.py`, several runs per arm, quoting `Lighting` and
  `Frame`. A one-view frame must not get slower for holding the machinery for two.
- Zero validation errors with layers on, including a capture with two views recorded.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — the frame structure and the
render-target table, both of which describe one view. Note the table is already stale on
cascade and atlas counts.

## Outcome

**Split, not built.** Nothing in `engine/gfx/` changed for this card. It is in `done/` because
the row is finished as a *row* — it has been grounded, decided and replaced — and leaving it in
`backlog/` beside its own four successors would be the arc overlap the groomer exists to find.
Read this as a record of a decision, not of a landing.

The grounding is what decided it. `Renderer.cpp` is 7,240 lines with 169 `vkCmd` call sites
across 20 entry points, `Renderer.h` is 2,033; there are 24 `record*` pass methods, and the
state the card said would have to become per-view is spread across sixteen `GpuImage`
declarations, roughly thirty companion members, six `FrameSync` fields and four scattered TAA
variables. That is not one card, and a half-converted renderer verified by a byte-exact golden
set is worse than an unconverted one.

Three findings worth keeping, because each one makes a successor smaller than the card feared:

- **The swapchain seam is at `Renderer.cpp:7069`.** Everything from `:6982` to `:7068` — every
  pass from instance upload to TAA — touches no swapchain object whatsoever. `recordTonemap` is
  the first consumer of `imageIndex`, and it reaches it through `composeImage`/`composeView`,
  a single accessor pair with four call sites that *already* has a non-swapchain branch for
  `presentTarget`. Recording without presenting is a third answer from that pair, not a rewrite.
- **`createRenderTargets` is already parameterised on an extent.** It reads three external
  inputs — `virtualExtent`, `swap.extent`/`swap.format`, `msaaSamples` — and everything after
  `:1352` uses a *local* `extent` rather than the member. Handing it a view and a size is a
  signature change with almost no body change behind it.
- **Per-view geometry plumbing exists and is in use.** `drawSceneIndirect(cmd, slot, view,
  pass)` and `viewCommandOffset(view)` already index by view; `recordGBuffer`, `recordShadows`
  and `recordPunctualShadows` all pass one, and `recordCull` already changes behaviour on
  `v == 0` for LOD. A second camera view is a 27th list, not a second set of 26 — because
  slots 1 and 2..25 are the sun and the punctual atlas and are not per camera view at all.

And one that makes the shape clearer: **the six resolution-independent images stay on
`Renderer`.** `envCube`, `irradianceCube`, `prefilteredCube`, `brdfLut`, `shadowMap` and
`punctualShadowMap` are scene properties, not view properties, and a `View` that swallowed the
shadow atlas would re-render 24 layers per view for nothing.

The successors, in the order they have to land:

| | | |
|---|---|---|
| [C31](C31-a-view-owns-the-targets-it-draws-into.md) | A view owns the targets it draws into | L |
| [C32](C32-uniforms-are-per-view-not-per-frame.md) | Uniforms are per view not per frame | M |
| [C33](C33-a-pass-chain-records-without-presenting.md) | A pass chain records without presenting | L |
| [C34](C34-a-game-asks-for-a-view.md) | A game asks for a view | M |

C33 carries the decision the card was uncertain about. C25 expected to be wrong about the
memory cost and guessed that views might share targets by rendering serially rather than
allocating two sets — that guess is now the plan, and the cost of it is one destination image
per view instead of seventeen, paid for with TAA disabled on non-primary views and a full
chain of GPU work per view.
