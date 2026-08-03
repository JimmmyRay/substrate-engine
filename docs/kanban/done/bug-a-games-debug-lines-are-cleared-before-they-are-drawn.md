---
id: bug-a-games-debug-lines-are-cleared-before-they-are-drawn
title: A game's debug lines are cleared before they are drawn
arc: bug
size: S
verification: golden, validation
---

# bug-a-games-debug-lines-are-cleared-before-they-are-drawn — A game's debug lines are cleared before they are drawn

`Engine::endFrame` clears `renderer().debugLines` at [engine/Engine.cpp:2146], and `endFrame`
runs *after* `Game::frameUpdate` ([engine/Engine.cpp:2515] then [:2561]). A game that pushes
world-space lines from `frameUpdate` therefore has them erased before `drawFrame`
([:2238], inside `endFrame`) ever sees them. Afterwards the clear sits in `beginFrame`, and a
game's lines survive to the draw alongside the physics wireframe and the audio occlusion lines.

The renderer documents the opposite. [engine/gfx/Renderer.h:952-963]: *"A plain vertex vector
the application refills each frame ... `PhysicsWorld::drawDebug` writes into this, and **a game
drawing its own lines writes into the same one**."* That sentence is false today, and it is the
only place the capability is described — so the first game to try it finds a vector that
silently eats what it is given.

**Nothing has caught it because nothing has used it.** The two writers that exist are both
inside `endFrame` and both run *after* the clear: `physicsWorld.drawDebug` at [:2153] and the
audio occlusion pair at [:2172-2173]. Both keep working either way, which is why the ordering
has never mattered. `game/demo/` draws no lines; the first caller is `game/battle_arena/`,
which wants to preview a queued action's flight arc, and that preview is blocked on this.

The fix is moving one statement from `endFrame` to `beginFrame`. The clear's own comment at
[:2143-2145] defends a property — *"turning the toggle off empties it on the next frame rather
than leaving the last one drawn forever"* — and that property survives the move unchanged: the
clear is still unconditional and still happens once per frame, just at the other end of it.

What I expect to be wrong about: nothing about the ordering, which is mechanical. The risk is
that some path pushes lines *outside* the frame — between `endFrame` and the next
`beginFrame` — and currently gets away with it because the clear is late. A grep says there is
no such path, but a grep is not a run, which is what `golden` is here for.

## Verification

- `scripts/golden.sh` — thirteen cases, byte-identical. None of them enables `--physics-debug`,
  so this proves the move regressed nothing rather than proving the fix; a moved pixel here
  would mean lines are now drawn that were not before, which is exactly the defect in reverse.
- Zero validation errors with layers on, over a run with `--physics-debug` and `--audio-debug`
  both set, so both existing writers are exercised through the new ordering.
- **The fix itself is proven by a caller**, since no engine test draws a line: a game pushing
  into `renderer().debugLines` from `frameUpdate` must see them on screen. `game/battle_arena/`
  is that caller and lands in the same session.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — the debug-line pass, if it
states when the vector is filled. The contract sentence in
[engine/gfx/Renderer.h:952-963] becomes true rather than changing, so it needs no edit.

## Outcome

Landed as one moved statement: the clear went from `endFrame` to `beginFrame`, beside
`render.uiDrawList = nullptr`, which is the other per-frame draw-state reset and the same
category of thing. The physics wireframe's comment changed from "cleared unconditionally" to
"appended rather than assigned", because that is what it now is.

**Verified by a caller, which is the only way this could be.** No engine test draws a line, so
the fix was proven with `--capture` from `game/battle_arena/`, which now previews a queued
action's flight arc through `renderer().debugLines`: **3919 pixels of the chain-line colour in a
frame with a chain queued, and 0 in the same frame with none.** Before the fix both would have
been 0. `scripts/golden.sh` 13/13 byte-identical, `./test.sh debug` 1075 tests in 109 suites,
and 300 frames with `--validation on --physics-debug --audio-debug` printing zero warnings,
errors and VUIDs -- the last one chosen so both pre-existing writers were exercised through the
new ordering.

What the estimate did not predict: nothing about the ordering, which was mechanical. The
suspected risk -- some path pushing lines outside the frame and getting away with it because the
clear was late -- did not exist; the grep was right, and `physicsWorld.drawDebug` and the audio
occlusion pairs are still the only two engine writers.
