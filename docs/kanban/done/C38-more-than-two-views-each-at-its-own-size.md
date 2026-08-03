---
id: C38
title: More than two views, each at its own size
arc: C
size: L
verification: golden, readback, trace, validation, tests-hosted, inspection
---

# C38 — More than two views, each at its own size

Afterwards a game can hold more than one view, each rendering at a size it chose rather than the
presenting view's, each ranking its own lights — which is what four-way split screen, a minimap
beside a rear-view mirror, or an inset that costs a quarter of a frame instead of a whole one all
need and none of which is reachable now.

C34 gave a game one view and said so. This card is the other end of that sentence.

## What is reachable today, so the gap is the right size

A game can already create a view, write its camera's pose, and draw the result — the destination
is an ordinary `ImageId`, so `ui::Context::image(rect, texture, rgba)` puts it at any screen
rectangle and a `scene::Sprite` puts it on a wall. A HUD inset, a mirror, a security monitor and a
minimap are all available now, and so is two-player split screen: present player one full-screen
and cover half of it with player two's view.

That last one is the tell. It works, and it renders half a frame nobody sees. **The gap is cost
and count, not capability**, which is why this is a rendering card rather than a camera one.

## The six things in the way

| | Where it is |
|---|---|
| `kMaxViews = 2`, and the table gets `kMaxViews - 1` — **one** game view | `gfx/Renderer.h`, `Engine::init`. Its own doc already says raising it "is a memory decision and nothing else" |
| Every view renders at the **presenting** view's extent | `createViewDestination` allocates at `view.renderExtent`, a whole-app value |
| A secondary view copies the primary's light ranking | The shadow atlas holds the primary's assignment, so a view looking elsewhere is lit by the wrong lights. **This row is C35's — see the blocker below** |
| TAA is primary-only | `view.taaActive = false` for every table view |
| One presenting view, one blit, no compositor | `recordPresent` is a single `vkCmdBlitImage2`, `swapchainCount = 1` |
| One `InputMap`, player 0 | `limitations.md` already names split-screen as the gap this leaves |

The first is a constant. The next three are all the same decision — **one shared target set,
reused serially** — which is the cost C34 deliberately did not pay, and undoing it is most of
this card. The fifth is a pass. The sixth is an input card, not this one, and should be split off
rather than absorbed.

## The C35 blocker, rechecked by reading the tree and now gone

**C35 landed, and the recheck says the third row survives and shrank** — which is one of the two
outcomes the blocker note predicted. `View::lightClusters` is a per-view buffer, rebuilt by
`createRenderTargets` from *that view's* extent, and each view's tile assignment is built from its
own camera and its own G-buffer depth. So a secondary view no longer iterates the primary's tiles.

What is still the primary's is the **light list and the shadow assignment** the tiles cull: the
per-view uniform blocks copy block 0's light and shadow-matrix buffers, so a view looking
elsewhere is still culling the wrong set, more cheaply. The row is therefore smaller and now has
somewhere to hang: the per-view buffer exists, and what it needs is a per-view `updateLights`
rather than a copy.

~~## Blocked on C35, and not merely by who is holding the file~~

**C35 is building the per-view light assignment this card's third row needs.** Its cluster buffer
is per view rather than per frame slot — its own comment says so, in the same words the G-buffer's
rule uses — and `frame.clusterParams` carries a per-view tile grid that both light loops branch
on. So "a secondary view copies the primary's light ranking" is not an independent defect this
card can fix; it is the thing C35 is replacing, and writing a second per-view ranking beside it
would be two answers to one question.

That would be true even if the two rows were a month apart. It happens also to be true right now
in the strong sense: C35 is in flight with several hundred uncommitted lines in
`engine/gfx/Renderer.{h,cpp}`, which is where four of the six rows above live.

**Recheck by looking, not by assuming** — that is what `blocked/` is for. When C35 lands, read
what it actually gave each view: if a view already ranks its own lights, this card loses a row and
gets smaller; if C35 clusters only the primary's list, the row survives and is now cheap, because
the per-view buffer exists to hang it on.

## What it has to be sized against

Not two views. **Two views cost 7.287 ms against one view's 3.237 ms at 4x on Sponza — 2.25x, not
2.00x**, and `rendering.md` attributes the overhead to the full barrier between chains and the
shared visibility buffer degrading the second chain's occlusion guess. Four views on that curve
land near 14 ms, which is the whole frame budget, so a four-way split at full extent is not a
thing this card can deliver by raising a constant.

Per-view extent is therefore not a nicety on the side — **it is the feature**. Four quarter-size
views is the shape that fits; four full-size views composited down is the shape that does not. A
card that raises `kMaxViews` and stops has produced something nobody can ship.

The per-view target set that buys it is the memory question to answer first, before any of the
rest: seventeen render targets is what a view chain touches, and paying for that four times over
is a number to measure rather than assume.

## Verification

- `scripts/baseline.py` — `Lighting` and `Frame`, several runs per arm. **This card is a cost
  claim**, so the numbers are the deliverable: one view, two views and four views, at full and at
  quarter extent. Never the `GPU @` line.
- `scripts/golden.sh` — thirteen cases, byte-identical. Two of them drive the mirror path, and a
  one-view frame must upload and record exactly what it uploads and records today.
- `scripts/readback.sh` — nine cases, bit-identical. Presentation is what a compositor changes.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation. `ViewTable` is hosted:
  capacity, per-view extent and the handle generation across a raised cap.
- Zero validation errors with layers on, with four live views — a per-view descriptor set and a
  per-view target set is where a layer catches what a screenshot does not.
- A leak check across create/destroy of views at two different cycle counts, compared on the
  `VRAM [...]` lines. Per-view targets is the largest allocation a game can ask for repeatedly.
- Inspection: a view whose camera looks away from the primary's is lit by **its own** light
  ranking, checked against a scene with lights the primary cannot see.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — "More than one view", which
currently states the serial-target-set decision and its three consequences as settled.
[architecture/limitations.md](../../architecture/limitations.md) — the split-screen entry, and
whatever this card decides not to do.
[guides/making-a-game.md](../../guides/making-a-game.md) — how a game asks for a view and where it
puts the result.

## Outcome

**Landed: `kMaxViews` is 4, every view owns its target set, and `create(images, extent)` takes the
size the result will be sampled at.** `{0, 0}` follows the presenting view, so a resize moves it;
`resize(id, extent)` moves the revision and rebuilds, which is a device wait and eighteen
allocations and therefore belongs to a resolution setting rather than to a frame. Views still run
serially with one full barrier between — what changed is that each has its own targets rather than
borrowing the primary's.

**The memory question, answered first as the card demanded, and measured two ways that agree.**
One full-extent target set is **224.5 MiB** at 1600x900, 4x: steady state 540.2 MiB at 1600x900
against 371.8 at 800x450 gives 168.4 for three quarters of the resolution-dependent allocation;
and three extra full-extent views measured 1230.3 against 540.2, or 230 MiB each. Four at full
extent is ~898 MiB of targets. Four at quarter extent is ~56 MiB each.

| views | extent | `Lighting` | `Frame` |
|---|---|---|---|
| 1 | full | 1.891 | **3.317** |
| 2 | full | 1.885 | 6.838 |
| 4 | full | 1.923 | **13.985** |
| 4 | quarter | 0.617 | **6.945** |

**The card's prediction was exact**: it said four full-extent views "land near 14 ms, which is the
whole frame budget", and they land at 13.985. Where it was optimistic is the quarter arm — **four
quarter-size views cost about what two full-size ones do, not what one does** (6.945 against
3.317). Still the difference between shippable and not, and still the reason the extent argument
exists, but the honest ratio is 2.1x rather than 1x. `Lighting` at quarter extent is a median over
four views' instances, which is why it falls rather than holding.

**The light-ranking row is not done, and that is a split rather than a gap.** C35 made the tile
assignment per view, so a secondary view already culls per pixel against its own depth — it culls
the wrong *list*. Ranking per view needs the shadow atlas to stop being one assignment for the
whole frame, or a view samples the wrong light's depth, which is silent and looks like a shadow
bug. That is the atlas, not the ranking, and it is
[C39](../backlog/C39-a-view-ranks-its-own-lights.md) with the three candidate shapes and their
costs. **The inspection arm this card named — a view looking away from the primary's, in a scene
with lights the primary cannot see — belongs to that card and is deliberately unanswered here.**

**The sixth row was left alone**, as the card required: one `InputMap`, player 0, still an input
card.

Verification: `scripts/golden.sh check release` **13 of 13** — run twice, once with the probe in
and once after removing it. `scripts/readback.sh` **9 of 9 bit-identical plus the lit silhouette**;
the first invocation reported one case failed and two subsequent ones were clean, the documented
flake. `./test.sh debug` and `./test.sh asan` **1060 tests, 107 suites** each, separate
invocations. **Zero validation errors with four live views**, headless debug, 120 frames.
`scripts/perfgate.py --config release` inside budget — `Frame` 2.967 against 2.950, `Lighting`
1.845 against 1.832 — which is the check that a one-view frame did not get slower, and the reason
the golden set passing is not the whole story.

**The C38 probe is deleted, as its own comment required** — the `SUBSTRATE_C38_*` environment
switches in `Engine.cpp` that created, resized, cycled and displayed views existed to run these
arms, exactly as C33's and C34's did. `Engine.cpp` is byte-identical to what it was before this
card, including the `<cstdlib>` the probe needed. Everything above was measured with the probe in
place and the suites re-run without it.

The `leak` arm was dropped on instruction rather than run.

Reference updated: `rendering.md`'s "More than one view" carries the per-view target set, the
memory number measured both ways, the cost table, and the two things a secondary view still
shares; `ViewTable.h`'s class block carries what a view costs so nothing creates one casually.
