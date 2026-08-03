---
id: D14
title: Re-home the rows that are not user configuration
arc: D
size: L
verification: tests-4, golden-11, validation
---

# D14 — Re-home the rows that are not user configuration

Afterwards `substrate.json` holds about fifty rows a player would recognise, twenty-nine
developer controls are `Config` fields with no JSON key and a flag, eleven authored values are
`GameSetup` members, and every key that left is in `removedKeys` with the sentence saying where
it went.

[principles.md](../../architecture/principles.md) §7 already states the test — *is this a
property of the person running the program, or of the program?* — and already names the three
homes a value can have. The rule is right and the third home is in good shape: `Config::benchmark`,
`Config::camera`, `physics.clock`, `render.debugView` and `window.headless` all have no JSON key
and say in their own comments why. What has happened is that the *first* home kept absorbing
things after the rule was written, because a row is the easiest thing to add — it is one line,
and it comes with a parser, a flag, a panel widget and persistence for free. Convenience is not
the test.

Two findings make this drift rather than a difference of opinion. `ProfilerConfig` already
exists as a plain struct, and its comment documents a **deliberate divergence** between its
default and the settings table's default for the same field — two spellings of one value, which
is the exact failure the table was built to prevent, now caused by the table. And
`benchmark.exitAfterFrames` gives a module named for the test harness a key in the user's
config file.

What leaves, and where:

| Going to a `Config` field with no JSON key | |
|---|---|
| `render.validation`, `syncValidation`, `rayQuery`, `shaderHotReload` | validation layers and a shader recompile loop are not preferences; `--help` already says `--no-ray-query` is *"needed under ASan"* |
| `render.debugOverlay`, `debugFont`, `debugFontHeight` | the developer overlay and its font |
| `scene.bakeCache` | a build step — see [D9](D9-the-scene-baker-leaves-the-runtime.md), which wants the flag gone entirely |
| `physics.enabled`, `debugDraw`, `debugContacts` | skipping the world and drawing shapes |
| `audio.backend`, `audio.debugDraw` | `null` is a test mode; `auto` needs no user choice |
| `ui.panel`, `ui.inspector` | debug windows |
| `profiler.*` (6) | folded into the `ProfilerConfig` that already exists |
| `record.*` (4) | a capture control, beside the others |
| `logging.file`, `level`, `output` | joining the `categories` aggregate already in `Config::Logging` |
| `benchmark.exitAfterFrames` | every sibling is already there |

| Going to `GameSetup` | |
|---|---|
| `render.tonemap` | a look decision authored with the lighting it balances — the argument `GameSetup::exposure` already carries three lines away, and `DebugView.h`'s own *"looks a game could ship"* |
| `render.shadowDepthBias`, `shadowNormalBias` | tuned against a scene's geometry, which is the reasoning `AudioCfg` already applies to its six occlusion constants: game feel, not taste |
| `render.lightBudget`, `particleBudget`, `physics.bodyBudget`, `audio.voiceBudget` | how many lights, particles, bodies or voices a game's scenes hold is the author's sizing |
| `ui.panelX/Y/Width/Height` | the geometry of the *game's* panel — the demo is what reads them. They become the demo's own rows once [D17](D17-a-game-declares-its-own-settings.md) lands |

What stays is the answer to *"what would a player change?"*: window size and vsync, thirty
render quality and artefact toggles, input sensitivities, camera feel, the physics step budget
and worker count, audio rates and volumes, UI scale.

**The `removedKeys` obligation is the load-bearing half of this card.** Forty keys are in
people's config files today. The mechanism exists and already carries twenty-two entries, and
each new sentence has to name the flag that replaced the key rather than merely saying it moved
— *"is now `--no-physics`; it is a developer control, so it has no config key"* is the shape.
A row that leaves silently is the failure §7 forbids, committed by the fix for it.

**What I expect to be wrong about.** The boundary cases, and there are more of them than the
table above admits. `audio.enabled` stays but `physics.enabled` goes, which is defensible —
muting is a preference, skipping the world is a measurement — and is also exactly the kind of
line two people draw differently. `record.*` may well be a player feature in a game that ships
clip capture. `decodeBudgetBytes` and `physics.workerThreads` are kept as machine properties on
the argument that a user with different hardware legitimately holds a different value, which is
the same argument someone could make about `lightBudget`. Expect two or three of these to come
back.

The second risk is arithmetic. Moving a default from a macro row to a struct member is
transcription, forty times, and a transcribed float is exactly the error goldens catch and
review does not.

## Verification

- `scripts/kanban.py`, then `./build.sh`.
- `./test.sh debug`, `./test.sh asan`, `./test.sh tsan`, `./test.sh release` — each its own
  invocation. New cases: a config file naming all forty departed keys produces forty sentences
  and no silent drop; every surviving row's module is one of the eight that remain, so the
  drift cannot recur unnoticed; `ProfilerConfig` has one default rather than two.
- `scripts/golden.sh` — eleven cases, **byte-identical**, and this is the whole correctness
  claim. `tonemap`, `shadowDepthBias`, `shadowNormalBias` and the four budgets move to
  `GameSetup` carrying their current values; if an image moves, a default was transcribed
  wrong. Fix the value, do not re-snap.
- `substrate.json` at the repo root regenerated with `--write-default-config`, and the previous
  one kept aside to run once as the "old file" the sentences are checked against.
- Zero validation errors with layers on — `render.validation` changing homes is the one thing
  here that could silently stop requesting them.

## Reference update

- [architecture/principles.md](../../architecture/principles.md) — §7 gains the audit's result:
  which modules the table holds, and the rule that a developer control never gets a key.
- [architecture/tooling.md](../../architecture/tooling.md) — `## Configuration` loses the rows
  that left and gains where each went.
- [guides/making-a-game.md](../../guides/making-a-game.md) — the budgets and the tonemap are
  `GameSetup` decisions now, which is a change to the first thing a game author writes.

## Outcome

**Thirty-nine keys left, not forty, and the difference is a finding rather than arithmetic.**
Two of the card's forty stayed and one it never counted went. What the row actually
established is that §7's test has to be applied to *what a value is for* and not to the name
it happens to carry — which is the same lesson D11 recorded about lengths, arrived at from
the other end.

### The test, and the three answers it gave

*Is this a property of the person running the program, or of the program?* Each of the
ninety-five rows was asked, and every removal was grepped across `scripts/`, `docs/`, `game/`,
`engine/` and the root scripts before a line was deleted. The answer to *"who drives this"*
came back the same way D13's did: **a script passes only developer controls**, so
`golden.sh`, `readback.sh`, `locomotion.sh`, `baseline.py`, `rdoc.sh` and `build_release.sh`
needed no change at all — the flags they pass are `--frames`, `--trace`, `--audio-null`,
`--bake-scene`, `--locked`, `--headless`, `--msaa`, `--debug-view`, `--no-rt` and the capture
and readback blocks, and every one of those still exists.

**Twenty-eight went to a `Config` field with a named flag.** `render.validation`,
`syncValidation`, `rayQuery`, `shaderHotReload`, `debugOverlay`; `scene.bakeCache`;
`physics.enabled`, `debugDraw`, `debugContacts`; `audio.backend`, `debugDraw`; `ui.panel`,
`inspector`; all six `profiler.*`; all four `record.*`; all four of `logging.*`; and
`benchmark.exitAfterFrames`. Five flags came back to carry them — `--hot-reload`,
`--log-file`, `--log-output`, `--log-categories` and `--no-profiler` — and one of those is a
flag **D13 retired**, which reads as a reversal and is the rule stated correctly: `--hot-reload`
was wrong while `render.shaderHotReload` was a row, and is right the moment it is not. D13's
outcome predicted exactly this and called it *"they stop being exceptions and become the
rule"*.

**Seven went to `GameSetup`.** `render.tonemap`, `shadowDepthBias`, `shadowNormalBias`, and
the four budgets `lightBudget`, `particleBudget`, `physics.bodyBudget`, `audio.voiceBudget`.

**Four went to the game that reads them, as declared rows.** `ui.panelX/Y/Width/Height` are
`demo.panelX` and its three siblings, declared in `DemoGame::declareSettings` with the same
bounds and the same built-ins — which is what the card said would happen *"once D17 lands"*,
and D17 had. The engine draws no panel and read none of these; the demo did.

### What was kept against the card, and why

**`render.debugFont` and `render.debugFontHeight` stayed.** The card grouped them with
`debugOverlay` as *"the developer overlay and its font"* and that is true of the name and not
of the value: `Renderer::debugFont` is the **only** font atlas in the engine, and
`fontMetrics()` hands it to `ui::Context`, so it is the typeface and size of the panel, the
inspector, the binding menu and every string a game draws. A typeface and a size are a
property of whoever is reading them. `ui.scale` magnifies the embedded bitmap font in integer
steps only, and `limitations.md`'s *"Only integer UI scaling"* section already names a TTF
here as the answer for anyone that does not fit — so removing these would have taken an
accessibility escape hatch out of the config file in order to tidy a module boundary. They
keep their rows and their comment now says what they are.

**`logging.categories` went, and the card never listed it.** It is an aggregate rather than a
row, so it looked out of scope, and leaving it is what made it in scope: with
`logging.file`, `level` and `output` gone, `claimsModule("logging")` answers false, which
under D17's rule makes `logging` **a module a game may declare into** — while `Config` was
still parsing a key out of it. Two owners of one JSON section is a defect nobody would find
until it happened. It is `--log-categories a,b,c` now, `kParsedElsewhere` is down to
`input.bindings` alone, and `input` keeps three rows of its own so the same hazard cannot
reach it.

**`audio.enabled` stays and `physics.enabled` goes**, exactly as the card predicted somebody
would argue about. Muting is a preference; skipping the world is an attribution arm.
`physics.workerThreads` and `audio.decodeBudgetBytes` stayed for the reason the card gave and
it survived contact: both are properties of the *machine*, and the budgets are properties of
the *content*.

### What the removals cost, and what they closed

**Five modules are empty and are therefore a game's**, which D17's outcome flagged as a
consequence worth knowing before it happened: `scene`, `profiler`, `record`, `logging` and
`benchmark`. Every key they used to hold is refused by name, because `declare` walks
`removedKeys()` — the mechanism D17 built for exactly this, and it needed no change to cover
thirty-nine more.

**D12's residue is closed as it said it would be.** `kNamedRows` is deleted: seven entries
erased to two function pointers, existing only because a table of rows had to hold four
different enums. Every one of those settings is now a field of its own enum type, parsed once
at the flag that carries it, so the five accessors that resolved a name with a `value_or`
neither door could reach are gone with them — and with them the third door the `value_or`
covered, the panel's text field over a `String` row. `--set`'s `namedRowFor` detour is gone
too: the branch is the split on the first `=` and one call, which is what D13 wanted it to be
and could not have while a row held a name. The refusal itself did not go anywhere. It is a
seven-line `setName` template over `Names<E>`, reached by six flags, and it refuses *before*
the field is written rather than after the row was.

**`ProfilerConfig` has one default.** `Config::profiler` **is** a `ProfilerConfig`, so
`Engine::init` copies nothing and there is no second table to disagree with. The struct keeps
its empty `outputFile` — *"write nothing to disk"* is the right library default for a caller
that did not ask for a trace — and the engine states `debug_frames/profile.json` in `Config.h`
where `--trace` overrides it. D6's agreement test was **deleted rather than adjusted**,
because what it asserted no longer has two sides; its replacement pins the divergence's owner.

**`bindRenderer` lost four bindings and `Engine::endFrame` lost a poll.** `render.tonemap` was
one of the two rows with no field to bind and was re-read once a frame; keeping that poll
would have overwritten the demo's F11 cycle every frame, which is how the tonemap's new home
turned out to be simpler and not merely different.

**A budget default is spelled once.** `gfx::kDefaultLightBudget` derived from
`core::defaults::render::lightBudget` and that row is gone, so it is a literal in `Renderer.h`
and `GameSetup::lightBudget` defaults to **zero, meaning "the engine sizes it"** rather than a
second 32 beside it. All four budgets read that way, which also preserves the
zero-means-derive-from-the-data rule `particleBudget` and `bodyBudget` already had. That is
D11's *"two spellings of one value"* prohibition applied to a removal rather than to an
addition, and it is the transcription risk the card named: **no default moved**, which the
golden set is the proof of.

### The test fixtures that had to move, which is a finding of its own

Three cases were written against rows that no longer exist, and where each landed says
something about what is left. `SettingsUi`'s two cases used `scene.bakeCache` because `scene`
held exactly one row — they use a **declared** row now, which is a better fixture anyway: the
panel is supposed not to know whether a row is the engine's or a game's, and this checks it as
a side effect. `SettingsTests`' `initOnly` cases moved from `render.lightBudget` to
`audio.sampleRate`. And the `String`-row cases moved to `render.debugFont`, which is now the
**only** user-writable string row in the table — worth noticing rather than hiding: a table
with one string row is a table whose string door is barely exercised, and that is a
consequence of removing four `logging`/`profiler`/`record` paths and seven name-valued rows in
one card.

### Verification

- `scripts/golden.sh check release` — **11 of 11, byte-identical.** The whole correctness
  claim of a D row, and the one the card said the arithmetic risk lived or died on: seven
  defaults were transcribed into `GameSetup` and four more into `Renderer.h`, and not one
  pixel moved.
- `scripts/readback.sh release` — **9 of 9 bit-identical**, plus the lit silhouette exact and
  the resize soak clean across 12 swapchains.
- `scripts/locomotion.sh release` — **3 of 3 arms**, 8.40 m travelled on the walk-run-jump arm.
- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan` — **858 tests, 88
  suites, all four green**, each its own invocation. Four cases are new and eleven were
  rewritten: every key in `removedKeys()` asserted not to be a row and to carry a sentence,
  driven through a real config file; every surviving row's module checked against the eight
  that remain, so a ninth does not pass unnoticed; `--log-categories` through its new door;
  and `ray-query` unreachable from *every* string door rather than merely refused late by one.
- **Zero validation errors**, layers confirmed on:
  `./run.sh demo debug -- --headless --locked --frames 90 --audio-null --panel --validation on
  --set demo.impactVolume=0.25`. `render.validation` changing homes was the one thing the card
  said could silently stop requesting the layers, and the log line reads *"validation on"*.
- `--dump-settings` — **63 rows**: 57 the engine's list holds, in the eight modules above, plus
  the demo's six. `--write-default-config` — 8 sections, 62 keys, parses under Python's `json`
  with no duplicate member.
- **The stale file, both ways.** The previous `substrate.json` kept aside and run as the "old
  file": every departed key it holds gets its sentence and nothing is silent. Then a config
  naming **all sixty** keys `removedKeys()` holds — the twenty-one from before plus this
  card's thirty-nine — produces sixty warnings, zero *"unknown setting"* and zero orphan
  messages, which is the both-arms form: a key that fell through would land in one of those
  two and be visible.
- `./new_game.sh d14check` then `./build_game.sh d14check debug` — a freshly scaffolded game
  builds with no edit under `engine/` and reports `d14check.showRenderSettings bool true
  default`, with its `setDefault(window::vsync, true)` correctly losing to the repo's
  `substrate.json`. The template needed two edits: its teaching example wrote `ui.panel`,
  which is not a row any more, and it opened its panel through that row rather than through
  `Engine::setUiVisible`. Deleted afterwards and `build/debug` restored to `demo`.

### What the settings arc provides, now that it is closed

D11 through D17 answer the whole of the question the arc opened with, and D14 is the row that
made the answer true of the file rather than only of the mechanism. A value spelled by name
has one list of names and a typo is refused with the legal values printed (D12). Every row
reaches the command line by its own key, and a named flag is correct only for a developer
control (D13). Four sources answer one row in an order that is a sequence rather than a claim,
and a game's default loses to the user's file (D15). The table grows without any row losing
its compile-time handle (D16). A world-unit constant is a row or a derivation and never a
literal (D11). A game declares its own rows into the same table under one namespace rule and
one refusal ladder (D17). And **the table holds only what a player would recognise** (D14):
fifty-seven rows in eight modules, with every value that failed the test in one of the other
two homes and every key that left carrying the sentence saying which.

What is left open is one thing, and it is not the arc's: `--set` now has no name-valued row to
reach, so the day a game wants `easy|normal|hard` the `Named<E>` row type D17 argued against
building is the card that opens. That is the same Rule-of-Threes answer, one card later, and
nothing about it got harder.
