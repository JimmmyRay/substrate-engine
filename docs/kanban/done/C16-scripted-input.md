---
id: C16
title: Scripted input
arc: C
size: S
verification: tests-hosted, golden-12
---

# C16 — Scripted input

A deterministic action feed: a list of `(frame, action, pressed)` driven from the command line, resolved by `InputMap` exactly as a device would be. **Found by C6**, which could unit-test its own refusals 32 ways and could not press its own save key. Three callers waiting for it: an end-to-end save regression, the golden suite (which currently proves what a scene *renders* and nothing about what it *does*), and input regression itself — the binding table has 49 actions and no test drives one through the map from a key event to a game reading `pressed()`

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- `./test.sh debug`, then `./test.sh asan`, each its own invocation. The suite must gain a
  hosted test that plays a **scripted** press into an `InputMap` and asserts that the
  action a game reads with `pressed()` sees it on the frame the script named and on no
  other -- resolved through the binding table, not by calling `onKey` beside it. That is
  the check this card's own body asks for and the one nothing else can stand in for.
- `scripts/golden.sh` -- twelve cases, byte-identical. Worth keeping, but be honest about
  what it proves: the golden cases run `--headless --locked` and press nothing, so a
  byte-identical set says only that this feature does nothing when no script is given.
  That is a real property -- it is what makes the feed safe to leave in the engine -- and
  it is not evidence that the feed works.

## Reference

[architecture/tooling.md](../../architecture/tooling.md), [architecture/systems.md](../../architecture/systems.md).

## Outcome

`input::Script` in `engine/core/Input.{h,cpp}` — a list of `(frame, action, down)` played
into an `InputMap` by frame index, through the same `onKey` / `onMouseButton` / `setGamepad`
calls the window layer uses. `--input-script 60:Game.Save,90:Camera.Forward+,150:Camera.Forward-`;
`+` presses, `-` releases, a bare action taps. `Engine::beginFrame` applies it between
`pollGamepads` and `beginFrame` — after the devices so a scripted run does not depend on
what is plugged in, before the resolve so a scripted press is subject to text mode, the
deadzone and the tap edge like any other.

**It names actions and follows their bindings, and that is the load-bearing decision.** A
rebind moves what a script drives, which is what makes this a test *of* the 49-row binding
table rather than a way past it — and it is why the tests assert the raw key behind the
binding went down rather than only that the action fired. An implementation that wrote
`pressed` directly would satisfy "the game saw it" and prove nothing, so several of the
twelve `ScriptTest` cases exist purely to fail against that implementation.

Three things the estimate did not predict:

- **A pad is a state, not an event.** `setGamepad` takes the whole struct, so a step
  pressing a second button has to restate the first. Rebuilding it by replaying every step
  up to the current frame is what avoids carrying state between calls — which in turn is
  what lets `apply` be `const` and a scripted frame be a function of its index alone.
  A script with no pad step never calls `setGamepad`, so a real pad still works beside one.
- **A pad *axis* takes its sign from the binding, not the step.** `Camera.Forward` is
  `Pad.LeftY-`, so pressing it means driving that axis to -1 and letting `resolve`
  multiply the scale back out. Full travel rather than something just past
  `kDigitalThreshold`, because an action carries a float and half a stick is half the speed.
- **The two ways to waste a run both have no symptom.** A step naming an action this build
  does not have, and a script running past `--frames`, both look exactly like a feature
  that does not work. Both are now errors logged from `Engine::applyBindings`, which is the
  earliest moment the answer exists — the map is not complete until the game has declared
  its own actions, which is the same reason the conflict scan is there.

**Deliberately not done.** No script *file* (`--input-script @path`) and no mouse motion or
scroll in the feed. Both are speculative until a consumer needs them: G8, G9 and C17 want a
press on a stated frame, which a command line holds comfortably, and adding a file format
now would be a schema nobody has asked a question of. No default of `--frames` from
`lastFrame()` either — the accessor is there and a harness can use it, but having one flag
silently set another is the coupling `--capture`'s implied frame index already costs enough.

**Verification.**

- `./test.sh debug` — **667 tests passing**, up from 654; 13 new (12 `ScriptTest`, 1
  `ConfigTest`).
- `./test.sh asan` — **667 passing**, no leaks or diagnostics.
- End-to-end, which is the case C6 could not write: `./run.sh demo release -- --headless
  --locked --frames 40 --audio-null --input-script 20:Game.Save` writes
  `debug_frames/demo.sav` and logs *"saved debug_frames/demo.sav (2 sections, 7327
  bytes)"*. A run with `--frames 10 --input-script 20:Game.Lode` logs both guards.
- `scripts/golden.sh check release` — **8 of 12 cases differ, and the cause is not this
  card.** See below.

**The golden set is currently invalid, and it is not this row's doing.** The failing eight
are every case that reaches the tonemap; the four that pass byte-for-byte are exactly the
four `--debug-view` cases (`albedo`, `normal`, `depth`, `ssao`), which never do. Deltas are
whole-image — `lit` reports 1362726/1440000 pixels over tolerance, mean 29.18. The baselines
in `debug_frames/golden/` carry an mtime of 21:34 today, between G5 landing at 21:10 and G7
at 22:42; **G5's own card records all 12 byte-identical at 21:10**, and G7's records that its
first pass rewrote `tonemap.frag` and that the rewrite was reverted before it landed. So the
reference images were re-snapped from a tree carrying a tonemap that is not in HEAD, and
every lit case has been failing since.

Nothing was re-snapped here, deliberately. This row cannot be the cause: it touches no
shader, renderer, scene or game file, and without `--input-script` the feed does not write
to the map at all — asserted by `ScriptTest.AnEmptyScriptTouchesNothing`, which is the check
the twelve goldens were standing in for. **The reference set needs regenerating on a card
that owns it**, where which tonemap is correct is the subject rather than a side effect.
