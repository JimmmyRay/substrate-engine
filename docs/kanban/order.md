# Recommended order

The one thing the board cannot express. Columns are directories and directories have no
sequence, so *what state a row is in* lives on the card and *what to do next* lives here —
along with the argument for it, which is the part worth keeping.

## Recommended order — the C and D rows

**This section used to schedule the D rows behind the C arc. It no longer does, and the
reason is a distinction the first version did not draw.**

A D row is not one thing. Every one of them contains a **decision** — what a handle looks
like, what the verbs are, what a namespace means, what a sentinel is called, which helper is
the one to call — and a **retrofit**, which is applying that decision to code already
written. The two have opposite cost curves:

> A decision is cheap while it is still a decision and expensive once five subsystems have
> been written without it. A retrofit of code that is not growing costs the same whenever it
> is done.

Deferring a decision therefore does not postpone its cost; it multiplies the surface the cost
is eventually paid over. This document already contains the proof twice, found by audit
rather than predicted: the `extras` prologue that
has already diverged in one of three copies, and
[`Logger::vformat`](../../engine/core/Logger.h#L99), an extraction that **exists and is not
called** by three sites that each hand-rolled a truncating buffer instead. Neither is a
consistency complaint. Both are what an uncleaned tree teaches the next author, and the next
author here is every capability row in Part 1.

So the order is by cost curve rather than by arc.

### Stage 0 — the decisions, and the checks that make later work provable — **landed**

Nothing below this line is large. All of it is cheaper now than at any later point.

1. ~~**D7 first, and the reason is not its size.**~~ **Done.** It is the row that makes every
   other row's verification mean something: a suite compiled to different flags than the
   engine, and a `scripts/test.sh releas` that runs debug while reporting nothing, both produce
   results that look exactly like results. `substrate_tests` now takes `${SUBSTRATE_WARNINGS}`
   rather than a retyped subset; `test.sh` has the catch-all `build.sh` always had; `run.sh`
   checks for Sponza only on the path that opens it, so a game with its own scene runs without
   it; and `usage()`, `list_games()`, the game-argument handling and the sanitizer environment
   moved into [`scripts/common.sh`](../../scripts/common.sh), which the five root scripts source.
2. ~~**The decision halves of D1, D2 and D3 — not their sweeps.**~~ **Done**, as
   [principles.md §8](../architecture/principles.md#8-one-vocabulary-for-the-engines-surface):
   the lifetime and bring-up verbs, the `init` return shape, the predicate form, where
   `[[nodiscard]]` goes, the namespace rule and the three sentinel rules. It is a section
   rather than a diff, which is the point — **C1 is now written against a convention rather
   than inventing one**, and D1's and D2's sweeps are Stage 2 with nothing to decide left in
   them. One corollary is worth repeating here because it changes what D2 costs: where the
   namespace is right and the directory is wrong, the fix is to move the file.
3. ~~**D5 and D6, which are defects rather than conventions.**~~ **Done.** The three
   truncating buffers now call `Logger::vformat` — including the one in `Physics.cpp` that
   truncated and then handed the result to `Logger::debug`. The nine `AudioEngine` and three
   `PhysicsWorld` accessors that indexed unchecked are bounds-checked, and `ProfilerConfig`'s
   defaults now match the settings table's in every field but `outputFile`, whose difference
   is deliberate and documented. Three new test cases pin all of it: that the value
   `addBody`/`addSource` return on failure is safe to ask every accessor about, and that the
   two profiler configurations cannot drift apart again.

**What Stage 0 did not do**, so the next reader does not go looking: no `[[nodiscard]]`
sweep, no namespace or file moves, no respelling of `addBody`. Those are the retrofit halves
and they are Stage 2.

### Stage 1 — the capability rows, written in the conventions above

1. **C1 first, and alone.** It touches five subsystems and adds nothing, which is precisely
   what makes it verifiable — see below. Landing it beside a capability row would forfeit that.

   **The handle type landed ahead of the subsystems, and it earned its keep immediately.**
   `Handle<Tag>` reserves generation 0 for "never issued" rather than using an index
   sentinel, so that a zeroed handle — memset, default-constructed into an aggregate, copied
   out of a resized vector — is invalid instead of reading as a live handle to slot 0, which
   is the first slot every subsystem hands out. Making that true meant `InstanceTable`'s
   first generation had to become 1, and **the golden suite's `physics` case failed on the
   spot**: `Engine.cpp` was hand-building `InstanceId{slot, 0}` for every placement, a
   handle that validated only because it happened to agree with how `create()` numbered a
   fresh slot. It now asks `idAt(slot)`, which is what that method is for.

   Worth recording because it is the argument for the whole row in miniature: the bug was
   not that a generation was wrong, it was that a caller could construct a handle at all.
   The four subsystems still handing out bare `uint32_t` have no such tripwire.

   **Physics and audio then landed the same shape**, and each needed a deferred reclaim for
   its own reason: Jolt will not have a body removed from under a step, and `ma_sound_uninit`
   walks a node graph the device thread is also walking. So `destroy` moves the generation
   immediately — the handle a caller holds goes stale on the call it made — and the slot only
   reaches the free list at the next `step()` or `update()`. Two behaviour changes fell out
   and are tested: `createBody` now **refuses** a `Character` motion instead of routing it,
   and the voice budget now bounds *live* sources rather than lifetime totals, which is what
   it always meant and could not previously distinguish.

   **`SceneAnimator` was not the same job as the other three, and the decision it forced is
   recorded here because the code alone does not explain it.** A character's index is not CPU-only: it is written into
   `GpuInstance::meta.w`, and `jointBase`/`weightBase` pack every character's joint matrices
   contiguously into one buffer the skinning dispatch indexes. So destroying a character
   raises two questions the other subsystems did not have — what happens to the hole in the
   joint buffer, and what happens to instances whose `meta.w` still names the slot. A handle
   cannot make the second a compile error, because the value that crosses to the GPU is a
   bare index by construction. Leaving holes is the answer consistent with the rest of the
   design. **What landed: a dead character keeps its joint block, filled with identity, and
   keeps its base forever.** The two alternatives both alias -- repacking the prefix sums
   moves every later character's matrices under any instance still naming this one, and
   handing the block to a different skin points it at a different skeleton. So a slot is
   reused only by a skin whose joint count fits its block, and a stale `meta.w` draws a bind
   pose rather than another character's animation. Four tests pin it, and they had to,
   because the golden set cannot: its one skinned case is a single character at a fixed pose,
   so a joint-packing bug moves no pixel.

   The same conversion also paid **D3's live defect**: `findClip` used to return
   `kNoCharacter`, a character-index sentinel, and it worked only because every sentinel in
   the engine is the same number. It returns `kNoClip` now, and `skinOf` returns `kNoSkin`.
2. ~~**C2 can be pulled forward past anything**, including ahead of C1.~~ **Done, and pulled
   forward exactly that way.** It depended on nothing, and more downstream work was waiting on
   it than on any other single item. Nine test cases, and the golden set is byte-identical
   across all twelve — which it had to be, because the only existing caller of the query
   surface is audio occlusion and `Audio.h` states that nothing in that class can change a
   frame. **The reverse-direction lookup it needed is worth knowing about before C1**: a
   `BodyID`-to-index map now lives in `PhysicsWorld::Impl`, because reporting a hit in the
   caller's terms is otherwise a scan of every body per hit, and C1's free list will have to
   keep it in step when a body is destroyed.
3. **C8 can be pulled forward past anything.** It is independent, it is the cheapest frame-time
   win available, and it sharpens a trigger that is currently guesswork.
4. **C13 goes first within the load path, and alone.** C14 and C15 are ordered by what it
   measures. It does not gate whether the bake happens —
   that is settled by shipping — it decides how much
   of C15 is worth writing first, and it becomes the standing instrument that catches a
   load-time regression once the asset count is in the thousands.

**The reversal this stage encodes.** The previous version of this section said *"D1 follows
C1, or the two collide in five files."* The collision was real and the resolution was wrong:
ordering the sweep after the row means C1 writes five subsystems in whatever convention
happens to exist, and D1 then rewrites all five. Deciding the vocabulary in Stage 0 and
writing C1 in it lands them correct once. **D1's remaining scope is the retrofit of code C1
does not touch**, which is Stage 2 and collides with nothing.

### Stage 2 — the retrofits

Sequenced by whether the code underneath them is growing, which is the only thing that makes
a retrofit compound:

1. **D4 goes first among these, because `Renderer.cpp` is the file that grows.** Every pass
   added spells another descriptor-set list twice and another dispatch round-up by hand, so
   this is a retrofit with a decision buried in it. It touches one file, adds no capability,
   and the golden set plus a clean validation layer is a complete check on it.
2. **D8 next, for the same reason at smaller scale** — every new screen-space pass writes a
   fifth `worldFromDepth`. Its verification is the strongest in the arc, and it should wait for
   any branch currently editing the shaders it unifies.
3. ~~**D1's sweep and D2's thirteen headers last.**~~ **Both landed, in that order.** These
   were the only rows in the document whose cost genuinely does not compound: `[[nodiscard]]`
   on thirty existing getters is the same edit in two years as today. Doing them last was
   right for a second reason the ordering did not predict — D1's sweep renamed things D2's
   sweep then had to qualify, and running them the other way round would have touched most of
   those call sites twice. D2 cost more than its S said: the decision was small, the retrofit
   was ~1,400 call sites, and the compiler drove it (each round's errors named the exact
   line and the exact namespace, which is a check no grep gives you).

### The C and D rows against the other arcs

**G3 gates C6 and C9, and G4 gates C10.** Phase 1 needs neither, so this document's first phase
and that document's G1b-through-G2 can proceed in either order or at the same time. Phase 2
cannot complete before G3.

---
## Recommended order — the G rows

1. **G1**, and it is first for a reason beyond dependency: it is the only stage whose
   correctness the golden image set can fully prove, because it is pure motion. Everything
   after it is verified against a baseline G1 establishes.
2. **G1b**, immediately after, so the boundary acquires its second consumer while G1 is
   still fresh. A boundary with one consumer is a guess.
3. **G2**, before the scene work rather than after, because it *deletes* the 34 assignments
   rather than moving them. Doing G3 first means rewriting them once to move them and once
   to remove them.
4. **G3**, the headline capability, and the largest behavioural change in the arc.
5. **G4**, then **G5** with **G5b**. G5 needs G4's mutable materials to have anything to
   select a variant with.
6. **G6** once G3 has nodes; it is small and can slot in anywhere after that.
7. **G7** is orderable independently of all of the above — it touches physics and audio and
   nothing this arc restructures. It was listed last while it was the last row, because it is
   the only one here that is not about the *shape* of the API; it is also the row
   `limitations.md` argues loudest for. **G9 is a game that needs contacts, so G7 is now
   pulled forward without ceremony** — ahead of G6, and beside G5 if that is convenient.
8. **G8** any time after G1, and it is fifteen minutes of work that unblocks a control
   scheme. Doing it late means writing G9's input handling twice.
9. **G9 last, and it is the only row that has to be.** Every row above it is a thing a game
   could use; this is the game using them. Landing it earlier would mean building the scene
   against whichever half of the API existed at the time and then rebuilding it, which is
   the cost the arc's own ordering argument is about.

**G9 is also where the arc is judged.** The stages before it are verified by the golden set
and the unit suite, which prove that nothing broke. Nothing in that check can tell you
whether the API is any good to write against — only writing against it can, and this is the
row that does.

**The two stages that add nothing come first, and that is the argument most likely to be
abandoned under impatience.** It is worth recording why it should not be: an earlier
ordering argument in this project said that a regression suite is worth less than the
guarantee that the thing it guards does not drop input on the floor — and then the work was
done in the other order, with the result that a defect fix had to be checked against goldens
snapped *after* the defect. Re-snapping was the only available move. The ordering here has
the same shape, and the same cost is available if it is ignored.

---
## Recommended order — the P rows

By cost curve rather than by phase, which is the argument the C arc's own ordering section
runs on.

1. **P3 can be pulled forward past everything, including ahead of P1.** It depends on nothing in
   this arc, it pays a D-arc debt by finishing D8's extraction, and it fixes a real defect in the
   sky path on the way. Its verification is also the strongest available — the perspective path
   must come out byte-identical — which makes it a good row to land while the rest is still being
   argued.
2. ~~**P1 next, and alone.**~~ **Done, and alone as prescribed.** Every other row waited on it,
   and the byte-identical check it was landed alone to protect held: eleven of eleven. The
   promotion was smaller than the argument about it — what cost the row was deciding that the
   table holds no `VkImage`, forced by its own `tests-hosted` line, since the unit suite links
   no Vulkan. **One correction worth carrying forward to every later row here**: the card said
   growth would defer by frames-in-flight "exactly as C1 did", and that read two different
   things as one. C1 defers because Jolt and miniaudio forbid removal mid-step; a descriptor a
   frame in flight may read is answered in this engine by `vkDeviceWaitIdle`, which is what
   `ensureInstanceCapacity` and C10's `unloadModel` both already do. A retirement list is still
   unwritten and its trigger is now stated: a caller that loads or frees images per frame during
   play, which is C10's streaming.
3. ~~**P2 before P4**, per the decision-versus-retrofit argument in Phase 1.~~ **Done, and the
   ordering argument paid.** The sprite pass will be written against a frame that already
   preserves texels rather than one it has to be retrofitted into, and the row's own
   verification is what proves the frame does: `scripts/readback.sh`, five cases, bit-identical,
   with the expected image computed from the source rather than snapped. **One correction to
   carry forward**: the card called `pixelExact` three switches -- jitter, TAA, the tonemap
   curve -- and it is four. The overlay's image sampler is linear, chosen by C5 for icons drawn
   at whatever height a layout gives them, and at 1:1 linear is only *nearly* exact. P4 should
   assume nothing about a filter it did not choose.
4. ~~**P4**, at which point the engine draws 2D.~~ **Done, and the ordering argument paid
   twice.** P2's frame preserved texels before the pass was written, so nothing had to be
   retrofitted; P1's image table meant the pass needed no texture array of its own; P3's
   orthographic camera meant it needed no second camera. The row added one method, one
   hosted class, two shaders and no base class. **Two corrections to carry forward.** The
   card said *one instanced draw per layer* and it is one draw for **all** layers: one blend
   state makes one global sort possible and one global sort makes one draw possible, which
   is `particle.frag`'s own argument arriving at the same answer, and it leaves a layer as
   purely a sort key. And the card's Phase 2 milestone -- *ten thousand unlit sprites with
   the readback still bit-exact* -- was not checkable by the verification the card named,
   because all five readback cases drew through the *overlay*; `--readback-sprite` and two
   more cases are what closed that, and **P8 should expect the same gap**: a tilemap checked
   only against snapped references is the thing this arc exists not to do. *(P8 never needed
   the case: it declined, and its first correction is the one immediately above — one draw for
   all layers is what removed the row's reason to exist.)*
5. ~~**P7 next among Phase 3**, because it is the cheapest and unblocks the most gameplay, and
   because its larger half is a gap a 3D game has too.~~ **Done, and the "larger half" was
   right in a way the card did not predict: the motion API was mostly already decided.** G3
   had shipped `setBodyTransform` for kinematic bodies while writing the scene sweep, so the
   card's `setTransform` was a widening rather than an addition, and the row's real work was
   arguing that one verb should refuse fewer motion types instead of gaining a twin. **One
   correction to carry forward to P5 and P6**: the card's `verification` line named four
   configurations and a validation run, and **not one of those five checks would have failed
   a `freedom` field that was parsed, stored and never handed to the solver.** A count of
   configurations is not a claim about coverage. What closed it is an *equality* — the plane
   coordinate asserted exactly unchanged over 300 steps — plus a control arm that drifts
   under identical treatment. A row here whose feature is a constraint should name the
   property before it names the configurations.
6. **P5 and P6 in either order.**
7. ~~**P8 last, and reconsidered before it is started.**~~ **Done, and the reconsideration
   declined it** — which is why the instruction was worth writing as part of the order rather
   than left to judgement on the day. The row rested on one cost model, *one draw per chunk
   against one per tile*, and P4 had already dissolved it by landing one draw for **all**
   layers at 0.053 ms for ten thousand sprites, with no sort. What remained was chunking,
   which a game's own 1-byte-a-cell grid beats, and an authoring format, which this arc had
   already refused at smaller scale for sprite sheets. The refusal and its two triggers are in
   [limitations.md](../architecture/limitations.md#what-stays-declined-and-its-trigger--the-2d-arc),
   and `tests/TilemapTests.cpp` is the alternative, compiled and run rather than sketched.
   **The correction to carry forward is about ordering rather than about tilemaps**: a row
   scheduled last must have its *justification* re-read against what the arc landed, not only
   its scope. The rows in front of it are the ones most likely to have answered it, and the
   argument that decided this one had been sitting in P4's outcome for two rows.

### The P rows against the other arcs

**Nothing in this arc gates on G3 or G4**, which is worth stating because the C arc's Phase 2
and Phase 3 did and it shaped their ordering. P1 does not need the scene tree; P4 keeps sprites
in their own dense arrays rather than as nodes; P6 needs one quad rather than `createMesh`. So
this document's Phase 1 and Phase 2 can proceed alongside the G arc in either order.

**C10 is the one row that would collide.** It owns streaming and async loading, and P1 owns
synchronous residency underneath it. If C10 lands first, P1 is written against its queue; if P1
lands first, C10 gains an image table to stream into. Either order works and neither should be
attempted at once.

---
