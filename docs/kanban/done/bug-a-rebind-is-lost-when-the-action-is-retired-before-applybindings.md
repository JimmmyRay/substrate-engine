---
id: bug-a-rebind-is-lost-when-the-action-is-retired-before-applybindings
title: A rebind is lost when the action is retired before applyBindings
arc: bug
size: S
verification: tests-hosted, inspection
---

# bug-a-rebind-is-lost-when-the-action-is-retired-before-applybindings — A rebind is lost when the action is retired before applyBindings

C36 made `saveBindings` write a retired row that differs from its default, so a player who
rebinds the fly camera, switches to a third-person scheme and quits keeps the edit. **That holds
for a scheme switched during play and not for one switched before `applyBindings` runs**, and the
second case loses the edit silently.

`core::input::applyBindings` — the config-to-map free function — resolves rows through `find`,
and `find` refuses a retired action. So a game that retires an action inside `Game::init`:

1. has the config's rebind for that action dropped at startup, with a `Config binds unknown
   action` warning that reads as a stale config file rather than as data loss;
2. leaves the row sitting at its default;
3. writes nothing for it on the next `saveBindings`, because the row now *is* its default.

The player's edit is gone, and the only trace is a warning that names the right action for the
wrong reason.

**This is a decision about the lifetime, not a patch.** Closing it means letting `applyBindings`
reach dead rows by name — which gives a different answer to "is a retired action findable" than
every other consumer gives, and C36's consumer table was written deliberately with `find`
refusing them. The candidate shapes, and picking between them is the work:

- a second lookup that ignores `live`, used only by `applyBindings` — smallest, and makes the
  table's "not found" true of *actions* rather than of *rows*;
- ordering: `applyBindings` before any `Game::init` retirement, enforced rather than documented;
- keeping the config's unmatched rows and re-applying them on revival, which fixes it for rows
  retired at any time but adds state.

**Prefer the one that does not need a rule nobody can check.** The current workaround —
`retire` after `applyBindings`, not before — is exactly that kind of rule.

**Provenance.** Found while executing
[C36](../done/C36-an-action-can-be-retired-and-the-pointer-has-a-verb.md), which implemented the
consumer table as specified; the hole is in the specification, not in the implementation. Stated
in [limitations.md](../../architecture/limitations.md), "A retired action's config binding is
dropped at startup, not at retirement".

## Verification

- `tests-hosted`: `./test.sh debug` then `./test.sh asan`, each its own invocation. **A test that
  fails today**: write a config binding a non-default key to an action, retire that action, run
  `applyBindings`, revive it by re-declaring, and assert the binding is the config's rather than
  the default. It must be red before the fix.
- `inspection`: record what the warning said and whether it still fires, since a warning that
  names the action for the wrong reason is half of why this went unnoticed.

## Reference update

[systems.md](../../architecture/systems.md), the consumer table in the input section — the `find`
row is the one that changes — and the [limitations.md](../../architecture/limitations.md) entry,
which is retired rather than edited if this is closed.

## Outcome

**Fixed with the second lookup, and the ordering option was not a trade-off — it is ruled out by
the call order that already exists.** `InputMap::findDeclared(name)` is `find` without the
liveness test, used by `core::input::applyBindings` and nowhere else.

Red first, verbatim, against the unmodified `Input.cpp`:

```
tests/InputTests.cpp:831: Failure
  applied    Which is: 0
  1u         Which is: 1
a retired action is declared, not unknown
tests/InputTests.cpp:834: Failure
  map.bindingList(fly)   Which is: "F"
  "G"
the player's edit, not the default
tests/InputTests.cpp:835: Failure
Value of: map.isDefault(fly)  Actual: true  Expected: false
```

**Why ordering could not be the answer.** `Engine::run` calls `game.init(*this)` and then
`applyBindings()` on the next line, with the comment "After the game declared its actions, and
never before: a config can only rebind an action that exists" — and `Engine::initInput` says the
same from the other end. So `applyBindings` runs *after* `Game::init` deliberately, and moving it
earlier would drop the rebind for **every** game-declared action rather than for the retired
ones. The narrower form — before any `Game::init` retirement — would mean splitting one opaque
call into a declare phase and a retire phase, which nothing outside the game can observe or
check: exactly the rule nobody can check this card said to avoid. The third option, keeping
unmatched config rows for later revival, buys coverage only for actions first declared after
startup and pays persistent state for it.

**What it does to C36's consumer table: it gains a row rather than changing one.** The
`find(name) → not found` row stays true as written — nothing that *reacts to input* can see a
dead row, and the new test asserts `find("Jump") == kInvalidAction` and
`findDeclared("Jump") == jump` side by side so a later edit cannot quietly merge them. The table
now also carries `findDeclared(name) → found`, and the distinction it draws is the useful one:
"not found" is true of rows a **frame** can see, while the one caller that *stores* a binding
rather than acting on one addresses **actions**. `Engine::applyBindings`' debug listing still
filters on `actionLive` and is untouched.

A tidy-up fell out of it: `find` is now `findDeclared` plus the liveness test and `declare` uses
it instead of its own hand-rolled name walk, so **three by-name loops collapse to one**.

`inspection`, as the card asked. The warning is unchanged in wording and now fires only for a
name no row claims — the retired row is applied silently and correctly, so "Config binds unknown
action" finally means unknown. The permanent test puts a retired row and a genuinely undeclared
row in the **same** `applyBindings` call, which is what stops the two answers collapsing back
into one; before the fix that call produced two identical warnings for two different failures,
which is half of why this went unnoticed.

Verification: `./test.sh debug` and `./test.sh asan`, separate invocations, **1029 tests from 105
suites, all passed** in both. `./build.sh debug` links clean end to end.

**Contradiction with this card's own text**, worth recording since the card is the record: it
presented ordering as a live candidate to be "enforced rather than documented". It is not a
candidate at all — the existing call order documents the opposite intent in two places and
reversing it breaks more than it fixes.

**One adjacent case deliberately left alone.** `Script::unknownActions` and `Script::apply` still
resolve through `find`, so an input script naming a retired action logs "no such action" and
presses nothing. That reads as correct — a script drives a device through an action's binding,
and a retired action resolves as unheld either way — but it is the same message on a row that
does exist, so it is a card if it ever matters.

Executed alongside another session's uncommitted C35 work in this checkout; nothing of theirs was
touched.

Reference updated: `systems.md`'s consumer table gains the `findDeclared` row and sharpens the
`find` row, and `limitations.md`'s "A retired action's config binding is dropped at startup" is
**retired rather than edited**, since it is no longer true.
