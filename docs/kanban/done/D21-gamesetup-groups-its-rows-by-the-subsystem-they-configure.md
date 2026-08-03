---
id: D21
title: GameSetup groups its rows by the subsystem they configure
arc: D
size: S-M
verification: golden, tests-hosted, scaffold
---

# D21 — GameSetup groups its rows by the subsystem they configure

Afterwards `GameSetup`'s thirty-odd fields sit in nested structs named for what they configure
-- `setup.audio.occlusion`, `setup.look.sunIntensity`, `setup.present.virtualResolution` --
rather than flat in one struct where a row's subsystem is something you infer from its name,
or from the comment above it, or not at all.

**Audio is the case that shows it.** Eleven of the flat fields are audio: `voiceBudget`,
`buses`, `sources`, `listeners`, `listenerFollowsCamera` and six `occlusion*` rows
([Game.h:94-211](../../../engine/Game.h#L94)). Only two of the eleven say so in their names. And
`occlusion` is the worst of them, because the engine already uses that word for three unrelated
things: this one is audio, `GameSetup::Source::occlusion` is the same feature *per source*, and
`render.occlusionCulling` is a depth-buffer visibility test with nothing to do with either. A
game writing `setup.occlusion = false` has written something whose meaning is not recoverable
from the line.

The struct grew this way honestly -- every row was added beside the rows that existed, and each
addition was one field. What it costs now is the thing `GameSetup` exists to be: the single
surface a game reads to learn what it may author. Flat, that surface answers "what can I set"
and refuses to answer "what does this belong to", which is the question somebody has when they
are looking for a row rather than reading all of them.

**A grouping is not a rename.** Every field keeps its name, its type, its default and its
meaning; only its address changes. That is what makes this a `D` row rather than a capability:
nothing becomes expressible that was not, and the two games in the tree change by prefix alone.

## Groups, as a starting point rather than a conclusion

`look` (sun, ambient, exposure, tonemap, shadow bias), `sizing` (the four budgets, subject to
[C40](C40-the-engine-sizes-its-own-pools-and-a-game-states-no-budgets.md) removing most of
them), `present` (virtual resolution, ui-inside-virtual, pixel exact), `sim` (gravity, physics
step), `audio` (the eleven above). `name`, `scene`, `sceneScale`, `characters` and `decals`
stay at the top: they are what the program *is* rather than how one subsystem is tuned.

## Verification

- `scripts/golden.sh check` -- every case byte-identical. Nothing here changes a value.
- `./test.sh debug`, then `./test.sh asan`.
- `scaffold`: a scaffolded game builds and runs without touching `engine/`, which is the check
  that the new shape is writable rather than only readable.
- `./build.sh` alone with `game/` absent, and both games building with it present.

## Expected to be wrong about

Whether `sizing` should exist at all. If C40 lands first, three of its four rows are gone and
the fourth may not be worth a group; the ordering between the two cards is worth deciding
before either starts rather than resolving by whichever ran last.

## Reference update

[guides/making-a-game.md](../../guides/making-a-game.md), which walks `GameSetup` field by
field and is the document this row is really about, and the `GameSetup` rows in
[architecture/principles.md](../../architecture/principles.md).

## Outcome

`GameSetup` is five nested structs and four top-level fields. `look` holds the lights, ambient,
exposure, tonemap and the two shadow biases; `present` the virtual resolution, `uiInsideVirtual`
and `pixelExact`; `sim` gravity and the step; `audio` the buses, sources, listeners,
`listenerFollowsCamera` and an `Occlusion` sub-struct; `tools` the two capture paths. `name`,
`characters` and `decals` stay at the top, because they are what the program *is* rather than
how one subsystem is tuned.

**`sizing` does not exist, and that was the card's own open question.** C40 landed first and
made all four budgets floors that nothing in the tree set; D21 deleted them outright rather
than grouping four fields whose only remaining job was to restate a subsystem's default. The
starting sizes live where they are used -- `gfx::kDefaultLightBudget`, `PhysicsConfig`,
`AudioConfig::voiceBudget`, the particle pool's own derivation -- and every one of them grows.
`Engine::initRenderer`, `initAudio`, `initPhysics` and `loadScene` each lost the line that read
a budget out of `GameSetup`.

The occlusion rows are the case the card was written around and they came out best:
`setup.occlusion` -- a bare word the engine spends on three unrelated things -- is
`setup.audio.occlusion.enabled`, with `cutoffHz`, `gain`, `attack`, `release` and `rayMargin`
beside it. `GameSetup::Bus` and `GameSetup::Source` moved inside `Audio` with them.

Forty-odd call sites, all of them mechanical, across `engine/Engine.cpp`, both games, the
scaffold template and the retired-settings messages in `core/Settings.cpp` -- which said
"moved into game code as `GameSetup::lightBudget`" for four rows that no longer exist and now
say the budget is gone because the pool grows.

### Verification

| Check | Result |
|---|---|
| `scripts/golden.sh check release` | **13 of 13, byte-identical** |
| `./test.sh debug` | **1070 tests, 108 suites** |
| `./test.sh asan` | **1070 tests, 108 suites** |
| `scaffold` | `./new_game.sh` builds and runs |
| `./build.sh release` with no game | links `libsubstrate.a` |
| `scripts/check_ascii.sh` | clean |

Both games build and run. Nothing changed a value, which is what makes the byte-identical
golden set the check that this was a grouping rather than an edit.
