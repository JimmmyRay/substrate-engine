---
id: chore-a-game-gets-one-zone-and-its-fixed-step-gets-none
title: A game gets one zone and its fixed step gets none
arc: chore
size: S
verification: trace, tests-4, golden-11
---

# chore-a-game-gets-one-zone-and-its-fixed-step-gets-none — A game gets one zone and its fixed step gets none

Afterwards `Game::frameUpdate`, `Game::fixedUpdate` and `Game::drawUi` are three zones rather
than one-and-a-half, and `game/demo/` carries the handful of scopes that show a game author
what instrumenting their own code looks like.

Two separate holes, and the second is the serious one.

**`game` covers two unrelated things.** The scope at
[Engine.cpp:2126](../../../engine/Engine.cpp#L2126) wraps `game.frameUpdate()` and
`game.drawUi()` together, so game logic and game UI are one number. They fail differently and
they are worked on by different people.

**`game.fixedUpdate` is inside no zone at all.** It is called from the fixed-step loop at
[Engine.cpp:2143](../../../engine/Engine.cpp#L2143), outside the `game` scope and beside
`Engine::simulate` rather than within it. So a game's simulation logic — the half of a game
that runs on the deterministic clock, and the half most likely to be doing real work — is
attributed to nothing anywhere in the trace. It shows up only as part of the gap between
`Frame` and the sum of its children, which is where unnamed work goes to be invisible.

**The demo has no instrumentation at all**, and it is 2,810 lines.
[`DemoGame.cpp`](../../../game/demo/DemoGame.cpp#L6) even includes `core/Profiler.h` without
ever using it — the include is left over from `dumpProfile()`, which is an `Engine` method
and needs no such include. That include is either deleted or made true, and this card makes
it true.

The argument is the one the engine keeps making about itself: **this engine is for projects
much larger than the demo**, and on such a project the game's own code is where the time
goes. Today the engine profiles itself in fine detail and profiles its caller not at all. A
game author looking for the pattern to copy finds no example in the tree.

That is what the demo scopes are for — not because the demo is slow, but because
`game/demo/` is the worked example of every other engine API and this is the one it is silent
about. A few scopes on the demo's own per-frame work, following the same naming rule the
engine uses, is the whole of it.

## Verification

- `scripts/baseline.py --config debug --zones` — `Game::frameUpdate`, `Game::fixedUpdate`
  and `Game::drawUi` appear as separate rows with numbers, and `Game::fixedUpdate`'s
  `total/frame` is a per-frame sum over however many steps the clock consumed rather than a
  per-step median. That column is the reason the sum is readable; a game running two steps in
  a frame has a median that understates it by half.
- The gap between `Frame` and the sum of its children narrows by at least what
  `Game::fixedUpdate` reports, which is the check that it really was unattributed before.
- `./run.sh demo` with a trace, and the demo's own zones appear under `Game::frameUpdate`.
- `scripts/golden.sh check release` — eleven cases, byte-identical. No golden case runs the
  demo, so this is a null check on the engine paths the demo shares.
- `./test.sh` in all four configurations.

## Reference update

[guides/profiling.md](../../guides/profiling.md) — "What is instrumented" gains the game's
three entry points.

[guides/making-a-game.md](../../guides/making-a-game.md) — the guide that holds the call-site
sketches. A game may profile its own code, the scope is the engine's own `Profiler::scope`,
and the demo is the example; none of that is written anywhere today.

## Outcome

Three engine zones, four demo scopes, and the `Profiler.h` include made true.
`Engine.cpp`'s `game` scope became `Game::frameUpdate` and `Game::drawUi`, and
`Game::fixedUpdate` was added inside the fixed-step loop where nothing wrapped
`game.fixedUpdate` before. `game/demo/` gained scopes on `applyActions`,
`DemoGame::playImpacts`, `DemoGame::driveLocomotion` and `stepDemoWorld`.

`./run.sh demo debug --locked --frames 300`, 239 traced frames:

| Zone | ms/frame |
|---|---|
| `Game::frameUpdate` | 0.0048 |
| `Game::frameUpdate/applyActions` | 0.0005 |
| `Game::fixedUpdate` | 0.0078 |
| `Game::fixedUpdate/DemoGame::driveLocomotion` | 0.0050 |
| `Game::fixedUpdate/DemoGame::playImpacts` | 0.0008 |
| `Game::fixedUpdate/stepDemoWorld` | 0.0004 |
| `Game::drawUi` | 0.0001 |

The 0.0078 ms is the number the card was about: it was attributed to nothing anywhere in the
trace before, and the `Frame`-to-children gap closes by exactly it, because adding a scope is
the only change.

**`Game::drawUi` sits above the `uiOpen` test rather than inside it**, which the card did not
specify and which the sibling card's rule decides: a row that vanishes when the UI is closed
reads as missing rather than as zero. It reports 0.0001 in a run with the panel shut.

**Two things the estimate did not predict, both about verification rather than about the
work.** `scripts/baseline.py` needed no change — its `total/frame` column is already a pooled
sum divided by frame count, which is exactly the per-frame-over-N-steps figure the card asked
for and reasoned would need arranging. And `baseline.py` invokes `./run.sh <config>` with no
game name, so it opens Sponza rather than the demo's own scene: `DemoGame::driveLocomotion`
and `stepDemoWorld` report zero or are absent there because neither a player character nor
`world.built` exists. The demo's zones have to be read from a `./run.sh demo` trace, not from
`baseline.py`. That is a property of the tool, not of this card, and it is worth knowing
before the next card tries to read a game zone out of a baseline run.

928 tests in each of debug, release, asan and tsan; `scripts/golden.sh check release`, eleven
of eleven byte-identical.
