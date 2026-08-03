---
id: C36
title: An action can be retired, and the pointer has a verb
arc: C
size: S
verification: tests-hosted, golden
---

# C36 — An action can be retired, and the pointer has a verb

Afterwards `core::input::InputMap` can take an action back out of circulation without
invalidating anybody's `ActionId`, and hiding the cursor for the length of a drag is a call
rather than a property of the camera. Two changes to `engine/core/Input.{h,cpp}` and nothing
else: no camera, no renderer, no pixel.

Both exist because G18 needs them, and both stand up without it.

## Retiring an action

`ActionId` is a bare index into a `std::vector<Action>` — `declare` is `push_back` and then
`size() - 1`, and every accessor is `id < actions.size()` followed by `actions[id]`. Erasing a
row therefore shifts every id above it and silently repoints the `ActionId` members a game is
holding, which is the failure that has no symptom until a keypress does the wrong thing.

So the row is tombstoned rather than erased: a `bool live` on `Action`, and `retire(ActionId)`
flips it.

```cpp
void retire(ActionId id);                          ///< stops resolving and listing; id stays valid
[[nodiscard]] bool actionLive(ActionId id) const;
```

**`retire` keeps `bindings` and `defaults` and only flips `live`.** That is the whole reason the
tombstone is worth more than a `clearBindings` call: `declare` is already idempotent by name, so
re-declaring a dead row revives it with **the same id and whatever the player had rebound it
to**. A control scheme that comes and goes — which is what a camera swap is — does not cost the
player their bindings.

Every consumer has to be told, and the interesting part is that they do not all answer the same
way:

| Consumer | A dead row |
|---|---|
| `value` / `held` / `pressed` / `released` | reads as zero / false |
| `find(name)` | is not found — the action is not active |
| `declare(name, defaults)` | **revives it**, keeps its bindings, returns the same id |
| `conflicts()` | is skipped |
| `BindingMenu`'s listing | is skipped |
| `Engine::applyBindings` | is skipped |
| `saveBindings` | is **written** |
| `actionCount()` | still counts — it is the table size, and three callers use it as a loop bound |

`saveBindings` is the deliberate exception and the one a reasonable implementation gets wrong.
It writes rows that differ from their default. A player who rebinds the fly camera, switches to
a third-person scheme and then quits must not lose that edit because the row happened to be
inactive at the moment the file was written.

`actionCount()` staying the table size is the other trap. Three loops use it as a bound —
`saveBindings`, `Engine::applyBindings` and `BindingMenu` — so making it return a live count
would make those loops skip real rows rather than dead ones.

## A verb for the pointer

Hiding the cursor for the length of a look-drag is wired to `Camera::orbitAction()` today, so
the window has to ask the camera which action is a drag. That is a hook for one behaviour, and
the next behaviour wants another one.

```cpp
namespace core::input {
void mouseGrab();                     ///< hide the cursor, report unbounded deltas
void mouseRelease();                  ///< give it back where it was taken
[[nodiscard]] bool mouseGrabbed();
}
```

**There is one cursor, so this is process state rather than one map's.** A static in `Input.cpp`
behind three free functions, which is the shape `core::Logger` and `core::Profiler` already use.
Free functions rather than statics on `InputMap`, because a static member would say the state
belongs to a map, and a second map would be talking about the same physical pointer.

A declarative alternative — an action flagged "grabs the pointer while held" — was considered and
does not work: it can only express *while this action is held*, and a first-person camera wants
the pointer for as long as it is the camera, with no button down at all. Grabs also want to
happen for reasons that are not actions, a cutscene and a modal being the obvious two.

`Input.cpp` is hosted and must pull in neither Vulkan nor a window, so these record a **desire**
and `Engine`'s frame applies the GLFW mode change. Effective grab stays
`desired && !uiOpen && hasFocus`, which is what already keeps a panel opening mid-drag from
leaving the pointer captured — the desire survives and nothing has to re-assert it. No
refcounting: one boolean, last writer wins. The static wants a reset entry point, because the
unit suite links the hosted sources and would otherwise carry a grab between tests.

## Verification

- `./test.sh debug`, then `./test.sh asan`, each its own invocation. All hosted:
  - A retired action reads as not-held and is not `find`-able, and its `ActionId` stays valid.
  - Re-declaring a retired action by name returns **the same id**, and a binding the test moved
    before retiring is still there afterwards.
  - `saveBindings` writes a retired row whose bindings differ from its default; `conflicts()`
    does not see it.
  - An action declared after a retirement gets a fresh id, and the retired row's id still
    resolves to its own name.
  - `mouseGrab` / `mouseRelease` round-trip, and the reset leaves no grab between tests.
- `scripts/golden.sh` — thirteen cases, byte-identical. Nothing here touches a pixel, and that
  is the claim being checked rather than an expectation being confirmed.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the input section, for the action
lifetime and who owns the pointer.
[architecture/limitations.md](../../architecture/limitations.md) — the entry on an input edge
having as many latches as consumers is unaffected, but the "actions are declared once and live
forever" assumption is not written down anywhere and now needs to be, as its opposite.

## Outcome

**Both halves landed as specified, and the specification turned out to have a hole in it.**

`Action` gained `bool live`; `retire(id)` flips it and **zeroes every player's `ActionState`**,
which is one thing the card did not say and the implementation needed: without it a read in the
same frame still sees the old state, and a later revival can fire an edge out of the state the
row was retired holding. `declare` now walks the table itself rather than calling `find`, so it
sees dead rows, flips `live` and returns the same id with `bindings` and `defaults` untouched.
`find` skips them; `conflicts()` skips them on both sides of the pair; `beginFrame` resizes a
dead row's per-player state — so a revive after `setPlayerCount` answers the right number of
players — and then `continue`s without resolving it.

Every row of the card's consumer table is implemented and covered:

| Consumer | Implementation | Test |
|---|---|---|
| `value`/`held`/`pressed`/`released` | `actionLive` guard, plus the zeroing above | `ARetiredActionStopsResolvingAndStopsBeingFound` |
| `find(name)` | `live &&` in the scan | same |
| `declare` | scans dead rows, flips `live` | `RedeclaringARetiredActionRevivesItWithTheSameIdAndTheSameBindings` |
| `conflicts()` | `live` guard both sides | `ARetiredActionIsNotCompetingForItsKey` |
| `BindingMenu` listing | `actionLive` in `rebuild` | `BindingMenuTest.ARetiredActionLeavesTheListing` |
| `Engine::applyBindings` listing | `actionLive` in the loop | not unit-covered — a `Logger::debug` loop in a Vulkan TU |
| `saveBindings` | **no change**; none of `isDefault`/`bindings`/`actionName` consults `live` | `ARetiredRowIsStillWrittenIfThePlayerReboundIt`, and `ARetiredRowAtItsDefaultIsNoMoreWrittenThanALiveOne` for the other half |
| `actionCount()` | unchanged, the table size | asserted in both retirement tests |

**The three `actionCount()` loops are exactly three**, and each was checked rather than assumed:
`saveBindings` (unchanged, filters on `isDefault` only), `Engine::applyBindings`' debug listing
(now skips dead rows), and `BindingMenu::rebuild` (skips them before the filter, so they leave
the "n of m" count too). The repo-wide grep otherwise finds a log line that is a count and not a
bound, two test assertions, and an unrelated symbol in `engine/ai/Planner.h`.

The pointer verb is a file-local boolean in an anonymous namespace behind `mouseGrab`,
`mouseRelease`, `mouseGrabbed` and `mouseGrabReset`. `Engine`'s cursor path now reads
`mouseGrabbed() && !uiOpen && glfwGetWindowAttrib(window, GLFW_FOCUSED)` — the card's
`desired && !uiOpen && hasFocus` written out, where the focus term had previously been implicit
via `loseFocus()` dropping the orbit hold.

**One judgment call, flagged rather than buried.** The card says the changes are to
`Input.{h,cpp}` and "no camera", while also saying to unwire `Camera::orbitAction()` — but
something has to assert the desire or the look-drag silently stops hiding the cursor. The
assertion went into the Engine's cursor path as one `if`/`else` over
`inputMap.held(cameraState.orbitAction())`, and the *cursor path itself* now depends only on the
verb. `Camera` is untouched. Moving the assertion into `Camera::update` is where it eventually
belongs and would have edited the camera, which the card forbids.

**The contradiction, and it is a real defect rather than a wording problem.** The table's `find`
row and its `saveBindings` row together leave a hole: `core::input::applyBindings` resolves rows
through `find`, which now refuses retired actions — so a game that retires an action inside
`Game::init` has the config's rebind dropped at startup with a `Config binds unknown action`
warning, the row falls back to its default, and the next `saveBindings` writes nothing for it.
The player's edit survives a scheme switch made *during play*, which is the case the card argues,
and not one made *before* `applyBindings` runs. Implemented as specified and opened as
[bug-a-rebind-is-lost-when-the-action-is-retired-before-applybindings](../backlog/bug-a-rebind-is-lost-when-the-action-is-retired-before-applybindings.md);
closing it means letting `applyBindings` reach dead rows by name, which is a different answer to
"is a retired action findable" than every other consumer gives, so it is a decision about the
lifetime rather than a patch.

Verification: `./test.sh debug` and `./test.sh asan`, separate invocations, **1028 tests from 105
suites, all passed** in both. `scripts/golden.sh check release` — **13 of 13**, and checked as the
claim rather than the tolerance: `cmp` reports every `<case>.png` identical to this run's
`<case>.actual.png`, actuals timestamped from this run against baselines from the previous day.
Nothing here touches a pixel, and that is what was checked. No device-lost.

**Executed alongside another session's uncommitted C35 work in the same checkout**, and this card
was chosen for being disjoint from it: `engine/gfx/Renderer.{h,cpp}`, `Pipeline.h`,
`SettingsBind.cpp`, `core/Settings.h`, the `light_cluster*` shaders and `game/battle_arena/` were
left exactly as found, and their in-flight state built and rendered thirteen matching goldens.

Reference updated: `systems.md`'s input section gains the retirement rule, the consumer table and
why the pointer is process state; `limitations.md` gains the startup hole as a stated limit with
the workaround (retire *after* `applyBindings`).
