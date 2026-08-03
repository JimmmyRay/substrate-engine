---
id: G8
title: What a game cannot reach yet
arc: G
size: S
verification: tests-hosted, scaffold
---

# G8 — What a game cannot reach yet

S

## Debug affordances — capabilities, not bindings

**The engine binds no keys and draws no panel of its own.** It exposes what it can do, and
the game decides how, or whether, to reach it. That keeps exactly one owner of the
keyboard, which is what the duplicate `case` hidden in the old `onKey` switch was about.

```cpp
if (in.pressed(act.screenshot)) e.renderer().requestCapture(nextShotPath());
if (in.pressed(act.profile))    e.dumpProfile();          // Profiler + GPU timings
if (in.pressed(act.nextView))   e.renderer().cycleDebugView(+1);
```

Most of these already exist as one call — `requestCapture`, `setDebugView`,
`setSampleCount`, `Profiler::dump()`. This row was written when the problem was the
*reach*: they were locals in `main()`, and `Engine&` was the reach.

**That half is done, and G1 did it.** `Engine` owns every one of those subsystems and hands
out references to the concrete types, and `game/demo/DemoGame.cpp` already writes
`r.requestCapture(nextCapturePath())` and `e.dumpProfile()` through an `Engine&`. Nothing in
the first three lines above needs building.

## What is actually left — three capabilities with no method at all

The other half of the row, and all of it that survives:

| Capability | What exists | What a game cannot write |
|---|---|---|
| Step the debug view list | `setDebugView`, `currentDebugView`, `DebugView::Count` | `cycleDebugView(+1)`. `DemoGame` open-codes a cast, a modulo over `Count` and a cast back |
| Ship a control scheme | `InputMap::setBindings`, `defaultBindingList`, `isDefault` | `setDefaultBindings`. See below — the two are not the same call |
| Record a session | `Recorder`, `Renderer::startRecording`, `AudioEngine::startCapture` | `Engine::startRecording` / `stopRecording`. All three pieces are built and only `record.enabled` at startup can put them together |

**One carve-out, stated rather than discovered: config-driven runs stay engine-side.**
`--capture`, `--frames`, `benchmark.captureFrame` and `--overlay` are run *modes* rather
than bindings, and `scripts/baseline.py` and `scripts/golden.sh` depend on them working
with no game involvement at all. The rule is:

> **The engine acts on its own config. The engine binds no keys.**

`--record` is that rule applied to the recorder, and it is why the recorder is reached
through `Engine` rather than left as three calls: starting one spans the renderer's frame
tee, the audio tap and the `Recorder` the engine owns, and a game that assembled those
itself would get a silent video and no warning.

## Notes the G8 row carries

**Why a default and not an override.** `InputMap::setBindings` already exists and a game can
call it today, so the row looks unnecessary until you ask what the binding menu then shows.
A binding written that way is indistinguishable from one the *user* chose: `isDefault()` goes
false, "reset" offers to put the camera back on W, and a saved bindings file writes rows
nobody touched. `setDefaultBindings` rewrites the declared default and the live list
together, which is the difference between a game shipping a control scheme and a game
pretending to be a user who edited one.

The ordering it depends on is already correct and needs no change: `Engine::run` applies the
config's rebinds *after* `Game::init`, so a user who has bound W to something else still
wins. That is also what bounds the call: at any later moment it would overwrite a live
rebind, so `Game::init` is the documented call site and the doc comment says so.

**Why one row rather than three.** They are three instances of the argument the debug
affordance section makes — a capability that exists with no door onto it — and three is the
Rule of Threes met, applied to doors rather than to code. They also land in three different
files and collide with nothing, so splitting them would be three rows of about sixty lines
each.

**What it must not become.** Not a capability registry, not a `DebugCommand` table, and not
an engine-owned debug menu. **The engine still binds no keys**; this row only makes sure that
every capability it has can be reached by a game that wants to bind one.

**Why the cycle arithmetic is not in `Renderer`.** `advanceDebugView` is a free function in
`gfx/DebugView.h`, which is one of `SUBSTRATE_HOSTED_SOURCES`; `Renderer.cpp` needs a device
to link and so has no unit test of any kind. The half worth pinning is the modulo, and
putting it where a test can reach it costs one function and no indirection — `Renderer` is
still where a caller finds it, as a one-line member.

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- A scaffolded game builds and runs without touching anything under `engine/`.
- `tests/DebugViewTests.cpp` pins the cycle: enum order, the wrap in both directions, that
  no step of any magnitude or sign leaves the list, and that the four views the golden set
  photographs through `--debug-view` are where a cycle from `Lit` lands on them. The golden
  set would be the stronger check and is unusable at the moment this card is worked; a
  hosted test is the part that does not depend on it.
- `tests/InputTests.cpp` pins `setDefaultBindings` against `setBindings` on all three
  symptoms the note above names — `isDefault()`, what "reset" restores, and whether
  `saveBindings` writes the row.
- `./test.sh debug` and `./test.sh asan`.

## Reference

[architecture/limitations.md](../../architecture/limitations.md).

## Outcome

**The row was largely overtaken before it was started, and the card said so only after it
was rewritten.** Its headline claim — that these capabilities exist and cannot be reached
because they are locals in `main()` — was paid in full by G1 and G1b. `Engine` has owned
every subsystem since, `game/demo/DemoGame.cpp` was already calling `requestCapture` and
`dumpProfile` through an `Engine&`, and the three-line sketch at the top of this card needed
no engine change at all. That was verified in the tree before anything was written, and the
body above was rewritten to describe what was left. A card that describes work already done
is worse than no card, and this one was two thirds that.

**What was left is what landed: three capabilities that had no method, not no reach.**

- `gfx::advanceDebugView(view, step)` in `gfx/DebugView.h`, and `Renderer::cycleDebugView(int)`
  as the one-line member over it. `DemoGame` had the cast, the modulo over `DebugView::Count`
  and the cast back written out inline; it now calls the method. The arithmetic went into
  `DebugView.cpp` rather than `Renderer` because that file is in `SUBSTRATE_HOSTED_SOURCES` and
  `Renderer.cpp` needs a device to link — which is what made the stronger verification
  possible at all.
- `core::input::InputMap::setDefaultBindings`. Four lines, and the argument for it is longer
  than the code: `setBindings` does the visible half and leaves `defaults` behind, so a
  shipped control scheme reads as a user edit in three separate places.
- `Engine::startRecording(path)` and `Engine::stopRecording()`. This is the one the old
  roadmap named third and this card had lost; it was recovered from the retired
  `ROADMAP-GAME-API.md` row rather than guessed. `initRecording()` was a private method
  driven only by `record.enabled` at startup, so **no key could reach the recorder at all**,
  and every piece behind it — the `Recorder`, `Renderer::startRecording`,
  `AudioEngine::startCapture` — was already built. `initRecording()` is now the config gate
  and two lines; `teardown()` calls the same public `stopRecording()` a keypress does, so
  exit and a key can no longer end a recording two different ways. `stopRecording()` returns
  the file it wrote.

**Three notes on what the estimate did not predict.** The row was sized S and called
"fifteen minutes" in `order.md`; the code is about that, and finding the third capability,
rewriting the card and the reference took the rest. Second, `startRecording` needed a `path`
parameter that the sketch did not have — without one every take overwrites `record.file`,
and taking several takes in a session is the whole reason a key beats a flag. Third, the
recorder start/stop cycle turned out to already work: `Recorder::start` reclaims a stopped
session's thread and pipes, which was written for a *dead encoder* and happens to be exactly
what a second take needs.

**Deliberately not done.** No capability registry, no `DebugCommand` table, no engine-owned
debug menu — recorded as a refusal with its trigger in `limitations.md` rather than left
implicit. No generic "cycle an enum" helper either: the demo cycles `DebugView` and
`TonemapOperator`, and two is a coincidence — the tonemap one also goes through the settings
table rather than a renderer field, so bundling them would be the Rule of Threes broken at
two *and* on two different intents. `InputMap::setDefaultBindings` rewrites the live list
unconditionally rather than preserving a rebind, because `Engine::run` applies the config's
rebinds after `Game::init`; the contract is stated at the declaration instead of made
conditional on hidden state.

### Verification

- **`./test.sh debug`** — 677 tests, 74 suites, all green. **`./test.sh asan`** — the same
  677, all green. Eleven of those are new: `tests/DebugViewTests.cpp` (6) and five in
  `tests/InputTests.cpp`.
- The cycle test is the strengthening this row got in place of the golden set, which is
  unusable at the moment (8 of 12 cases differ at HEAD for a reason that has nothing to do
  with this card, and no re-snap was attempted). It pins enum order, the wrap in both
  directions, `step == 0`, `±Count`, that a sequence of single steps equals one big step,
  and that **no step in [-5·Count-3, +5·Count+3] from any view leaves the list** — the last
  of which fails against the version that reduces only one operand and passes every other
  case. It also asserts that `albedo`, `normal`, `depth` and `ssao` sit 1, 2, 4 and 6 steps
  from `Lit`, which ties the cycle to the four positions the golden set photographs through
  `--debug-view`, so those cases become a real check on this the moment the set is good again.
- The binding tests assert the difference against `setBindings` on all three symptoms:
  `isDefault()`, what `resetToDefault` restores, and whether `saveBindings` writes the row
  into the config file.
- **Driven end to end through C16's input script**, which is what proves the door opens
  rather than merely compiles: `./run.sh demo debug -- --frames 140 --input-script
  "20:Capture.Record,50:Capture.Record,80:Capture.Record,120:Capture.Record"` produced two
  complete start/stop cycles in one session — `Record: started`, `wrote ... (3 frames)`,
  `started`, `wrote ... (4 frames)` — exit 0, zero validation errors, no binding conflict
  reported for the new `Capture.Record` on `.`. A second run scripting `View.Cycle` walked
  lit → albedo → normal.
- **`scaffold` — and it found something, which is recorded here rather than fixed.**
  `./new_game.sh g8probe` scaffolds and `./run.sh g8probe debug` builds and links with
  nothing under `engine/` touched, and runs clean for 30 frames against a named scene
  (`res:/emissive.gltf`, exit 0). **A scaffolded game with no scene — which is what the
  template generates — segfaults during bring-up**, after a
  `VUID-VkPipelineLayoutCreateInfo-pSetLayouts-parameter` validation error naming a null
  `VkDescriptorSetLayout`. This is **not** caused by this card: it was reproduced
  byte-for-byte at HEAD (13455b2) in a clean worktree with the same scaffolded game, and
  nothing here touches descriptor set layouts or scene loading. It is a real defect in the
  sceneless path and it means `scaffold` as a token is weaker than it reads — it has been
  passing on "builds and links". Left for its own card. The scratch game directory was
  deleted and `build/debug` put back on `demo`.
