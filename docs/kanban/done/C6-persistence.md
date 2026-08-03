---
id: C6
title: Persistence
arc: C
size: L-M
verification: golden-12, tests-hosted, validation
---

# C6 — Persistence

`core::SaveWriter`/`SaveReader`, the two `Game` virtuals, and versioning that refuses an unknown version with a reason -- all as the row named them. ~~**Needs G3**: the tree is what gets written~~ **It did not; see above.** A file is a header, a fixed-width table of contents, and named sections each carrying their own version, so the engine's half and a game's half version independently and an unknown section is *skipped* rather than fatal. The refusal logic lives in `scene/WorldSave.h` rather than inside `Engine::loadGame`, because it is arithmetic over a scene name and a slot count -- no device -- and putting it there is what makes the one part worth testing reachable by the hosted suite. 32 hosted tests, and the demo binds both halves so the runtime path has a caller outside the suite

## What C6 found once it looked at what it was actually saving

The row is small and its findings are not. Three, in order of how much they cost:

**A flag that looked like state and was not.** The first draft saved
`kInstanceVisible | kInstanceDynamic`, and one test asserted a freshly created instance
was visible. It was not — and the reason is that `kInstanceVisible` is written by the cull
dispatch into the *GPU's* copy of the table and never read back, so the CPU-side bit is
clear for everything, always. `Inspector` already knew this and refuses to draw a row for
it ([`Inspector.cpp:127`](../../../engine/ui/Inspector.cpp#L127)); the save did not, and was
writing a constant zero and restoring it over nothing. **A field that looks like
persistence and is not is worse than an absent one**, because the absent one prompts
someone to add it. `kSavedInstanceFlags` is now one bit, and the constant survives at one
bit precisely so the exclusion is written down rather than implied.

**The demo's save key already belonged to something else.** `Game.Save` went on F2, which
`View.Albedo` had owned since Phase 0, and nothing anywhere said so — `InputMap::declare`
deduplicates by *name*, which is silent about two actions reaching for one key. So one
press both saved and switched the debug view.

**And the check that found it found a real one underneath.** `InputMap::conflicts()` was
added to turn that class of failure into a startup line, and on its first run it reported
two more pairs that had been there far longer:

- `Camera.Up` and `Player.Jump`, both on **Space**. Not mode separation: `Camera::update`
  reads `up` unconditionally while a game reads its jump only where a player character
  exists, so in the demo — whose own HUD says `space=jump` — one press flew the camera
  *and* jumped. `Camera.Up` is now `E` alone, which is also the symmetry `Camera.Down`
  always had.
- `Camera.Orbit` and `Ui.Click`, both on **Mouse.Left**. This one is correct, and the
  engine already said so: `resolve()` returns zero for the non-exempt action whenever the
  pointer-mode-exempt one can fire. So `conflicts()` consults `pointerExempt` rather than
  inventing a second notion of "not at the same time" beside the one that exists. **A check
  that warns about the engine's own correct defaults on every run is a check nobody reads.**

The query is a query and not a refusal, because a collision is not always a bug — but both
of the ones it found on day one were, which is the argument for having it.

**What is still not covered.** The engine's save *decision* is hosted and tested 32 ways;
the ~6 lines of `Engine::saveGame`/`loadGame` glue are not, because there is no way to
drive a keypress headlessly. That is a missing facility rather than a missing test, and it
is worth its own row — scripted input would serve the golden suite and input regression
too, and building it inside C6 would have been the wrong place for it.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- Zero validation errors with layers on, in every capture.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

Recorded above, under *What C6 found once it looked at what it was actually saving*.
