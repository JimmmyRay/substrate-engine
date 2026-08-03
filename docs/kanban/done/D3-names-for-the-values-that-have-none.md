---
id: D3
title: Names for the values that have none
arc: D
size: S
verification: golden-12
---

# D3 — Names for the values that have none

One declaration of `kNoNode`, in [`scene/Node.h`](../../../engine/scene/Node.h) -- the precedent for a header holding one constant is `core/Format.h`, which argues the same case for one macro. The cross-domain returns went with C1 and C7: `findClip` returns `kNoClip`, `skinOf` returns `kNoSkin`, and `kNoCharacter` no longer exists to be borrowed. The `DebugView` half was paid by the settings arc and the shadow removal -- see below

## D3, and the two defects under it

**Landed, and the row grew a fourth copy while it waited.** C7 added
`SceneAnimator::kNoNode` for root motion -- a new declaration of the same literal, written
by someone who had read this row and was thinking about something else. That is the
strongest argument the row makes: the copies are not carelessness, they are what happens
when there is no one place to put it. There is now, and the four are aliases of it.

The sentinel half was ordinary: `kNoNode` was declared in
[`Collider.h:83`](../../../engine/scene/Collider.h#L83) and
[`AudioSource.h:71`](../../../engine/scene/AudioSource.h#L71), and
[`ParticleSystem.h:87`](../../../engine/scene/ParticleSystem.h#L87) — a third struct with the same
field, parsed by the same `extras` pass — writes `0xFFFFFFFFu` and explains it in a comment
instead. A fourth declaration in `Light.h` went away with `LightOverride` rather than being
named, so the count is at the threshold rather than past it, and the shape of the finding is
unchanged: three occurrences is the rule, and the one that spells the literal is the one that
shows what happens when the rule is missed.

The other half is not ordinary, because **every sentinel in the engine is the same number**,
and that turns a naming problem into a correctness one:

- [`Animation.cpp:263`](../../../engine/scene/Animation.cpp#L263) `findState` and
  [`:270`](../../../engine/scene/Animation.cpp#L270) `findParameter` return **`kAnyState`** — a
  *transition-source* sentinel meaning "from any state" — and
  [`:301`](../../../engine/scene/Animation.cpp#L301) `findClip` returns **`kNoCharacter`**, a
  *character-index* one. The headers decline to use either name and say "or UINT32_MAX".
- It leaks across the public surface exactly as you would predict:
  [`DemoGame.cpp:108`](../../../game/demo/DemoGame.cpp#L108) compares a *clip* index against
  `kNoCharacter`, which is correct today by numeric coincidence and by nothing else. **The
  in-tree game is the illustration and not the evidence** — a caller who guards every site
  correctly is still a caller who was handed a sentinel from the wrong domain, and the next
  one has no way to know which name to test against.
- The hazard is structural rather than hypothetical: `transitions.push_back({kAnyState,
  findState("x"), ...})` for a missing `"x"` builds a transition *to* `kAnyState` instead of
  being refused. The demo happens to guard each of its own call sites, which is why this is
  written as the shape of the defect rather than as a bug report against `game/`.

~~**And one enum whose names live in three files, none of them the enum's.** `DebugView` is
declared at `Renderer.h:185` with `Shadow = 9, Count = 10`, the index-to-name map is written
separately in `Config.cpp` as a string-to-*magic-integer* chain that never mentions
`gfx::DebugView`, again as `--help` text, and a third time in `DemoGame.cpp` — where the
`Shadow` case is missing, so the settings panel's tenth row is captioned `?`.~~
**Substantially paid, and by two unrelated pieces of work.** The settings arc moved the enum
into its own header — [`DebugView.h:33`](../../../engine/gfx/DebugView.h#L33), with
[`debugViewKey()`](../../../engine/gfx/DebugView.cpp#L5) as the single name map, which
[`Config.cpp:586`](../../../engine/core/Config.cpp#L586) now calls instead of chaining magic
integers. The `?` row went with the `Shadow` view itself when shadow maps were replaced by
traced rays. **What is left is one copy, and it is in the game rather than the engine**:
[`DemoGame.cpp:17`](../../../game/demo/DemoGame.cpp#L17) still writes its own switch over all eight
values, ending in a `default: return "?"` that is now unreachable.

**Recorded rather than deleted, because the two halves were paid for opposite reasons.** The
name map was extracted *because a capability row needed it* — `Config.cpp` had to become one
of `SUBSTRATE_HOSTED_SOURCES`, and the header exists to keep Vulkan out of it. The `?` was
fixed by *deleting the feature that carried it*. Neither is the D arc landing, and one of
them is the pattern this document should expect more of: consistency work gets done when a
capability row is forced through it, at whatever moment that happens to be, in whatever shape
that row needs. That is an argument for deciding the convention early, not for trusting that
the rows will collect them on the way past.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.

## Reference

[architecture/principles.md](../../architecture/principles.md).

## Outcome

Recorded above, under *D3, and the two defects under it*.
