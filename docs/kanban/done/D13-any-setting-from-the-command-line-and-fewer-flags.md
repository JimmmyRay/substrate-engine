---
id: D13
title: Any setting from the command line, and fewer flags
arc: D
size: S
verification: tests-4, golden-11
---

# D13 — Any setting from the command line, and fewer flags

Afterwards `--set <key>=<value>` reaches every row the table holds, about fifteen named flags
that existed only to override one preference row are gone, and `--help` states when a new flag
is correct: only for a developer control that has no JSON key.

Two problems, one door. The first is coverage: roughly sixty rows have no flag at all, so *"set
`render.fogHeightFalloff` to 12 and tell me if it still happens"* is a reproduction step nobody
can follow without editing the user's own `substrate.json` — which loses the thing being
reproduced. The second is that the flag list has been growing without a rule. Thirty-six flags
exist to assign a settings row, `--help` is a hundred and ten lines, and the next preference
added would have argued for a thirty-seventh by symmetry.

`--set` answers both and needs almost no code, because `setFromString` already does the work:
it refuses an unknown key, an `engine.` key, a value of the wrong type and an `initOnly` row
after the freeze, each with the message it already has, and it answers a *moved* key with the
sentence saying where it went. The branch parses nothing on purpose — splitting on the first
`=` is the whole of it.

It is also the only door a game's row could ever have. [D17](D17-a-game-declares-its-own-settings.md)
lets a game add rows; a flag table a game contributed to would be the registry this tree has
refused twice, and `--set demo.difficulty=3` needs the engine to know nothing.

Which flags go is decided by what drives them, not by taste. `scripts/` passes fifteen:
`--frames --locked --debug-view --headless --config --msaa --no-ssr --no-rt --no-ibl --taa`,
the capture block, `--golden --diff --trace` and the RenderDoc pair. Those stay, and so do the
ones that mirror an F-key, because `--no-ssao` beside F8 is one idea spelled two ways for one
reason. What goes is the residue: `--vsync`, `--ui-scale`, `--stream-threshold`,
`--bloom-strength`, `--ssr-roughness`, `--taa-blend`, `--rt-distance`, `--particle-budget`,
`--body-budget`, `--physics-threads` and the rest of `kNumberFlags` and `kStringFlags` whose
row is a preference nothing automated ever sets. Each was a line in three places — the table,
the parser and `--help` — to save typing a key that `--dump-settings` already prints.

The percent scaling goes with them, and that is a small win of its own: `--bloom-strength 5`
means 0.05 through a `scale` column that exists because *"150% is a scale nobody would type as
1.5 twice"*. `--set render.bloomStrength=0.05` is the number the file holds and the dump
prints, so there is one representation instead of two.

**What I expect to be wrong about.** That the split is clean. Some retired flag is probably
load-bearing in a shell history, a `docs/guides/` example or a comment that says *"run with
`--vsync`"*, and grep across `docs/`, `scripts/` and the comment bodies has to happen before
the deletions, not after the test suite passes.

## Verification

- `scripts/kanban.py`, then `./build.sh`.
- `./test.sh debug`, `./test.sh asan`, `./test.sh tsan`, `./test.sh release` — each its own
  invocation. `ConfigTests`: an engine row set by name; an unknown key refused; an `engine.`
  key refused; `--set` with no `=` warns and changes nothing; the same key twice, last wins;
  and the value recorded as `cli` with origin `--set`.
- `scripts/golden.sh` — eleven cases, byte-identical, and this is the test that matters here:
  every case is driven by flags, so a retired flag that a case still passes shows up as an
  unknown-option warning and a changed image rather than as a silent no-op.
- `grep -rn -- '--vsync\|--ui-scale\|--bloom-strength' docs/ scripts/ engine/ game/` clean of
  every retired flag before the branch closes.

## Reference update

- [architecture/tooling.md](../../architecture/tooling.md) — `### Flags` states the rule: a
  named flag is correct only for a developer control with no JSON key; a preference gets a row
  and reaches the command line through `--set`.
- [architecture/principles.md](../../architecture/principles.md) — §7's three-homes table gains
  the same sentence, since the third home is what the rule is about.

## Outcome

**The spelling is `--set <key>=<value>`, split on the *first* `=`.** The first rather than
the only one, because a value may contain one — `--set profiler.outputFile=a=b.json` is a
path, not a second assignment — and that is the whole of the parsing, as the card said it
would be. The branch is thirty lines in `applyCommandLine`, most of them comment:

```bash
./run.sh demo release -- --set render.fogHeightFalloff=12 --set window.vsync=true
```

Everything else it does is delegate. `settings::find` decides whether the key exists;
`namedRowFor` (D12's hook, built for this) decides whether the row's value is a name, and
if it is, the value is canonicalised or refused through the same list `--tonemap` consults;
otherwise `setFromString` takes it, with the refusals it already had — an unknown key, an
`engine.` key, a value of the wrong type, an `initOnly` row after the freeze, and the
sentence saying where a *moved* key went. **There is no policy in this branch.** The value
lands as `cli` with `--set` in the origin column, so `--dump-settings` traces it exactly
like a named flag.

A refusal at this door and at a dedicated flag print the same sentence, differing only in
which door they name, because they are one setting:

```
--set render.tonemap: `reinhardt` is not one of aces, reinhard, clamp, none -- keeping `aces`
--tonemap: `reinhardt` is not one of aces, reinhard, clamp, none -- keeping `aces`
```

`--set` with no `=` logs an **`error`** and assigns nothing. The card said *warns*; it was
written before D12 settled the distinction, and by that rule this is an instruction the
program understood and could not carry out, one rung above the unknown *flag* it sits
beside. It also does not swallow what follows any further than the one token it consumed —
the same contract every valued flag has held since the flag tables landed.

### Twelve flags went, not fifteen, and the split was cleaner than the card feared

Retired, each of them a flag whose entire job was to assign one preference row that no
script drives and no key mirrors: `--vsync`, `--ui-scale`, `--stream-threshold`,
`--bloom-strength`, `--ssr-roughness`, `--taa-blend`, `--rt-distance`,
`--particle-budget`, `--body-budget`, `--physics-threads`, `--lod-threshold` and
`--hot-reload`. `kNumberFlags` went from fifteen entries to four.

**The `scale` column went with them**, which is the small win the card predicted and it is
real: `--bloom-strength 5` meant 0.05 through a column that existed because *"150% is a
scale nobody would type as 1.5 twice"*, so one setting had two representations. Every
remaining number flag is 1:1 with its row, and `NumberFlag` is now two fields.

Kept, and the reasons are three:

- **Everything a script passes.** `--frames --locked --realtime --headless --audio-null
  --msaa --width --height --debug-view --config --trace --no-rt --no-ssr --no-ssao
  --no-bloom --input-script --scene --bake-scene`, the capture block, `--golden --diff
  --compare-*`, the readback and sprite blocks, `--virtual-resolution`,
  `--ui-outside-virtual`, `--resize-every` and the RenderDoc pair. `scripts/golden.sh`,
  `scripts/readback.sh`, `scripts/baseline.py`, `scripts/locomotion.sh`,
  `build_release.sh` and `run.sh` were grepped for every candidate *before* a line was
  deleted, and the answer was the one the rule predicts: **a script passes only developer
  controls.** Nothing they drive was a candidate, so no caller had to change.
- **`--audio-null` specifically**, which `--set audio.backend=null` would technically
  subsume. It is short, it is in three verification scripts as of today, and replacing a
  word with an assignment in all three buys nothing.
- **The `--no-*` family and the switches an F-key already spells.** `--no-ssao` beside F8
  is one idea spelled twice for one reason, and the family is the vocabulary a trace is
  attributed with. `--no-particle-sort`, `--no-rt-shadows`, `--no-lod` and `--no-occlusion`
  have no key but are the same idea: an arm of a measurement.
- **`--record` and `--record-file` together.** `--record` takes an *optional* number and
  sets two rows, so it cannot be an assignment; splitting a two-flag feature across two
  spellings would read worse than keeping both.

`--tonemap`, `--log-level` and `--validation` also stayed. Each assigns a row with a JSON
key, so the rule says they could go — but all three are name-valued, all three are what a
bug report asks somebody to type, and D14 re-homes those rows out of the table anyway, at
which point they stop being exceptions and become the rule.

### `--help` states the rule, and stopped being a fourth list of names

The preamble is now the test a new flag has to pass, and the four inline enum lists D12
left behind are printed from `legalNames()` — the same tables that parse them. `--help` was
the last place a spelling could exist that the parser would refuse. It is 105 lines with a
fifteen-line preamble it did not have, so the flag list itself lost about thirty.

### The defect the grep turned up

**`--no-occlusion` has been two flags wearing one word, and only one of them fires.**
`kBoolFlags` claims it for `render.occlusionCulling` and `continue`s, so the branch below
that sets `audio.occlusionOff` has been unreachable since the flag tables landed —
`--help` documented both meanings and the audio one has never been available. Left alone
deliberately: whichever of the two gets renamed is a published flag changing meaning, which
is a card of its own and not this one. `Config.cpp` says so where the dead branch is, and
`--help` now documents only the one that fires.

### Verification

- `scripts/golden.sh check release` — **11 of 11, byte-identical.** The claim a D row lives
  or dies on, and the one the card called the test that matters: every case is driven by
  flags, so a retired flag a case still passed would show as an unknown-option warning and
  a changed image rather than as a silent no-op.
- `scripts/readback.sh release` — **9 of 9 bit-identical**, plus the lit silhouette exact
  and the resize soak clean across 12 swapchains.
- `scripts/locomotion.sh release` — **3 of 3 arms**, and it is a real caller: it passes
  `--headless --locked --audio-null --frames --input-script`, all kept.
- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan` — **829 tests,
  all four green**, each its own invocation. Nine `ConfigTests` cases are new or rewritten:
  `--set` against all six row types plus a name-valued row; provenance `cli` / `--set` for
  both a plain and a named row; an unknown key and an `engine.` key refused with the rest
  of the line still applying; no `=` assigning nothing and not swallowing the next flag;
  the first `=` only; the same key twice, last wins; a name refused *identically* through
  both doors, compared against each other rather than against a literal; and a loop over
  all twelve retired flags asserting none of them moves any row of the table.
- `./run.sh demo debug -- --headless --locked --frames 90 --audio-null --set
  render.validation=on --panel` — **zero validation errors**, layers confirmed on. That
  run also proves `--set` reaches an `initOnly` row before `freezeInitOnly`.
- `--dump-settings` after `--set render.bloomStrength=0.2 --set logging.level=debug` — both
  rows report `cli` with origin `--set`, beside a `config` row that did not move.

### What D14 through D17 inherit

- **A row is reachable from the command line the moment it exists.** D17's
  `--set demo.difficulty=3` needs no engine change at all: a row a game declares is found
  by `settings::find` like any other, and the flag table it would otherwise have needed is
  the registry this tree has refused twice.
- **D14 deletes flags, it does not move them.** Every row it re-homes out of the table
  loses its key, and by the rule stated here that is precisely when a *named* flag becomes
  correct — so `--validation`, `--tonemap` and `--log-level` stop being exceptions on the
  way past rather than needing an argument.
- **`--help` is generated where it can be.** Four name lists come from their tables. A row
  type D16 makes dynamic has one fewer hand-maintained copy waiting for it.
