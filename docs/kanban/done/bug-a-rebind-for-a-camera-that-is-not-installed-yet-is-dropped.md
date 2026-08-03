---
id: bug-a-rebind-for-a-camera-that-is-not-installed-yet-is-dropped
title: A rebind for a camera that is not installed yet is dropped
arc: bug
size: S-M
verification: tests-hosted, scripted-input, inspection
---

# bug-a-rebind-for-a-camera-that-is-not-installed-yet-is-dropped — A rebind for a camera that is not installed yet is dropped

A config rebind of a `FlyCamera` row is lost at startup, because the flycam is not installed when
`applyBindings` runs. The demo logs `Config binds unknown action "Camera.Forward"; ignored`, the
row never existed to be rebound, and the next `saveBindings` writes nothing for it.

**This is the same failure the retirement bug had and a different cause**, which is why it is a
separate card.
[bug-a-rebind-is-lost-when-the-action-is-retired-before-applybindings](../done/bug-a-rebind-is-lost-when-the-action-is-retired-before-applybindings.md)
fixed the case where a row **exists but is dead** — `findDeclared` reaches it. Here the row has
**never been declared at all**, so there is nothing for any lookup to find. G18's
declare-on-activate is what makes it reachable: a game holding three cameras and installing one
has two thirds of its camera bindings undeclared at startup, every time.

`Camera.Orbit` happens to be safe because both `FlyCamera` and `ThirdPersonCamera` declare it, so
one of them is always installed. That is luck, not design, and it is why this went unnoticed
through C37.

**The tension to resolve, and it is the work.** G18's whole argument for declare-on-activate is
that a game should pay input surface only for the active camera — two held cameras declaring
`Camera.Forward` in their constructors is a conflict `InputMap::conflicts()` would be right to
report. So the fix cannot be "declare everything up front". Candidates:

- **Park the config's unmatched rows and apply them on declaration.** `applyBindings` keeps what
  it could not resolve; `declare` checks that store and applies a pending rebind when the row
  first appears. Fixes it for any action declared after startup, not just cameras, and pays one
  small map that is empty in the common case.
- **A rebind store keyed by name rather than by `ActionId`**, which is arguably what the binding
  file already is — the question is whether `InputMap` should hold it rather than re-reading the
  config.
- **Declare-on-construction with a live flag**, i.e. move `activate` to the constructor and let
  retirement do the hiding. This is a reversal of G18's decision and should only be taken with
  its argument answered, not ignored.

Prefer the first unless something rules it out: it is the only one that also covers an action a
game declares lazily for a reason that has nothing to do with cameras.

**Provenance.** Found while executing
[C37](../done/C37-first-person-third-person-and-isometric-cameras.md), which reported it as
"inherent to G18's model, not to this card's code, but newly reachable". Not measured; the demo's
startup log is the whole of the evidence so far.

## Verification

- `tests-hosted`: `./test.sh debug` then `./test.sh asan`, each its own invocation. **A test that
  fails today**: apply a config binding a non-default key to an action that has not been declared
  yet, then declare it, and assert the binding is the config's rather than the default. Establish
  it red first.
- `scripted-input`: `scripts/locomotion.sh`, nine arms. It already asserts the demo's shipped
  binding line, which is the surface this changes.
- `inspection`: record whether `Config binds unknown action` still fires, and for what. After the
  fix it should mean an action no game will ever declare — if it still fires for a camera row
  that later appears, the fix did not land.

## Reference update

[systems.md](../../architecture/systems.md), the input section's consumer table and the camera
section's declare-on-activate paragraph, which is where the trade-off is currently argued.

## Outcome

**Parked, per the card's preferred candidate, and the other two were ruled out on their own
terms.** `applyBindings` now keeps the rows it could not resolve and hands the set to the map;
`declare` takes and **erases** a matching parked row when the action first appears, moving the
live list only and leaving `defaults` alone — so `isDefault` goes false and the next save writes
it.

Red first, verbatim, three cases before any fix:

```
BindingFileTest.AConfigRebindWaitsForAnActionNothingHasDeclaredYet
  map.bindingList(forward)  Which is: "W Pad.LeftY-"   vs "G"
  map.isDefault(forward)    Actual: true  Expected: false
BindingFileTest.AHeldRebindIsTakenOnceAndNotReplayedOverALaterEdit
  map.bindingList(forward)  Which is: "W"  vs "G"
BindingFileTest.ASecondConfigTableReplacesWhatTheFirstWasHolding
  ...  Which is: "W"  vs "T"   the last table read is the file
```

**Why not the other two.** A name-keyed rebind store is the same mechanism with a wider mandate:
every path touching a binding would have to consult it, and it duplicates state the action table
already owns for the ~95% of rows that resolve immediately. The parked set is that idea narrowed
to exactly the rows with nowhere else to live, and it empties itself as they find one.
Declare-on-construction with a live flag reverses G18 without answering it — two held cameras both
declaring `Camera.Forward` is a real `conflicts()` report, and hiding it behind a flag means
either `conflicts()` learns about liveness (a second notion of "not at the same time" beside the
resolver's) or it starts reporting collisions between schemes that can never run together. It
also fixes only cameras, where parking fixes any action declared after startup.

**All three interactions the card named, handled and tested.** *Retire/revive*: the row is
consumed by the `declare` that takes it, because a store that kept it would replay the config over
whatever the player rebound since — `AHeldRebindIsTakenOnceAndNotReplayedOverALaterEdit` parks
`G`, declares, rebinds to `T` in-session, retires, revives, and asserts `T`; the trap is written at
the erase. *Never-declared rows*: cannot accumulate, because `applyBindings` **replaces** the set
rather than appending, so it is bounded by one config file and empty in the common case — they are
not actions, so `actionCount()` does not grow and `conflicts()` cannot see them.
*`saveBindings`*: untouched and deliberately so, since it walks the action table and therefore
never writes a parked row.

**The warning moved to teardown**, and that is the one thing this needed outside `Input`:
"nothing has declared it yet" and "no game will ever declare it" are the same state at startup and
only distinguishable at exit. `Engine`'s startup line is now
`Input: N actions, N rebound, N held from <path>`.

`inspection`, against the running demo, with a config carrying `Camera.Forward: ["G"]` (a flycam
row, undeclared at startup) and `Camera.Nope: ["H"]` (bogus). Installing the flycam at frame 60:

```
[Input] [DEBUG]   Config binds "Camera.Forward", which nothing has declared yet; held
[Input] [DEBUG]   Config binds "Camera.Nope", which nothing has declared yet; held
[Input] [STATUS]  Input: 53 actions, 0 rebound, 2 held from .../inspect.json
[Core]  [STATUS]  Shipped camera (fly): ... Camera.Forward=G* Camera.Back=S Pad.LeftY+ ...
[Input] [WARNING] Config binds unknown action "Camera.Nope" (H); ignored
```

`Camera.Forward=G*` — the config's key with the `*` marking it off its default, so the next save
writes it — and the warning fires once, for the row no game will ever declare. In a bare run with
no toggle both names warn at exit, which is the honest answer for a run in which neither was
declared.

Verification: `./test.sh debug` and `./test.sh asan`, separate invocations, **1055 tests from 106
suites, all passed** in both. `scripts/locomotion.sh` **9 of 9 arms** before and after, the two
summaries diffing byte-identical including the `Shipped bindings` and `Shipped camera (follow)`
lines.

**One residual, stated rather than left to be found.** Parking fixes the *apply* side;
`saveBindings` still writes only rows that exist, which this card required. So a player who
rebinds the flycam, plays a whole session on the follow camera and rebinds something else before
quitting still loses the flycam edit. Closing that means writing parked rows back out — preserving
player data at the cost of a typo living in the config forever — and is a separate decision that
was not taken. Also noted: the startup `conflicts()` scan runs once, so a row declared late is
never checked for collisions. Pre-existing and not made worse here, except that a late-declared
row now arrives carrying the player's key rather than the shipped default, which is one more way a
collision could first appear after startup.

A wording correction: the card says the fix costs "one small map". It is a vector — the order is
the config's, lookup happens once per `declare` over a normally-empty container, and a map header
would cost more than the scan.

Executed alongside another session's uncommitted C35 and collider work; none of their files was
touched.

Reference updated: `systems.md`'s input section records the parking, the three properties that
make it safe, the move of the warning to teardown, and the residual it does not fix.
