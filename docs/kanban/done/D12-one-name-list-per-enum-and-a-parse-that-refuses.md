---
id: D12
title: One name list per enum, and a parse that refuses
arc: D
size: S
verification: tests-4, golden-11
---

# D12 — One name list per enum, and a parse that refuses

Afterwards every value spelled by name — a tonemap operator, a debug view, a log level, a log
destination, an `auto|on|off`, an audio backend — has exactly one table of names, and a name
that table does not hold is **refused with the legal values listed** rather than silently
becoming the default.

`Config.cpp` parses six of these through `lookup(table, text, fallback)`, whose whole contract
is the failure: an unrecognised name yields `fallback`. So `--tonemap reinhardt` starts the
program in ACES and says nothing, `--log-level verbse` runs at status and says nothing, and
`"validation": "of"` in a config file reads as `auto`. The comment above `lookup` defends this
— *"refusing to start over a typo in a benchmark sweep costs more than the typo"* — and that
argument is wrong in the only case it covers: a benchmark sweep that silently measures the
wrong arm produces a number, and a number nobody can tell is wrong is worse than an exit code.
Refusing the *value* while leaving the previous one standing costs the sweep nothing and tells
the truth.

This is [principles.md](../../architecture/principles.md) §7's first prohibition — *a key that
parses and does nothing* — committed by the parser rather than by the config file. The settings
table removed it from one door and left it standing in the other.

The second half is that four of these enums have their names written twice. `gfx::tonemapKey`
and `kTonemapAliases` are two lists of the same operator's spellings, free to disagree;
`debugViewKey` and the loop in `Config::debugView` are the same pair again. An operator added
to the enum and to `tonemapKey` but not to the alias table is reachable from code and not from
a file, with nothing to catch it. `Named<T>` becomes the one list and `tonemapKey(op)` becomes
"the first entry whose value is `op`", which is also what makes a canonical spelling a thing
that exists.

Two enums are introduced because their values are owned by nobody and three callers each reach
them: `Tristate` (`auto|on|off`, for validation, ray query and shader hot reload) and
`AudioBackend`. `Tristate` carries `enabled(v, whenAuto)` rather than resolving `auto` itself,
because what `auto` means differs per caller — the build type for validation, the device's
offer for ray query — and that difference is the whole reason the third state exists.
`Config::render.debugView` stops being a `std::string` and becomes `gfx::DebugView`.

**What this does not build.** `Type::Enum` in the settings table. [D14](D14-re-home-the-rows-that-are-not-user-configuration.md)
moves every enum-valued row out of the table, so a row type, a per-row value list, a trait and
a width field would be machinery with no consumer. When a game declares an enum row —
[D17](D17-a-game-declares-its-own-settings.md)'s follow-on — `declare` takes a `Named<E>` list
and the table grows the type then, with something to serve.

**What I expect to be wrong about.** The alias policy. `none` for `clamp` and `warning` for
`warn` are input conveniences today that round-trip unchanged; making the first entry canonical
means a file saying `none` is rewritten as `clamp` on the next save. That is right — the alias
is an input, the file is output — but it is a behaviour change nobody asked for, and it may
turn out that some caller depends on the spelling surviving.

## Verification

- `scripts/kanban.py`, then `./build.sh`.
- `./test.sh debug`, `./test.sh asan`, `./test.sh tsan`, `./test.sh release` — each its own
  invocation, never chained. `ConfigTests` is the suite: an unknown name refused and the old
  value standing, for each of the six; case-insensitivity and aliases; and a loop over
  `0..Count` for every enum asserting each value is reachable by some name — the guard that
  catches an operator added to the enum and not the table.
- `LogLevelNamesThatAreNotNamesFallBackToStatus` asserts the behaviour being deleted and is
  **removed rather than adjusted**. A test that pins a fallback cannot survive the card that
  removes fallbacks.
- `scripts/golden.sh` — eleven cases, byte-identical. No default moves and no value changes;
  only what happens to a name that was never legal.
- By hand: `timeout -s TERM 30 ./run.sh demo -- --tonemap reinhardt --frames 1` prints a
  refusal naming all seven operators and runs in ACES because that is what it already was —
  not because a fallback chose it.

## Reference update

- [architecture/tooling.md](../../architecture/tooling.md) — the `## Configuration` section
  gains the rule: a value spelled by name is refused when it is not one, and the message lists
  what it could have been.
- [architecture/principles.md](../../architecture/principles.md) — §7's *"a key that parses and
  does nothing"* prohibition is stated for the command line too, not only for the config file.

## Outcome

**The mechanism is `engine/core/Names.h`, and it is three functions over one list.** A
`Named<T>` list per enum, living beside the enum it names, and `nameOf` / `parseName` /
`legalNames` derived from it. `gfx::tonemapKey` and `debugViewKey` survive as one-line
wrappers over `nameOf` because three call sites read better for them, but they are no longer
a second list — they *are* the list, read backwards. That is what makes the round trip total
by construction rather than by two authors agreeing.

Seven enums converted, and where each list lives is in
[tooling.md](../../architecture/tooling.md#names-and-the-parse-that-refuses):
`gfx::TonemapOperator`, `gfx::DebugView`, `core::LogLevel`, `core::LogOutput`,
`core::LogCategory`, the new `core::Tristate` and the new `scene::AudioBackend`.

**The card predicted four enums with their names written twice and there were five.**
`LogCategory` was the one it missed, and it was the worst of them: `Logger.cpp`'s
`categoryName` printed `Vulkan` on every line while `Config.cpp`'s `kLogCategories` parsed
`vulkan`, two switch-and-table pairs that never met. A category added to the enum could have
been printable and unparseable, or the reverse, and nothing would have said so. One list now,
spelled the way the log line spells it and matched case-insensitively, so the word that turns
a category on is the word that appears beside it.

### The refusal, and why it is this and not an exit

**The value is refused; the run is not.** An unrecognised name leaves the setting holding
what it held — the previous value at the command line, the built-in default in a config file
— and logs an `error` naming the flag or the key, the text, every spelling that would have
worked, and what is standing instead:

```
--tonemap: `reinhardt` is not one of aces, reinhard, clamp, none -- keeping `aces`
```

Three decisions inside that, each deliberate:

- **Not a hard failure.** The card's argument for refusing is about a *measurement* that
  silently reports the wrong arm, and refusing the value answers it completely: nothing the
  typo asked for takes effect and the log says so. Exiting would additionally punish someone
  whose hand-edited `substrate.json` has one bad word in it, which is a different kind of
  damage from the one being fixed and not one anybody asked to have fixed.
- **The same at both doors.** `--tonemap` and `"tonemap":` are the same setting; a rule that
  differed by door is a rule nobody would remember. What differs is only *what is left
  standing*, and only because it has to: the file is the first door, so what stood before it
  is the built-in.
- **`error`, not `warn`.** One rung above the unknown *flag* beside it, and the distinction
  is real: an unknown flag is a word the program does not know, and this is an instruction it
  understood and could not carry out.

The two doors are implemented against one list, `kNamedRows` in `Config.cpp` — seven rows,
each erased to two function pointers, because the rows behind them name four different enums
and one table has to hold all of them. `loadFromFile` snapshots the seven values, applies the
file and settles them; `applyCommandLine` checks before it writes, which is why a refused
flag leaves the previous value's *provenance* untouched as well as its value. There is no
`Type::Enum` in the settings table, for the reason the card gave.

**Canonicalisation landed as designed and the risk the card flagged did not appear.** The
first entry naming a value is canonical, so `none` is stored as `clamp`, `warning` as `warn`,
`true` as `on`. Nothing depended on the input spelling surviving: `substrate.json` at the
repo root holds canonical values for all seven rows, and the only two code sites that write
one of these rows (`Engine::init`'s `pixelExact` block and the demo's F11 cycle) already
wrote canonical names because both go through `tonemapKey`.

### What else came out of it

`Config::render.debugView` is a `gfx::DebugView` rather than a string, and `Config::debugView()`
is gone with it — there is nothing left to derive. That closes a hole four golden cases were
one typo away from: `--debug-view sao` used to be accepted by the flag and resolved to `lit`
by the reader, so a case could have photographed the lit buffer and passed.

`Tristate` replaced three copies of a `bool triState(text, whenAuto)` whose `else` swallowed
every typo — `"validation": "of"` read as `auto` and said nothing. It carries
`enabled(v, whenAuto)` rather than resolving `auto` itself, exactly as the card specified.
`scene::AudioBackend` deleted `AudioEngine::init`'s own three string comparisons and its own
*"unknown backend -- using auto"* warning, which fired one subsystem after the name arrived
and could not name the key it came from. It lives in a new `scene/AudioBackend.h` for the
reason `gfx/DebugView.h` exists and states: the config parser has to turn `"backend": "null"`
into the value, and `core/Config.h` should not pull a scene graph and rapidjson behind it to
name three states.

**Deliberately left: `physics.clock`.** It has no config key and no name a person types —
`--locked` and `--realtime` are its only writers and both assign a canonical spelling — so
there is no door for a typo to arrive through and a list of two names would have no second
consumer. Its `namesEqual` comparisons now come from `Names.h`, which is what let `lowered`
be deleted. Also left: `--help`'s three inline name lists and `Logger.cpp`'s `levelStyle`.
The first is a fourth spelling and is real, but [D13](D13-any-setting-from-the-command-line-and-fewer-flags.md)
rewrites `--help` wholesale and a refusal now prints the legal values anyway; the second is a
*style* row carrying a colour and a stream, not a name, and no parse has ever accepted
`CRITICAL`.

**The totality guard became a compile error rather than only a test.**
`static_assert(core::namesEveryValue(table))` sits beside five of the seven lists, so an
enumerator added to the enum and not to the list does not build. The two masks —
`LogCategory` and `LogOutput` — have no `Count` for it to walk and are guarded by the suite
instead, looping over `AllLogCategories` and over `LogOutput::Both`, both of which a new bit
has to be added to anyway.

**What is still not total, stated rather than hidden.** The derived accessors resolve with a
`value_or`, and it is not the fallback this card removed: neither door can produce a row that
does not hold a name. What it covers is the generated panel's text field over a string row,
which is a third door `Config` does not own — and it does not log, because
`Engine::endFrame` polls `tonemap()` once a frame. `Config.cpp` says so above the accessors,
and [D14](D14-re-home-the-rows-that-are-not-user-configuration.md) closes it outright by
taking all seven rows out of the table.

### Verification

- `scripts/golden.sh check release` — **11 of 11, byte-identical**. This is the whole
  correctness claim of a D row and it holds: no default moved and no value changed.
- `scripts/readback.sh release` — 9 of 9 bit-identical plus the lit silhouette, and the
  resize soak clean.
- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan` — **821 tests,
  all four green**, each its own invocation. Nine cases are new or rewritten: every
  enumerator reachable by exactly one canonical name (a loop over `Count`, or over the mask
  for the two that have none); a default `Config` resolving to what each row's default names,
  which is what pins the accessors' last-resort enumerator against the table's string; an
  alias parsing and being rewritten canonically with its provenance intact; a config file
  naming all seven rows wrongly and getting seven refusals and seven defaults; a flag refused
  with the file's value and the file's provenance both left standing; and `--debug-view`,
  `--tonemap`, `--log-level`, `--validation` and `logging.output` each walked over their whole
  enum.
- `LogLevelNamesMapToLevelsAndAnUnknownOneFallsBackToStatus` was **removed rather than
  adjusted**, as the card required. Its replacement drives the same values through a door
  instead of through the typed setter, which is what makes it a test of the refusal rather
  than of the reader.
- `./run.sh demo release -- --headless --locked --frames 3 --audio-null --tonemap reinhardt
  --debug-view sao --log-level verbse --validation of` — four refusals, each naming its legal
  values, and the run continues in ACES on the lit buffer because that is what was already
  standing. The card said the tonemap refusal would name *seven* operators; there are three
  and an alias, which is what the enum has held since the dead entry was retired.
- `./run.sh demo debug -- --headless --locked --frames 90 --audio-null --validation on
  --panel` — zero validation errors, layers confirmed on.
- `--dump-settings` — all seven rows report the same value and the same provenance as before.

### What D13 through D17 can now assume

- **A name is a value.** Anything that arrives spelled by name has been checked at the door,
  so a consumer resolves rather than validates. D14 can move any of these seven rows to a
  `Config` field of its enum type and delete a `std::string` rather than move a parse.
- **`Names.h` is the shape an enum-valued setting takes.** When D17 has a game declare an
  enum row, `declare` takes a `Named<E>` list — that type exists now and is what every engine
  enum already uses, so the table grows one row type and no new vocabulary.
- **`kNamedRows` is where a new name-valued row registers**, and it is seven lines in
  `Config.cpp`. D13's `--set <key>=<value>` is a third door and has to consult it; that is one
  call to `namedRowFor` rather than a policy of its own.
- **`Tristate` and `AudioBackend` exist and are public**, so D14 does not have to invent them
  on the way past.
