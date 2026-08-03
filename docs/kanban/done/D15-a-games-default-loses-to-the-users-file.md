---
id: D15
title: A game's default loses to the user's file
arc: D
size: S
verification: tests-4
---

# D15 — A game's default loses to the user's file

Afterwards `settings.setDefault(handle, value)` writes only where nothing else has, so a game
shipping its own answer for vsync or MSAA states it in one line and the user's `substrate.json`
still wins. `set` keeps today's meaning — *my answer regardless* — for the cases that want it.

`Source` runs `Default < Config < Game < Cli`, and `set` defaults to `Source::Game`. So the
plain call a game author reaches for **overrides the config file**. `Game.h` documents the way
out and it is four lines of boilerplate around a comparison against `Source::Default`, which
means the common intention costs ceremony and the rare one is free. That is the wrong way
round: a game supplying a reasonable default is what almost every game does, and forcing a
value over the user's explicit choice is what a fixed-resolution game or a benchmark harness
does occasionally and deliberately.

Two doors named for what they do, rather than one with a flag, because they are different
intentions and a `bool respectUser` parameter at the call site reads as neither. `setDefault`
is *my answer unless you said otherwise*; `set` is *my answer regardless*. The dump names
`game` in the source column for both, so a user asking why their file did not take gets the
same answer either way.

Precedence itself does not move. Flipping `Game` below `Config` would take away a game's
ability to force a value at all, and it would change what the source column means for every
existing reader. The ordering is right; only the ergonomics were backwards.

Note the interaction with [D11](D11-the-engines-world-unit-constants-come-from-the-scene.md),
which inserts a `Scene` tier just above `Default`. `setDefault`'s test is *"has anything
claimed this row"*, so it must compare against `Source::Default` specifically rather than
"anything below `Game`" — otherwise a scene-derived value would be silently overwritten by a
game default, which is the opposite of what both cards want.

**What I expect to be wrong about.** That two doors are enough. A game may well want *my answer
unless the **user** said otherwise, but over anything the scene derived*, which is a third
predicate and the point at which this stops being two named methods and starts being a
parameter after all.

## Verification

- `scripts/kanban.py`, then `./build.sh`.
- `./test.sh debug`, `./test.sh asan`, `./test.sh tsan`, `./test.sh release` — each its own
  invocation. `SettingsTests`: `setDefault` writes over a `Default` row and is refused over a
  `Config` one; `set` still overrides `Config`; both record `Source::Game`; and `setDefault`
  against a row the command line already set changes nothing.
- No golden run needed: the demo does not call either today, so nothing an image depends on
  moves. If the demo gains a `setDefault` in the same branch, it does.

## Reference update

- [architecture/principles.md](../../architecture/principles.md) — §7's precedence list gains
  the distinction between a game's default and a game's override.
- [guides/making-a-game.md](../../guides/making-a-game.md) — the four-line idiom becomes one
  call, and the guide says which door to reach for.

## Outcome

**A reorder, and the card was wrong about which thing needed reordering.** It opens by
saying *"precedence itself does not move; only the ergonomics were backwards"*. The
ergonomics were backwards and `setDefault` fixes them, but the precedence was not what the
card — or `Settings.h`, or `Game.h`, or `tooling.md`, or the guide — said it was.

**The order before.** `Source` declares `Default < Config < Game < Cli` and five documents
repeat it, and nothing implemented it. `Settings::writable` takes the `Source` it is passed
and never looks at it, so a write is simply the last write; the order is therefore whatever
order `Engine::init` opens the doors in, and that order was the file, **then the command
line, then `game.configure`**. What the program actually ran was `Default < Config < Cli <
Game`. A game's `set` beat the user's command line, and the plain call a game author reaches
for beat the user's file as well — so *both* of the losses this card exists to establish
were absent, not just the one it named.

It went unnoticed because nothing exercises it. The demo's `configure` writes no setting and
says so in a comment, and the only `Source::Game` writes in `engine/` are the two
`pixelExact` makes from `initRenderer`, well after either call. Four documents could claim
the opposite of the code indefinitely.

**The order now.** `game.configure` is called between `loadFromFile` and
`applyCommandLine`, so `Default < Config < Game < Cli` is the sequence the doors open in
rather than a claim about it. Nothing was added to the setter, deliberately: a rule there
would also refuse a panel toggle or an `F8` keypress — both write as `Source::Game` — over a
row a startup flag claimed, and a runtime write beating a startup one is the whole reason
the table binds live fields. Precedence is a sequence; the one predicate a sequence cannot
express is a game's *default*, and that is `setDefault`, which writes only where the row
still reads `Source::Default`.

The move costs one thing, and it is stated in `Game.h` rather than discovered: a game
*reading* a setting from `configure` now sees the user's file and not the flags. That is the
same trade in the other direction, and §7 is what decides which direction to take it.

**The `initOnly` interaction is unchanged, and now checked.** `freezeInitOnly()` runs at the
end of `Engine::init`, hundreds of lines after `configure` at either position, so a game
default still reaches an `initOnly` row before what it sizes is sized — `render.lightBudget`
took a game default and read `48 game` in a live dump. A user's file still beats it there,
because `setDefault`'s test is provenance rather than timing. After the freeze the row is
refused for the other reason and logs the sentence it already had, which is the distinction
worth keeping: *"somebody else already said"* is silent and ordinary, *"too late"* is not.

**One exception, and it is written out rather than hidden.** The two `pixelExact` writes
cannot be made before the flags — both rows have to follow `bindRenderer` — so they compare
against `Source::Cli` by hand. Without that they overrode the command line, which
[rendering.md](../../architecture/rendering.md) had claimed they did not.

**Verification.** `scripts/golden.sh check release` 11 of 11, twice — once before the
demonstration below and once after it was reverted. `./test.sh` at debug, release, asan and
tsan: **833 tests, 88 suites, all passing** in each, four of them new. Three in
`SettingsTests` cover every adjacent pair on its own (`Default < Config`, `Config < Game`,
`Game < Cli`), all four sources on one row, `setDefault` against a row claimed by the file
and by a flag, and the three `initOnly` cases; one in `ConfigTests` runs the real sequence
through the real doors — `loadFromFile`, then the two game doors, then `applyCommandLine`
with `--set`.

**The demonstration.** `Engine.cpp` is not hosted, so the sequence itself was checked by
running it: a temporary `configure` in the demo, five writes, one config file, one `--set`,
reverted before the branch closed.

```
window.vsync          bool    false   game            # a game override, over "vsync": true in the file
render.bloomThreshold float   1.5     config  d15.json  # a game default lost to the file
render.fogDensity     float   0.5     game            # a game default, where nothing else spoke
render.msaaSamples    uint32  1       cli     --set   # over "msaaSamples": 8 and a game default of 2
render.lightBudget    uint32  48      game            # an initOnly row, reached before the freeze
render.ssao           bool    true    default
```

A second run added `--set window.vsync=true`, which took the row from the game's forced
value to `cli --set`. Under the previous ordering that flag lost.

**What D16 and D17 inherit.** Both a rule and a mechanism. D16 makes the table dynamic:
whatever replaces the fixed array has to keep `source(id)` answerable *before* a write
decides whether to happen, since that is now the whole of `setDefault`. D17 lets a game
declare its own rows, and a declared row arrives with the same four tiers — its default is
the declaration, its `config` tier is a section of the user's file, and `configure` is where
the declaration happens, which is already the slot between the file and the flags. Neither
inherits a precedence rule inside the setter to fight, because there is none.

**What I expect to be wrong about, revised.** The card guessed the third predicate would be
*"over the scene, under the user"*. The one that actually turned up first is *"under the
command line, over everything else"* — `pixelExact`, which needed it because it cannot be
sequenced. One occurrence is not a pattern; a second is the point to re-read this.
