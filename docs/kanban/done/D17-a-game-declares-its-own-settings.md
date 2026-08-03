---
id: D17
title: A game declares its own settings
arc: D
size: M
verification: tests-4, golden-11
---

# D17 — A game declares its own settings

Afterwards a game adds rows to the settings table from a new `Game::declareSettings`, and every
consumer treats them as ordinary: they load from `substrate.json`, save back to it, appear in
`--dump-settings`, take `--set demo.difficulty=3`, draw in a generated panel and clamp to their
declared bounds. None of those consumers learns that a game exists.

The engine has a table of ninety values a user can hold an opinion about and a game has none,
which is backwards for the thing being built: a game is where difficulty, mouse sensitivity,
subtitle size and colourblind mode live, and every one of those is a preference by
[principles.md](../../architecture/principles.md) §7's own test. Today a game either
hand-rolls a second config file — a second parser, a second save, a second panel, and two
files a user has to know about — or ships without settings.

**The hook is not `configure`, and the ordering is the whole design.** `configure` runs after
the config file and the command line have both been applied, and `loadJson` walks the *file*
rather than the table precisely so that a key nothing claims produces a message. A row declared
after that has already been reported as the user's typo. Deferring those messages instead is
worse in three ways: they would move past `Logger::init`, so a user running at `error` level
would stop being told their moved key moved — which is the obligation the removed-key
mechanism exists to meet, and one [D14](D14-re-home-the-rows-that-are-not-user-configuration.md)
is about to load forty more keys onto; `--write-default-config` and `--help` exit from inside
the command-line parser, before `configure` runs at all, so a deferred design writes a default
file with no game section; and replaying a stashed value through the string door is not
lossless — a 64-bit integer above 2⁵³ rounds through the double the string parser uses, a float
loses precision through `%g`, and stringifying discards the JSON node's own type, which is what
the wrong-type refusal is made of.

Declaring before the file is read costs a seventh method on `Game` and makes all of it
disappear. The split is enforced rather than documented: `declareSettings` gets no `GameSetup`
and may only add rows, `configure` may only write them, and the schema freezes the moment the
first returns. A `declare` from `configure` is refused with a message naming the right method.

That freeze is also what makes the storage safe. Two reads on `Settings` return a reference
into a slot, so a registration between taking one and using it would dangle; freezing before
anything binds or reads means the addresses are stable for the process. It is the symmetric
twin of the existing init-only freeze — one refuses a *write* after the thing it sized was
sized, the other refuses a *row* after everything that reads rows by name has read them.

**No macro.** The engine's list is an X-macro because a name appeared by hand in four places;
for a game row all four are generated from the one `declare` call, leaving the member and the
call itself. Two is a coincidence. The macro that generates both from one list is designed and
written down, and opens as a card when a second game duplicates the pattern.

**No enum rows yet either**, for the same reason: `declare` takes a `Named<E>` list and the
table grows the type when a game wants `easy|normal|hard`, reusing
[D12](D12-one-name-list-per-enum-and-a-parse-that-refuses.md)'s name tables rather than a
parallel mechanism. The handle still carries the type, so a misspelled member is a build error
and `set(demo.difficulty, true)` does not compile — which is the part of the engine's
compile-time safety that survives ids being assigned at runtime.

The refusal ladder is where the care goes, and one entry is easy to miss: a game reusing a key
that *used to be* an engine setting. After D14 there are forty of those in the wild, and every
config file still carrying one would silently start feeding a row it was never written for.

**What I expect to be wrong about.** That the demo is a fair test of this. It will declare two
rows and read them, which proves the mechanism and proves nothing about a game with thirty
across four modules — where the panel's per-module filter, the save's section handling and the
`--help` story all get their real exercise. The scaffolding template gets one row for the same
reason, and that is still one game.

## Verification

- `scripts/kanban.py`, then `./build.sh` — the engine must still build, test and sanitize with
  nothing under `game/` in the tree, which is what makes this a hook rather than a dependency.
- `./test.sh debug`, `./test.sh asan`, `./test.sh tsan`, `./test.sh release` — each its own
  invocation. Cases: a declared row is an ordinary row through both doors; it is found by key
  and appears in both dumps; it saves under its own section and reloads as `Source::Config`;
  a module an engine row owns is refused, as are `engine.`, a duplicate, a malformed key and a
  removed engine key; nothing may be declared after the freeze; and two tables are independent
  — the case that would be impossible with a global registry, and therefore the one that
  records why the lookup is a member.
- `./build_game.sh demo`, then `scripts/golden.sh` — eleven cases, byte-identical.
- By hand: `--set demo.difficulty=3` reaches it; `--write-default-config` writes a `demo`
  section; the panel draws it; a value edited in the panel and saved survives a restart.
- `./build_game.sh` against a freshly scaffolded game, which is what
  [G8](G8-what-a-game-cannot-reach-yet.md) verifies and what the template row is for.

## Reference update

- [architecture/tooling.md](../../architecture/tooling.md) — `## Configuration` gains game rows,
  the declaration ordering rule and the refusal policy.
- [architecture/principles.md](../../architecture/principles.md) — §7's first home is no longer
  the engine's alone, and `Game`'s virtual count goes from six to seven with two run-once
  methods becoming three.
- [guides/making-a-game.md](../../guides/making-a-game.md) — declaring a setting is a thing a
  game does, and this is where it is written down.

## Outcome

**The hook is seven lines and everything else was already there, which is the outcome the
five rows before this one were buying.** `Game::declareSettings(Settings&)` is a new virtual;
`Engine::init` calls it and then `freezeRows()`, immediately before `loadFromFile`. Nothing
else in the engine changed to serve a game's row. `--dump-settings`, `--dump-settings=json`,
`saveJson`, `writeDefaultConfig`, `--set`, `setFromString`, the clamp, the provenance column
and `ui::drawSettings` were all already written against `rowCount()` rather than against
`count()`, so a declared row appears in every one of them without a line of new plumbing.
The demo's two rows show up in a live dump as `demo.impactVolume float 0.25 cli --set` beside
`render.ssao`, and there is nothing in the code that prints them that knows the difference.

### The namespace decision: a game owns every module the engine does not name

`mygame.difficulty` is a game's. `render.msaaSamples` is refused, and so is every other
`render.` key **whether or not a row claims it today** — which is the part that took the
argument. The alternatives were a single reserved `game.` prefix and a key-by-key check, and
both are worse:

- **A single `game.` module** would flatten a game with thirty rows across four concerns into
  one section of hand-prefixed keys — `game.audioSubtitleSize` — which is the "two spellings
  of one value" failure this table exists to remove, arrived at from the other end. The panel
  filters by module, so it would also be one panel section for everything.
- **A key-by-key check** — *refuse only keys the engine currently has* — is not answerable at
  the moment of declaration, because the engine's list is not finished. "Not a row today" is
  not "not the engine's", and a game shipping `render.myThing` would be a live defect the day
  an engine release added that key: the user's value would silently start feeding different
  code, in a file that groups by module and cannot show which half a key belongs to.

Module granularity also falls out of the file's own shape rather than being imposed on it.
The key **is** the JSON path; `substrate.json`, the generated panel and
`--write-default-config` all group by module; so the module is already the boundary every
consumer draws, and making it the ownership boundary adds no concept. `engine.` needed no
special case at all — it is an engine module like any other, and the test that refuses
`render.` refuses it for the same reason. `AGameOwnsEveryModuleTheEngineDoesNotName` walks the
engine's own list rather than a list of module names written in the test, because a list of
names in a test is exactly the thing that stops being maintained.

**The entry the card said was easy to miss was real and is the one that does not follow from
the module rule.** A key that *used to be* an engine setting is refused by name, and the shape
that gets past everything else is `lighting.sun`: the engine does not name `lighting` any
more, so nothing about the module objects, while every `substrate.json` still carrying that
key would silently start feeding a row it was never written for. The check is a walk of
`settings::removedKeys()`, so D14's forty are covered the day it lands without anyone
remembering to. A *new* key in that module is accepted — `lighting.torchWarmth` is a game's,
because no file in the wild means it.

One entry the card did not name and the ladder needed: **`kEngine` is refused from `declare`.**
That flag means live state the engine reports through no JSON key, so a game's row wearing it
would be one the string door refuses, the save never writes and the panel draws as a readout —
a control that reports success and changes nothing, which is the failure the panel exists to
prevent, reached from the declaration side. `kInitOnly` is *not* refused: a game sizing
something at startup is ordinary, and the table already refuses the late write with a reason.

### The orphan answer: refused, and kept

A key belonging to a game row that was not declared this run — the game removed the setting,
or a second game is running, or the engine's own scene is loaded with no game at all — is
**refused and left in the file**.

Refused is D12's convention and needed no new mechanism: the value is not applied, the run
continues, and a message names what happened. What it *did* need was a different sentence.
`loadJson` used to answer every unclaimed key with *"unknown setting `x` — see
--dump-settings"*, which is true of a typo and misleading about an orphan, and the two call
for different actions. So the loader now asks once per section whether any row of the table
lives in that module, and a key in a module nothing declares gets its own message saying so
and saying the key is being left alone.

**Kept is the half that matters, and it was already true — what this row did was notice, state
and test it.** `saveJson` merges into the document it read rather than rewriting one from the
table, so a key it does not understand survives untouched. That is what makes the whole
arrangement safe: a game that drops a setting in v2 and restores it in v3 does not cost the
user their answer, and two games sharing one `substrate.json` do not erase each other's
sections. `AnOrphanedGameKeyIsRefusedAndLeftInTheFile` loads an undeclared `demo` section,
saves a *different* row, and reads the orphan back through a second table that declares it —
`0.25`, `config`. The alternative — dropping unrecognised keys on save — would have looked
tidier and been a config file that edits itself, which is the rule `saveJson` already had.

### What was deliberately not built, and one place a previous card overstated this row

**No macro and no enum row**, both as the card argued. The engine's list is an X-macro because
a name appeared by hand in four places; for a declared row all four come from the one
`declare` call, leaving the member and the call — two occurrences, and two is a coincidence.
The enum row is the same shape: `declare` would take a `Named<E>` list, reusing D12's tables
rather than a parallel mechanism, and the table grows the type when a game wants
`easy|normal|hard`. **D16's outcome listed "the `Named<E>` enum-row type" as D17's**, and that
was an overstatement of this row's scope — the card it was describing argues against building
it, on the same Rule-of-Threes ground, and building it would have been a row type, a per-row
value list and a width field with no consumer. It is written down in
[tooling.md](../../architecture/tooling.md#a-game-declares-its-own-rows) and opens as a card
when something needs it.

**The compile-time safety survived intact and is now checked by the compiler rather than
asserted.** `declare` answers a `Setting<T>`, and `set` takes `Setting<T>` and `T`, so
`set(difficulty, true)` on an integer row deduces `T` as both `int` and `bool` and does not
compile — it is not a conversion that happens to be checked, and there is no runtime path it
could fall down instead. `ADeclaredRowsHandleRefusesTheWrongTypeAtCompileTime` asks the
compiler that question directly through a detection idiom, including the two near misses that
would be easy to assume are refused and are worth pinning: an `int` into a `Float` row, and a
string literal into a `String` row.

### What the card was right to expect to be wrong about, and how it came out

The card said the demo would be a poor test of this, and the way it turned out poor was not
the way the card guessed. It predicted *"it will declare two rows and read them"* — and the
harder question was **which two**. The card names `demo.difficulty` throughout, and the demo
has no game to be difficult: a `demo.difficulty` here would be a row nothing reads, which is
the control that moves, reports success and changes nothing — precisely the failure the table
and the generated panel exist to remove, committed by the row that exists to demonstrate them.
So the demo declares two rows it genuinely honours, both preferences by §7's own test:

- `demo.impactVolume`, a float scaling the one-shot at every collision. Not
  `audio.masterVolume`, which moves the music and the ambience with it.
- `demo.impactDust`, a bool gating the landing effect. Not `render.particles`, which is the
  whole subsystem.

Both read in `playImpacts`, both drawn by `drawSettings(ui, e.settingsTable(), "demo")` beside
the generated render block, both defaulting to today's behaviour exactly — which is why the
golden set did not move. What the card's *real* prediction says stands: two rows in one
function proves the mechanism and proves nothing about the panel's per-module filter, the
save's section handling or the `--help` story at thirty rows across four modules.

The scaffolding template gets one row, `<name>.showRenderSettings`, and it is read in `drawUi`
to decide whether the render block is drawn — so a scaffolded game demonstrates the whole loop
rather than a call. The template also lost the four-line `source(...) == Source::Default`
idiom D15 replaced: it was still teaching the pattern that card deleted, three lines above
where a game now declares.

### Verification

- `scripts/golden.sh check release` — **11 of 11, byte-identical.** The whole correctness
  claim of a D row: two new rows, two new reads and a new panel section, and not one pixel.
- `scripts/readback.sh release` — **9 of 9 bit-identical**, plus the lit silhouette exact and
  the resize soak clean across 12 swapchains.
- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan` — **854 tests, 88
  suites, all four green**, each its own invocation. Seven are new: the namespace rule walked
  over the engine's own module list; the retired-key and `kEngine` refusals; the orphan,
  through a load, a save and a second table that declares it; the typed handle asked of the
  compiler; a declared row through `Config`'s real doors in the real order (declare, freeze,
  file, `--set`); and the generated panel drawing and writing a game's module.
- `./build.sh` alone — the engine still builds, tests and sanitizes with nothing under
  `game/` configured, which is what makes this a hook rather than a dependency.
- `./new_game.sh d17check` then `./build_game.sh d17check debug` — a freshly scaffolded game
  builds with no edit under `engine/`, runs, and reports `d17check.showRenderSettings bool
  true default` in its dump. Its `setDefault` on `ui.panel` correctly lost to the repo's
  `substrate.json`, which reads `config`.
- `./run.sh demo release -- --headless --locked --frames 90 --audio-null --panel --validation
  on --set demo.impactVolume=0.25` — **zero validation errors**, layers confirmed on, with the
  generated panel drawing the `demo` module every frame.
- `--dump-settings` — **97 rows**: the 95 the engine's list holds plus the demo's two, with
  `demo.impactVolume` reporting `cli` / `--set`.
- `--write-default-config` — 13 sections, parses under Python's `json` with no duplicate
  member, and `"demo": {"impactVolume": 1, "impactDust": true}` is one of them.
- The ordering itself, which `Engine.cpp` is not hosted to test, in two live runs. A config
  file naming `demo.impactVolume` reports `config` — a row declared *after* the file was read
  would report `default`, which is the whole argument for the hook's position. And a temporary
  `declare` from `configure`, built and run and then reverted, is refused with the message
  naming the right method:

```
`demo.late` cannot be declared: the settings schema is frozen. A row is declared from
`Game::declareSettings`, which runs before the config file is read; `configure` is where a
game *writes* a row it has already declared
```

- The three unclaimed-key sentences, in one run against a file holding all three:

```
`oldgame.difficulty` is not a setting in this build: nothing declares the `oldgame` module.
    The value is left in the file untouched -- ...
unknown setting `render.ssaoo` -- see --dump-settings for every key there is
`lighting.sun` moved into game code -- authored lighting
```

### What the arc provides now, and what is left

D11 through D17 close with the settings table answering the whole of the question the arc
opened with. A value spelled by name has one list of names and a typo is refused with the
legal values printed (D12). Every row reaches the command line by its own key, and a named
flag is correct only for a developer control (D13). Four sources answer one row in an order
that is a sequence rather than a claim, and a game's default loses to the user's file (D15).
The table grows without any row losing its compile-time handle (D16). A world-unit constant is
a row or a derivation and never a literal (D11). And a game declares its own rows into the
same table, with one namespace rule and one refusal ladder, and reaches every consumer the
engine's rows reach (D17).

**[D14](D14-re-home-the-rows-that-are-not-user-configuration.md) is the arc's last row and it
inherits two things from here.** Its forty removed keys are covered by the declaration ladder
automatically, because that check walks `removedKeys()` rather than a list of its own. And
every row it takes *out* of the table frees a module: the day `render.validation`,
`render.tonemap` and `logging.level` stop being rows, the modules they were in are still the
engine's — `render` and `logging` both keep other rows — but a module D14 empties entirely
becomes a name a game may take, and that is a consequence worth knowing before it happens
rather than after.
