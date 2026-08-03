---
id: C31
title: A view owns the targets it draws into
arc: C
size: L
verification: golden-11, validation, inspection
---

# C31 — A view owns the targets it draws into

First of the four C25 was split into, and the one the other three stand on. Afterwards the
seventeen images a frame draws into are members of a `View` rather than of `Renderer`, and
`createRenderTargets` takes a view and an extent instead of reading `renderExtent` off the
object. **Nothing renders twice yet** — this is the container, and it is worth landing alone
because it is the one slice a byte-identical golden set can fully police.

The count, as it stands: sixteen `GpuImage` declarations sized from `renderExtent`
(seventeen images — `taaHistory` is two), plus roughly thirty companion members that have to
move with them or they will name images that no longer exist. `destroyRenderTargets`
([`Renderer.cpp:2434-2528`](../../../engine/gfx/Renderer.cpp#L2434)) is the authoritative
list: it is the exact inverse and nothing else in the file enumerates the set.

| Sized from `renderExtent` | `gAlbedo` `gNormal` `gOrm` `gEmissive` `gDepth` `gDepthResolved` `hdrTarget` `presentTarget` `ssaoRaw` `ssaoBlurred` `bloomChain` `depthPyramid` `ssrTarget` `fogTarget` `taaHistory[2]` `velocityTarget` |
| Not sized from it, and staying on `Renderer` | `envCube` `irradianceCube` `prefilteredCube` `brdfLut` `shadowMap` `punctualShadowMap` |

The six that stay are the reason this is a real boundary rather than a rename: the IBL bake
and both shadow maps are scene properties, not view properties, and a second view shares
every one of them. A `View` that swallowed the shadow atlas would re-render 24 layers per
view for no reason.

**`createRenderTargets` is already parameterised and nobody noticed.** It reads exactly three
external inputs — `virtualExtent`, `swap.extent`/`swap.format`, and `msaaSamples` — and
everything after [`Renderer.cpp:1352`](../../../engine/gfx/Renderer.cpp#L1352) uses a *local*
`extent`, not the member. So the signature change is real and the body barely moves.

Expected to be wrong about: whether `presentTarget` belongs in the view or beside it. It is
the swapchain-shaped image the compose step writes when a virtual resolution is set, and
C33's offscreen entry may want exactly that field for a different reason — in which case one
of the two is misnamed.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. A pure move of members has no other
  honest gate, and it is a complete one: every sized target is on the path all eleven take.
- Zero validation errors with layers on. Descriptor sets are written against these views in
  `createRenderTargets`; a set left pointing at a destroyed image is exactly what the layers
  catch and what a moved-member refactor gets wrong.
- `inspection`: `destroyRenderTargets` and the `View` declaration must be the same list. If
  they can drift, this row did not finish.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — the render-target table, which
is already stale on cascade and atlas counts and should be corrected in the same pass.

## Outcome

`Renderer::View` holds **47 members** — the seventeen images, plus the storage views,
descriptor sets, derived extents and the two TAA scalars that would name images that no
longer exist if they moved without them. `createRenderTargets(View&, VkExtent2D)` and
`destroyRenderTargets(View&)` take it; `primaryViewExtent()` is the one place the
`virtualExtent`-or-window rule now lives, since every caller needed it once the function
stopped reading it off the object. The count in the card was seventeen images and "roughly
thirty companions", and thirty was right to within one.

**The card said the body barely moves and it was right — the *call sites* were the row.**
445 references across a 7,240-line file, and the header block they were declared in was
interleaved with pipelines, layouts and samplers that stay. What made it tractable was
refusing a blind substitution: the render targets carry their member names into RenderDoc as
**debug-name string literals** (`"gAlbedo"`, `"taaHistory0"`), and the prose around them
names the member rather than the expression, so a word-boundary `sed` would have corrupted
both. A comment- and string-aware renamer did the 394 code-only substitutions in the .cpp
and left every literal and every comment identical — checked by diffing the set of string
literals before and after — with the header moved by hand and the compiler as the gate. **It
built clean on the first attempt**, which is the result that argues for the tool rather than
for the regex.

Three things the card did not predict:

- **Three loop variables were named `view`**, which would have shadowed the member inside
  the very teardown that names it. `for (auto& view : bloomStorageViews)` puts the loop
  variable in scope for its own range-expression, so the qualified form would not have
  compiled — or worse, would have compiled against the wrong thing. They are `storage` and
  `layer` now.
- **`View` had to be forward-declared.** A nested class named in a member function's
  *parameter list* must be declared before that declaration, unlike one named in a body.
- **`inspection` was the check that found something**, which the card half-expected: it
  asked whether `destroyRenderTargets` and the declaration could drift, and one member
  already had. `hizSet` — the cull dispatch's sampled view of the depth pyramid — was
  allocated in `createRenderTargets` under an `if (== VK_NULL_HANDLE)` guard and released by
  *nothing*. One view got away with it because the pool outlives the images; a second View
  would allocate its own on first use and leak it on every teardown, which is precisely the
  failure the neighbouring comment records ("the pool ran dry on the sixth resize"). It is
  freed with the pyramid it names now, and the guard reads as intended rather than by
  accident.

**On `presentTarget`, which the card expected to be wrong about**: it is in the View, and so
is `presentPlan`. Both are functions of *this* view's extent against the window, which is
what `createRenderTargets` computes from its new `extent` parameter — so the row's own
signature change answered the question. C33 may still want a differently-named field for an
offscreen destination; that is a different image from "where the tonemap lands when the blit
is not an identity", and nothing here has to move for it.

## Verification results

- `scripts/golden.sh check release` — **all 11 cases match**, byte-identical, exit 0. This
  is the whole gate on a pure move and it is a complete one.
- `validation`: **0 validation errors**, release, 240 frames with layers on. And **0 across a
  resize soak** — 480 frames, `--resize-every 20`, 24 swapchain recreations, which is 24
  round trips through `destroyRenderTargets`/`createRenderTargets` including the `hizSet`
  free and reallocation. A debug build (layers on by default, `verifyShaderBindings` live)
  ran 180 frames with resizes: **0 errors, no binding assertion**.
- `--sync-validation` reports its usual large constant set of `SYNC-HAZARD-READ_AFTER_WRITE`
  entries. Not read as a result:
  [tooling.md](../../architecture/tooling.md#synchronization-validation) already records that
  the SDK on this machine over-reports these, and standard validation is the token the card
  names.
- `inspection`: the `View` declaration and `destroyRenderTargets` now enumerate the same
  resources. The value members that own no device object — `renderExtent`, `presentPlan`,
  `ssaoExtent`, `depthPyramidExtent`, `depthPyramidLevels`, `taaHistoryIndex` — are
  overwritten by `createRenderTargets` rather than released, which is the intended asymmetry
  and is stated so the next reader does not "fix" it.

## Deferred

- [doc-the-shadow-section-describes-cascades-the-engine-no-longer-has](../backlog/doc-the-shadow-section-describes-cascades-the-engine-no-longer-has.md).
  The card predicted the render-target table would be stale on cascade and atlas counts and
  it was — the sun map is **one** 4096-square layer and the atlas is **24** layers, against
  the table's four 2048 cascades and eight. That table is corrected here. What is *not*
  corrected here is that `rendering.md` still has a twenty-line **Cascades** section
  describing the retired design as current, plus three more stale counts; correcting the
  shadow chapter inside a row whose gate is a byte-identical golden set would have made this
  row's result harder to attribute.
