---
id: D16
title: The settings table stops being a fixed array
arc: D
size: M
verification: tests-4, golden-11
---

# D16 — The settings table stops being a fixed array

Afterwards a row's default lives on the row, `Settings` holds a vector of slots rather than an
array sized by an enum, and `row()` and `find()` are members. Nothing behaves differently —
every existing test passes but for mechanical renames — which is what makes this reviewable in
one screen and worth landing on its own.

It exists to let [D17](D17-a-game-declares-its-own-settings.md) add rows, but three of the four
changes are corrections that stand without it.

**A default that only the constructor knows.** `saveJson` and `writeDefaultConfig` each need to
answer *does this differ from its default*, and each answers it by **constructing a whole
throwaway `Settings`** — because the default exists only inside the constructor's macro
expansion and there is nowhere else to read it. That is a hundred slots and a hundred strings
built to compare one value, on the save path, twice. Moving the default onto `Row` deletes both
constructions and the expansion. It also buys a check the list cannot make today: a `Float` row
whose default was written `0` instead of `0.0f` lands in the integer field and reads back
correctly *for the wrong reason*, and stays correct until somebody writes `1`. A tag on the
stored default and a `constexpr` walk in a `static_assert` catch it at build time. This is also
what unblocks [D11](D11-the-engines-world-unit-constants-come-from-the-scene.md), which names
the throwaway-defaults construction as the thing standing in the way of a row whose default is
derived.

**`Id::Count` cannot be both the boundary and the sentinel.** `find` returns it for *no such
key*, and two call sites test against it. Any scheme that gives a new row the id `Count` makes
the first such row read as unknown everywhere — in builds that have a game in them, which is
every build that ships and no build the unit suite runs. A separate `None` is the fix and it is
worth having before anything depends on it.

**Every getter indexes the slot array unchecked.** Safe while every `Id` was an enumerator;
not safe the moment one can be `None`. One private accessor with the clamp-and-answer-row-zero
policy `row()` already states in its own comment, warning once rather than every frame.

**`row()` and `find()` become members** because the alternative for D17 is a process-global
registry, and the header already says *"One instance, owned by `Engine`"* — a global would make
that comment false and would leak one test's rows into another's `saveJson`. The cost is
fifteen call sites outside `Settings.cpp`, four of them in one test file. The tempting middle —
a free `row()` that stays valid for engine ids beside a member that handles all of them — is
refused: two functions with one name where one silently answers wrongly for half its inputs is
exactly what `row()`'s own comment is written against.

Row metadata stays **two** arrays rather than one vector: the engine's `constexpr Row kRows[]`
is kept and registered rows go in a second. That makes *"a game adds rows, it does not edit the
engine's"* a fact about storage rather than a policy check somebody has to remember — the same
move `engine.` rows made with their flag — and it keeps the `static_assert` tying the table to
the enum. The cost is one branch in `row()`, on a path that is JSON parsing, a dump, and one
panel per frame.

**A bug this fixes on the way.** `writeDefaultConfig` opens a new JSON section whenever the
module changes while walking rows in order, and the list interleaves — `render.msaaSamples`,
then `scene.bakeCache`, then `render.validation` — so it writes `"render"` **twice**. The
round-trip test passes only because rapidjson tolerates duplicate members and the loader walks
all of them; every other JSON reader rejects the file. Iterating modules in first-appearance
order fixes it, and the fix is needed anyway once registered rows can interleave further.

**What I expect to be wrong about.** The `Default` type's converting constructors. Six
overloads taking `bool`, `int`, `unsigned`, `unsigned long long`, `float` and `const char*` is
the kind of overload set where an untyped literal in the list picks a constructor nobody
intended, and the `static_assert` is what catches it — so expect the first build to fail on
three or four rows whose literal was quietly the wrong type all along. That is the check
working, but it is not what the estimate assumed.

## Verification

- `scripts/kanban.py`, then `./build.sh`.
- `./test.sh debug`, `./test.sh asan`, `./test.sh tsan`, `./test.sh release` — each its own
  invocation. ASan is the one that matters: the unchecked slot indexing this card fixes is
  invisible without it. New cases: every row's fresh value equals its declared default; the
  written default config names each module exactly once (**fails today**); a handle naming no
  row reads as the first row rather than past the end.
- `scripts/golden.sh` — eleven cases, byte-identical. No value changes; this card is a
  refactor plus one file-format fix.

## Reference update

- [architecture/tooling.md](../../architecture/tooling.md) — `### The table`: where a default
  lives now, and why the written config groups by module.

## Outcome

**The table grows and the compile-time handles survived it in full, which was the one
outcome that could have made this card not worth landing.** The storage is two containers
rather than one, and that split is the whole design: the engine's rows stay a `constexpr
Row kRows[]` generated from `SUBSTRATE_SETTINGS` and indexed directly by an `Id` enumerator,
so `core::options::render::msaaSamples` is still `constexpr`, still typed, and a typo is
still a build error. A row `Settings::declare` adds goes in a deque the table owns, with an
id at or above `Id::Count`, and its handle is the `Setting<T>` `declare` returns — assigned
at run time, typed at compile time. Only the *id* stopped being a constant. Nothing became a
string lookup that was not one already, and the `static_assert` tying the row table to the
id enum still holds. The honest split is worth stating plainly because D17 inherits it: an
engine row is reached by a named constant and a game row by a handle the game stores, and
neither is reached by string except through the doors that were always string doors.

**`row` and `find` are members**, at a cost of fifteen call sites. The alternative for a
declared row was a process-global registry, which would have made the header's *"one
instance, owned by `Engine`"* false and leaked one test's rows into another's `saveJson`.
`TwoTablesDoNotShareWhatEitherDeclares` is the case that could not have been written at all
under the design that was refused, and it is why that test exists.

### The growth hazards, and what prevents each

Three, and they fail differently:

- **References into the table.** `getString` and `origin` return a `const std::string&`
  into a slot, `row` returns a `const Row&`, and a declared row's `key` and `label` are
  `const char*` into strings the table owns. A `std::vector` moves every one of those on the
  reallocation the next `declare` causes. **Both growable containers are `std::deque`**,
  which never moves what it already holds — a storage choice rather than a rule anyone has
  to remember, which is the difference between this and the three defects of the same shape
  this session already had. `GrowingTheTableMovesNothingAnythingElseIsHolding` takes one
  reference of each kind, declares 64 more rows, and reads them all back.
- **`bindLive`.** It stores the address of a field *outside* the table, so growth could
  never have moved it; the slot remembering it is what a vector would have moved. Both are
  covered by the same test, which drives the binding in both directions after the growth.
  `bindLive` on a handle naming no row is now **refused** rather than clamped: reading the
  first row for an unknown id is a wrong answer in one place, but binding it would write a
  wrong address every frame.
- **A row appearing after something has read the table.** `freezeRows` refuses it, and
  `freezeInitOnly` implies `freezeRows` — the latest defensible point while nothing declares
  anything, since by the end of `Engine::init` the dump, the panel and `bindRenderer` have
  all walked the table. That is the defined answer the card owed rather than an accident,
  and D17 moves the call earlier without inventing a mechanism.

**Provenance needed no special case for a declared row, and the one ordering that does not
work is written down as a test rather than left to be discovered.** A row declared before
`loadJson` takes `Config` from the file, loses a `setDefault`, and is beaten by `--set`,
exactly like an engine row. A row declared *after* the file was walked keeps its built-in
and its key was already reported as a typo — `loadJson` walks the file once, and that is
precisely the argument D17's card makes for declaring before the read rather than after it.

### The three corrections that stood on their own

- **The default moved onto the row.** `Row::builtIn` plus `settings::defaultString(row)`
  deleted both throwaway `Settings` constructions and the constructor's ninety-odd macro
  expansions — the constructor is now one loop over `kRows`, and `declare` builds its slot
  through the same function, so the two paths cannot disagree.
- **`Id::None` split from `Id::Count`**, so a count is a boundary and a sentinel is a
  sentinel. This was the entry that would have failed silently: the first declared row takes
  `Count` as its id, and both `!= Count` call sites would have read it as unknown.
- **Every getter went through one private accessor**, which is what makes a handle naming
  no row a wrong answer in one place rather than a read past the end. A write or a bind
  through such a handle is refused outright — a refused `declare` hands back exactly that
  handle, and it must not quietly assign `window.width`.

**The duplicate-section bug was real and is fixed.** `--write-default-config` opened a
section whenever the module changed while walking rows in list order, and the list
interleaves — `render.msaaSamples`, `scene.bakeCache`, `render.validation` — so it wrote
`"render"` twice. The round trip passed because rapidjson tolerates a duplicate member;
Python's `json` and every other reader reject the file. It groups by module in
first-appearance order now, and `TheWrittenDefaultConfigNamesEachModuleExactlyOnce` is the
test that would have failed before.

**What the card was right to expect to be wrong about, and how it came out.** The `Default`
converting constructors did catch mistyped literals, and there were **fifteen** rather than
three or four: every `Uint` row and both `Uint64` rows had written a bare `int` literal, so
`4` became `4u` and `67108864` became `67108864ull` across the list. None of them was a live
defect — the constructor cast to the declared `ctype` — but all fifteen were the same latent
one the card described, and the check now names the row at build time. The second half of
the guard turned out to be the *absence* of a `double` constructor: a `Float` row written
`1.0` is ambiguous and fails to compile before the assert ever sees it. What the estimate
missed in the other direction is that this changes `core::defaults::<module>::<row>` types
from `int` to `unsigned` for those rows, which nothing minded because every consumer already
assigns them to a declared `uint32_t`.

**What D17 inherits.** `Settings::declare(key, builtIn, label, min, max, flags)` returning a
typed handle; `freezeRows`/`rowsFrozen` to hang the declaration window off; and a refusal
ladder with the three structural entries in it — a malformed key, a key some row already
claims, and anything at all after the freeze. What D17 still owns is the `Game` hook and its
ordering, the `Named<E>` enum-row type, and the rest of the ladder: a module an engine row
owns, an `engine.` key, and a key that used to be an engine setting, which D14 is about to
add forty more of.

### Verification

- `scripts/golden.sh check release` — **11 of 11**, byte-identical.
- `scripts/readback.sh release` — **9 of 9** bit-identical, plus the lit silhouette.
- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan` — **847 tests,
  all four green**, each its own invocation. Ten are new: one over the default now living on
  the row, eight over growth and its refusals, and one over the written config's sections.
  ASan is the one that matters, and it is what says the private slot accessor and the deque
  storage are right rather than lucky.
- `./run.sh demo release -- --headless --locked --frames 90 --audio-null --panel
  --validation on` — clean, **zero validation errors**, with the generated panel walking
  the grown table every frame.
- `--dump-settings` — **95 rows**, the same 95 the list holds at `HEAD`, with values and
  provenance unchanged.
- `--write-default-config` — parses under Python's `json` with **no duplicate member**, 12
  sections, 46 keys under `render`.
