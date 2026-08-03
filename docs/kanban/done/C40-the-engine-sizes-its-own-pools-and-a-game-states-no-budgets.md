---
id: C40
title: The engine sizes its own pools and a game states no budgets
arc: C
size: M-L
verification: golden, tests-hosted, validation, leak
---

# C40 — The engine sizes its own pools and a game states no budgets

Afterwards a body past the physics world's capacity is **created** and a particle past the
pool's is **emitted**: each subsystem grows itself and reports that it did. The four
`GameSetup` budgets stop being ceilings a game is punished for guessing low and become
optional floors that pre-allocate, so a `configure()` that names none of them is the normal
case rather than the one that gets things silently refused.

**Absorbing this is what an engine is for.** Every one of the four numbers is an allocation
strategy belonging to a library or a GPU buffer, surfaced to a game because the subsystem
underneath cannot resize in place. That is a fact about the implementation, not something a
game author can reason about: the right value depends on how many bodies a scene will spawn
in its worst frame, which is exactly the thing nobody knows while writing `configure()`. The
present policy -- refuse, count, report -- makes the failure visible, and visible is better
than silent, but neither is as good as not failing.

The instinct it produces is the evidence. `game/battle_arena` set `bodyBudget = 64` because
its fighters are created in code rather than authored in the scene -- true, and irrelevant: a
`CharacterVirtual` is not a body and `createCharacter` skips the check outright
([Physics.cpp:824-827](../../../engine/scene/Physics.cpp#L824)), so the number bought nothing
and cost 194 slots against the engine's own default. The first real caller reached for it
wrongly, and reached for it at all only because it was there.

## Physics

`JPH::PhysicsSystem::Init` takes `inMaxBodies` and has no resize path, because
`BodyManager::Init` reserves the body array and `new BodyID[inMaxBodies]` outright
([BodyManager.cpp:119-130](../../../external/JoltPhysics/Jolt/Physics/Body/BodyManager.cpp#L119))
so a `Body*` held by a solver job across a step can never be invalidated. Growth is therefore
a rebuild: a new system at a larger size, everything re-added, between steps and nowhere else
-- which is where `reclaim()` already lives ([Physics.h:666](../../../engine/scene/Physics.h#L666))
and for the same reason.

Handles survive. Every id this class hands out is an index-plus-generation into its own
vectors with Jolt's raw id in `Body::id` ([Physics.h:614-620](../../../engine/scene/Physics.h#L614)),
so a game's `BodyId` is untouched as long as that field and `Impl::bodyIndexById` are
rewritten. Three things have to be reconstructed rather than moved:

- **Bodies**, from state read back through the `BodyInterface` -- shape, transform, both
  velocities, motion type, layer, the five material and damping fields, allowed DOFs, user data.
- **Characters**, and this is what makes the row M rather than S. A `JPH::CharacterVirtual` is
  constructed against a `PhysicsSystem*` and holds it for its collision queries, so every one
  is recreated against the new system with its transform, velocity and both jump windows
  carried across. `Character` does not currently retain the `maxSlopeAngle`, `mass` and
  `radius` its settings were built from; three floats on the slot removes all read-back guessing.
- **Cloth**, from a `SoftBodySharedSettings` ref retained beside each `ClothBody` slot, with
  particle positions and velocities restored.

## Particles

Cheaper, and the hazard is the pairing rather than the growth.
[ParticleSystem.h:314](../../../engine/scene/ParticleSystem.h#L314) states that the pool is not
resized because `setEmitters` derives `capacity()` and the renderer allocated its buffers
against that -- so growing is two calls, and missing the second emits into storage the device
does not have. `Renderer::setParticles` rebuilds the pool buffer, the key buffer, the
pipelines and `particleIndexBits` from `system->capacity()`
([Renderer.cpp:4687-4722](../../../engine/gfx/Renderer.cpp#L4687)).

`Engine` owns both objects, so it is the one place the pair can be made atomic: one method
that grows the system and re-pairs the renderer, with the device idle wait `addModel` already
does for the same reason. Nothing outside `Engine` should be able to do half of it.

## Lights and voices

`lightBudget` and `voiceBudget` go the same way and are expected to be smaller: neither is a
third-party allocation. They are in scope for this row rather than deferred, because a rule
that holds for two of four budgets is not a rule -- it is two exceptions.

## Verification

- `./test.sh debug`, then `./test.sh asan`. The hosted cases are where the physics half is
  actually checkable: create bodies past capacity and assert every earlier `BodyId` still
  resolves to the same body, and that a character's position, velocity and jump-buffer state
  are unchanged across a growth.
- `scripts/golden.sh check` -- every case byte-identical. A world that never reaches its
  capacity must not rebuild at all, and the golden set is entirely such worlds.
- Zero validation errors with layers on, including a capture taken across a particle growth --
  a pool regrown without the renderer re-paired is precisely a write past a mapped range.
- `leak`: two runs at different growth counts reaching the same steady-state `VRAM [...]` and
  RSS, under ASan with `--no-ray-query`. Dropping the old system's or the old pool's
  allocations is the failure mode this has.

## Expected to be wrong about

The growth factor, and whether a rebuild is cheap enough on demand or wants a high-water mark
plus headroom so a spawning frame rebuilds once rather than four times. Both are measurable
once growth exists; neither changes the shape of the work.

## Reference update

[architecture/systems.md](../../architecture/systems.md) on the physics world's and the particle
pool's lifetimes; the budget rows in [architecture/limitations.md](../../architecture/limitations.md),
[architecture/principles.md](../../architecture/principles.md) and
[architecture/tooling.md](../../architecture/tooling.md), all of which state the
refuse-and-report policy this replaces; and the `setup.bodyBudget` line in
[guides/making-a-game.md](../../guides/making-a-game.md).

## Outcome

All four budgets are floors. Physics rebuilds Jolt's fixed-size world at double capacity and
carries bodies, cloth and characters across; the particle pool and the light buffer reallocate
and the renderer is re-paired in the same call; voices double to `kMaxVoices` and then steal
the quietest one-shot. `game/battle_arena`'s `configure()` states no budget at all, and its
physics world sizes itself to 258.

**The estimate was right about the characters and wrong about what else growth would touch.**
The card predicted `CharacterVirtual` holding a `PhysicsSystem*` would be the hard part, and it
was — rebuild plus `RefreshContacts`, or every character reports airborne for a step. What it
did not predict was a **pre-existing defect that growth promotes from rare to ordinary**: the
interpolation snapshots were one pair laid out bodies-then-characters, so creating any body
after `finalize()` shifted every character's slot and `characterTransform` returned identity
until the next step — a character at the origin for one frame because something else spawned.
Runtime body creation was rare enough to hide it; growth makes it the normal case. Split into
four arrays, which is why this row touched `characterTransform` and `setCharacterTransform` at
all.

Two more the checks caught rather than the design. The particle pool was created without
`VK_BUFFER_USAGE_TRANSFER_SRC_BIT`, so the copy that carries particles across a resize was a
validation error the moment it first ran — found by a forced growth under layers, not by any
test. And `ParticleSystem::requiredCapacity` clamps to `kMaxCapacity` before returning, so the
"an emitter that can never fit is refused" guard compared a clamped value against its own
ceiling and could never fire; the unclamped need is computed at the call site now.

Three tests asserted the old ceiling semantics and were rewritten rather than deleted:
`TheBudgetRefusesRatherThanOverruns`, `BudgetCapsAndStaysAPowerOfTwo` and
`TheVoiceBudgetRefusesAndCounts`. A fourth,
`TheValueAddBodyReturnsOnFailureIsSafeToAskAbout`, used budget refusal only as a way to obtain
an invalid handle and now gets one from a shape Jolt will not build.

**Verification.** `./test.sh debug` and `./test.sh asan`: 1063 of 1063, 107 suites, each its own
invocation. `scripts/golden.sh check release`: 13 of 13 byte-identical. `scripts/locomotion.sh
release`: 9 of 9 arms — run although the card does not name `scripted-input`, because this row
touches the character controller; `platform-ride` is the arm the snapshot split would have
broken. Validation: zero errors across forced growths of the particle pool (256 → 1024 → 2048)
and the light buffer (2 → 8), after the missing `TRANSFER_SRC` was fixed. Leak: a run that grew
twice and a run pre-allocated to the same final size both settle at **540.6 MiB in 154
allocations**, for particles and for lights independently.

`stealVoice()` is covered by inspection only. Reaching it needs 1024 live voices, and a test
that decodes 1024 sources to exercise one branch is a slow test bought at the wrong price.

## Deferred

- `Renderer::lightTileWords` is recomputed on growth and can cross `kLightTileMaxWords`, which
  disables tiled light assignment for the rest of the run. That is the stated behaviour at init
  and is unchanged here, but it is now reachable *without a game asking for it* — worth a
  measurement before it is worth a card, and recorded in
  [limitations.md](../../architecture/limitations.md) as the behaviour past the limit.
- Nothing shrinks. A burst that grows the particle pool holds it at the high-water mark for the
  run. Correct for a first cut and cheap in memory; a game that swings between scenes would
  want the opposite, and that is [C41](../backlog/C41-a-game-composes-its-scene-and-a-glb-is-an-asset-it-imports.md)'s
  territory rather than this row's.
