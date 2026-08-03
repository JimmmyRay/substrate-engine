---
id: P1
title: The image table
arc: P
size: M-L
verification: golden-11, tests-hosted, validation
---

# P1 — The image table

`gfx::ImageTable`: a growable bindless array, a free list, a generation, `ImageId = Handle<struct ImageTag>`, `load`/`destroy`. Owned by `Engine`. The overlay becomes its first caller with behaviour unchanged and `kMaxOverlayImages` deleted. **Everything below waits on this**

## An image is a handle, and it can go away

```cpp
void MyGame::init(Engine& e) {
    hero  = e.images().load("res:/sprites/hero.png");
    tiles = e.images().load("res:/sprites/dungeon.png");
}

void MyGame::shutdown(Engine& e) {
    e.images().destroy(hero);      // the same verb pair as every other subsystem (C1)
}
```

`ImageId` is `Handle<struct ImageTag>`, so it carries a generation and an image handle passed
where a body handle belongs does not compile. The overlay's `loadImage` becomes a call to the
same table, and `ui::Context::image` takes an `ImageId` instead of a `uint32_t` slot.

## P1's scope, and the rung it stops at

The [scope table in CLAUDE.md](../../../CLAUDE.md) promotes by callers, never by count, and stopping
at the right rung is most of this row's design.

`Renderer::overlayImages` (`Renderer.h`, in the overlay block beside `overlaySetLayout`) is a
private member with one caller, which is exactly where it belonged when the overlay was the only
thing that wanted images. A sprite pass is a second caller that cannot reach it there. That
promotes it **one** rung — to a type the engine owns and hands out by reference, alongside
`instances()`, `particles()` and `physics()`.

*(The line numbers this card carried — `Renderer.h:1677-1680` and `:1680` — had drifted by
about 140 lines before the row was started; the real sites were `:807` for
`kMaxOverlayImages` and `:1822` for `overlayImages`. They are named rather than numbered
now, because a line number in a card is a fact with an expiry date and this one expired
without anybody noticing.)*

**It does not promote two rungs.** `GltfScene`'s array has no caller outside its own file, so
nothing about sprites requires unifying the two. That unification may well be right later; it
is not this row, and doing it here would be over-promotion of the kind `principles.md` names.
What P1 *does* owe the scene's array is the generation it lacks — but as a
declined row with a trigger, not as scope creep.

## Why P1 is not the asset manager that was refused

[CLAUDE.md](../../../CLAUDE.md) refuses "a `ResourceManager` / `TextureCache` owning GPU resources
behind handles" by name, and P1 is close enough to it that the difference has to be written
down rather than assumed.

What P1 is: a `std::vector<GpuImage>`, a free list, a generation counter, and one descriptor
array. Those are the same four things `Renderer` holds today in its overlay block and the same
four `GltfScene` holds, moved to where a second caller can reach them.

What it must **not** grow, each of which is the actual refused thing:

- **Reference counting.** `destroy` is explicit, under C1's Rule 3. A refcount is precisely what
  replaces an explicit destroy with an implicit one, and the C arc already declined it for
  C10 on that basis.
- **A path-keyed cache.** Loading the same file twice loads it twice. Deduplication is a cache,
  a cache needs invalidation, and invalidation is the system this is not.
- **A virtual `IImageLoader`.** One function, `stbi_load`, plus the KTX2 path that already
  exists.
- **Eviction, residency policy, or an async queue.** Loading is synchronous, as `loadImage` is
  today. Streaming is C10 and stays there.

If a later change adds any of the four, it has become the refused thing and the row that adds it
owes a new argument. That is a sharper test than a size limit, and it is the one to review
against.

## P1's one hard part, and the shape this card got wrong

**A variable-count descriptor set cannot grow in place.** Growth means allocating a new set at
the next power of two, rewriting every live slot, and swapping. That can only land where no
in-flight command buffer still references the old set.

~~So it defers by frames-in-flight, which is exactly the shape C1 landed for physics bodies and
audio voices, and for the same reason.~~ **That is the wrong precedent, and the tree had already
answered this question the other way before the card was written.**

`Renderer::ensureInstanceCapacity` grows a descriptor-referenced GPU array and calls
`vkDeviceWaitIdle`, with a comment stating in as many words that *there is no per-frame
retirement list in this engine and adding one to serve an event that happens at load time would
be machinery for nothing*. C10's `appendModel`/`unloadModel` took the same trade. Two cases, one
answer, and it is not deferral.

**C1's deferred reclaim is a different thing, and the distinction is what the card missed.** It
defers to the next `step()`/`update()` because Jolt will not have a body removed from under a
step and `ma_sound_uninit` walks a graph the device thread is also walking. That is a
*dependency's* re-entrancy rule, not a GPU descriptor's lifetime. Reading the two as the same
shape is how a row ends up building the machinery its neighbours declined.

So: **the device wait**, and the argument for it rather than a retirement list belongs on
`syncImages`'s declaration. A third case would be the Rule of Threes trigger for writing one,
and images are not a third case — the sketch at the top of this card loads in `Game::init` and
destroys in `Game::shutdown`, which is the same load-time event the other two serve. The trigger
to record is a caller that loads images *per frame during play*, and that caller is streaming,
which is C10 and stays there.

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- `scripts/golden.sh` -- ~~twelve~~ **eleven** cases, byte-identical. (`no-ibl` was retired for
  pinning a copy of `lit`; the card was written while the count was twelve.)
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- Zero validation errors with layers on, in every capture.
- Hosted tests for the lifetime rules: acquire, release, reuse of a released slot, and a
  released handle refused.

## Reference

[architecture/rendering.md](../../architecture/rendering.md), [architecture/systems.md](../../architecture/systems.md).

## Outcome

**What landed.** `gfx::ImageTable` (`engine/gfx/ImageTable.{h,cpp}`), owned by `Engine` and
reached as `e.images()`, with `load`/`destroy`, `ImageId = Handle<struct ImageTag>`, a free
list and a generation per slot. `Renderer::loadImage` and `Renderer::kNoOverlayImage` are
gone; so is `kMaxOverlayImages`. `ui::Context::image` takes an `ImageId` and is the one place
a stale one is refused. The demo is the caller outside the suite for both verbs.

**The estimate held at M-L, and the surprise was where the work was.** Not the descriptor
growth — that was thirty lines once `VARIABLE_DESCRIPTOR_COUNT` was the answer — but deciding
what the table is allowed to *hold*.

**The table holds no `VkImage`, and that was forced by the verification rather than chosen for
elegance.** The row's own `tests-hosted` line cannot be satisfied by a class that includes
`vulkan.h`: `substrate_tests` links `SUBSTRATE_HOSTED_SOURCES` and neither links the loader
nor sees the headers. So the split is the one `SceneData` already draws against `GltfScene`,
at a smaller scale — the table is the lifetime, the renderer is the residency, and
`Renderer::syncImages()` reconciles the second to the first against `ImageTable::revision()`
at the top of `drawFrame`, exactly as `InstanceTable` is reconciled to the instance buffers.
This is worth recording because the split reads as indirection until you notice what it buys:
the failure this row exists to prevent is *a slot handed to two holders*, which is a vector
and an integer, and it is now provable without a device. `limitations.md` had recorded the
identical free list in `GltfScene` as untested for exactly as long as it was inseparable from
one.

One consequence, stated so it is not read later as a bug: a file that resolves but is not a
decodable image is reported by the renderer at the next frame rather than by `load` at the
call site, and its slot keeps the fallback. A name that resolves to *nothing* — the failure a
game actually hits, an asset that was never fetched — still fails at the call site, because
`core::Resources` is hosted and the table runs it.

**The retirement-list decision: the device wait, and the card's own reasoning was wrong.**
Argued in full in "P1's one hard part" above and summarised on `Renderer::syncImages`'s
declaration. The short form: `ensureInstanceCapacity` and C10's `unloadModel` had both already
answered this with `vkDeviceWaitIdle`, and C1's deferred reclaim is not the same shape —
it defers because Jolt and miniaudio forbid removal mid-step, which is a dependency's
re-entrancy rule rather than a descriptor's lifetime. A third case would be the Rule of Threes
trigger for writing a retirement list; images are not one, because they arrive on the same
load-time event the other two do. **Trigger recorded for the next row that asks: a caller that
loads or frees images per frame during play. That is streaming, and it is C10.** One
`vkDeviceWaitIdle` covers the whole reconcile — both the image about to be freed and the
descriptor about to be rewritten — and it happens only on a frame where the revision moved.

**`kMaxOverlayImages` is deleted, not raised, and nothing replaced it with a bigger number.**
The binding is declared to the smaller of `maxPerStageDescriptorSampledImages` and
`maxDescriptorSetSampledImages`; the *set* is allocated at what is currently needed and
doubles, starting at one slot. Growth destroys the array's own descriptor pool and allocates
a wider set from the same layout, which is why the overlay pipeline is never rebuilt. On this
machine the array grew `1 -> 2` on the demo's single image, which is the mechanism working at
the smallest size it can.

**What was deferred.** The device half of *release* — destroy an image, then resync — has no
live caller: the demo destroys in `Game::shutdown`, after the last frame, and nothing binds a
key to it. It is covered by the hosted tests and by reasoning about a path the load case
already exercises, and P4's sprite pass will be the first thing to run it inside a frame.

**`GltfScene`'s array was not touched**, per the arc's non-overlap rule, and the prediction in
principles.md that P1 would delete `kTextureSlotHeadroom` alongside `kMaxOverlayImages` was
wrong: that constant belongs to the scene's array, which the same rule keeps out of this row.
The discharge table now lists the two residencies separately and the reference says so.
**The trigger for unifying them has moved, though it has not fired.** C10 made
`releaseTextureSlot` reachable from a keypress via `unloadModel`/`LoadedModel::textureSlotsUsed`
rather than only at load, so the generation-less free list is now exercised at runtime by a
game. The letter of the rule still holds — nothing outside `GltfScene.cpp` calls
`acquireTextureSlot` — but the hazard it guards is no longer latent, and `limitations.md` now
records that distinction rather than leaving the trigger reading as untouched.

**Corrections to this card, made before the code was written**: the two line citations had
drifted about 140 lines and are now named rather than numbered; the golden count was twelve
and is eleven; and the frames-in-flight claim above.

**Verification.**

- `scripts/golden.sh check release` — **11 of 11 byte-identical**.
- `./test.sh debug` — **695 passed, 75 suites**. `./test.sh asan` — **695 passed**, clean.
- Nine new hosted cases in `tests/ImageTableTests.cpp` (acquire, release, reuse, a released
  handle refused, a zeroed handle refused, double destroy, capacity refusal and recovery,
  the revision contract, and what the device half reads) plus two in `tests/UiTests.cpp` —
  a destroyed handle drawing the atlas rather than what took its slot, and no table at all
  degrading the same way.
- `./run.sh demo debug -- --frames 60 --locked` with layers on — **zero validation errors**,
  and the log shows `Overlay image array grown: 1 -> 2 slots` then `image 1: res:/ui_test.png
  (64x64)`.
