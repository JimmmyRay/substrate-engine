---
id: G2
title: Settings / property table
arc: G
size: M
verification: golden-12, tests-hosted, scaffold
---

# G2 — Settings / property table

M

## `drawUi` — configuration at runtime

```cpp
void DemoGame::drawUi(Engine& e, ui::Context& ui) {
    if (!ui.beginPanel("Sample", {16, 16}, {320, 640})) return;

    // Every row comes from the property table: a checkbox per bool, a slider per ranged
    // float, a list per enum, with name, range and doc read from the same table that
    // parses substrate.json and that the inspector walks.
    drawSettings(ui, e.settings(), "render");

    ui.separator();
    ui.slider("Torch", e.scene().light(torch).intensity, 0.0f, 80.0f);

    // A second function beside drawInstanceInspector, not a table applied to a node --
    // see G6, and the reasoning Inspector.h already records.
    drawNodeInspector(ui, e.scene(), player);
    ui.endPanel();
}
```

---

## Settings — the property table, and the refusal it has to answer

**`ui/Inspector` already exists, and it refused a property registry on purpose.** Its
header is explicit: *"There is no property registry, no reflection macro, no `describe<T>()`
and no field table -- and that is not an economy... A property system is a schema plus a
type-erased setter plus a name-to-offset map, which is three abstractions to save writing
`ui.slider("X", p.x, ...)`."* `limitations.md` records the same refusal in its deliberate-
omissions table.

**That refusal is correct and this row does not overturn it.** It is about *inspecting
objects*, and it comes with a stated trigger — *"The third thing worth inspecting is when
to look again; two is a coincidence, and the second one will be a second function."* One
thing is inspected today. G6 makes it two. The trigger has not fired, and G6 is written
below as a second function accordingly.

G2 is a different problem with a different consumer set, and the distinction is worth
stating precisely because the two look alike:

| | Instance inspection | Settings |
|---|---|---|
| Consumers | One panel | JSON parse, JSON save, a panel, a typed setter, a string setter |
| What a table would save | `ui.slider("X", p.x, ...)` calls | 34 assignments **and** the drift between two copies of a value |
| Persistence | None — an instance is not serialised by name | The name *is* the JSON key; it already exists as a string |
| Failure mode without it | More code | A config key that silently does nothing |

**The honest version of the argument: if a generated settings panel were the only consumer,
the inspector's reasoning would apply and win.** It is not. The name of a setting already
exists as a string in `substrate.json` and is already parsed, already written back, and
already needs to survive a key the build does not know. A table is not being introduced to
name things — the things are already named, in two places, by hand, and the table makes
that one place.

**What it deletes.** There are **34** `renderer.x = config.render.x` assignments in
`main.cpp` today. If the table names the *renderer's* live field rather than a `Config`
member, all 34 stop existing, and with them the class of bug where the config says one
thing and the renderer holds another. The slider, the JSON key and the game's `set()` all
write one variable.

The mechanism is an X-macro, the precedent being the input action list, which already
generates an enum, a name table and a `static_assert` per row from one list.

## Two doors, one table

The same list generates the id enum, the name string, the metadata row **and** a `constexpr`
typed handle, so the two spellings of a setting cannot drift:

```cpp
namespace options::render {
    inline constexpr Setting<bool>     ssao{Id::render_ssao};
    inline constexpr Setting<float>    exposure{Id::render_exposure};
    inline constexpr Setting<uint32_t> msaaSamples{Id::render_msaaSamples};
}
```

- **Typed door**, for game and engine code: `set(options::render::exposure, 1.4f)`.
  `Setting<T>` is a `uint16_t` in a type wrapper, so a misspelling is a build error rather
  than a silent runtime no-op, and `set(options::render::exposure, true)` does not compile.
- **String door**, for the console, JSON parsing and the inspector, all of which are
  inherently dynamic: `setFromString("render.msaaSamples", "8", Source::Cli)`. (`exposure`
  was the example while this was written; S1 moved it into game code, which is itself the
  point -- the string door is for settings, and an exposure was never one.)

Both land in one `applyById(uint16_t, Value)`. **The constant path mirrors the JSON path
exactly** — `options::render::msaaSamples` against `render.msaaSamples` — because two
spellings for one setting is the thing the single table exists to prevent.

## Refresh: polling, not dispatch

There is **no `switch (module)`, no notification and no observer list**, because the
mechanism already exists and is better than one. `Renderer::drawFrame` compares, each frame:

```cpp
if (renderTargetsDirty || pipelinesDirty || featureKey() != builtFeatureKey) { ... }
```

That is the specialisation-constant design already in place — one live variant per pipeline,
compared per frame, which is *stricter* than a variant cache. Every feature behind a
specialisation constant already refreshes itself this way, and the rest are live fields read
per frame; `exposure` and `fogDensity` need no refresh at all.

Counting what `main.cpp` actually does: **34 are plain field writes, and 3 need a call** —
`setSampleCount`, `setDebugView`, and `lightBudget`, which must precede `init()`. A module
dispatch would be new machinery for the 8% case, a second refresh mechanism sitting beside
`featureKey()`, and a coupling from `Settings` to every subsystem — the seam the `Engine`
rule above exists to keep shut. It is also **wrong for the batch case**: loading a config
sets 34 values, and a per-`set` refresh would recreate render targets several times during
startup.

So the 8% gets an **optional side effect on the row**, defaulting to none:

```cpp
SETTING(render, exposure, float, 0.1f, 4.0f, "Exposure")           // polled; no effect
SETTING_FX(render, msaaSamples, uint32_t, 1, 8, "MSAA samples",
           [](Engine& e, uint32_t v) { e.renderer().setSampleCount(v); })
```

`set()` writes the field, then runs the row's effect if it has one. **Batch application
collects the distinct effects and runs each once at the end**, which is what makes config
load correct rather than thrashing.

`lightBudget` is the third case and is not a side effect at all: it is a row marked
**init-only**, which `set()` refuses after `init()` *with a reason*. That replaces a silent
clamp — the light buffer is sized from the budget at init, so a budget raised afterwards
would memcpy past a mapped range, and the current code clamps quietly rather than saying so.

## Verification

Everything below must pass before this may enter `done/`:

- `scripts/golden.sh` -- twelve cases, byte-identical. A generated panel that changes a pixel
  has changed a default, which is a defect rather than a rendering change.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- `--dump-settings` shows the same values and the same provenance column before and after.
- A scaffolded game builds and runs without touching anything under `engine/`.

## Reference

`architecture/tooling.md` for the table and the config mechanism, `architecture/principles.md`
for the rule deciding what is a setting and what is authored content.

## Outcome

**Four fifths of this row landed with the settings arc, and the last fifth is what this
close is about.** The table, both doors, the JSON load path and the deletion of the 34
assignments came in ahead of the board, because S's migration could not be done without
them. What was left was the generated panel and JSON *save* — and the two turned out to be
one piece of work, because a panel that forgets on exit is a demo of a setting rather than
a setting.

**The panel is `ui::drawSettings(ui, settings, "render")`** — `engine/ui/SettingsUi.h`, one
widget per row of a module, name and range and label from the table. It replaced nine
checkboxes and four sliders written out by hand in `game/demo/`, which is not a saving of
lines so much as the removal of a fifth spelling of every key. Hosted, so the unit suite
reaches it, and the tests drive it by Tab and Enter rather than by clicking coordinates: a
row's pixel position depends on how many rows precede it, and adding a setting has to stay
free.

**What the row did not predict: the panel is what forced the last three hand-applied rows
to stop being hand-applied.** `SettingsBind.cpp` said *"three rows have no plain field to
point at... three is not a pattern worth a mechanism; a fourth would be"*, and a generic
writer over the table is that fourth caller — it cannot know to call `setSampleCount`. The
resolution was not the `SETTING_FX` column this card designed. It was to poll the two of
them at the end of `Engine::endFrame`, which is the mechanism `featureKey()` already is, so
no effect column, no `Engine&` in `core/`, and nothing to subscribe to. `lightBudget`, the
third, was already bound and `initOnly`. **The card's own design was the more expensive
answer and the file's comment was the better guide.**

**A live defect fell out of the same question.** Asking "which rows would a generated
control lie about" found four that nothing re-reads after startup — `render.validation`,
`syncValidation`, `debugFont`, `debugFontHeight`. Setting any of them from the console did
nothing and *said* nothing. They are `initOnly` now, so the write is refused with a
sentence, and the panel draws them as readouts. That widened `initOnly`'s meaning from
"sizes something at init" to "is applied once", and its warning was reworded to match.

**`Settings::saveJson` writes what differs from the default plus every key the file already
had**, which is `saveBindings`' rule for an action arrived at independently for the same
reason. A file that fails to parse is refused rather than replaced. It is the **fourth**
caller of the write-temp-and-rename pattern, so that came out to
`core::writeFileAtomically` (`engine/core/FileWrite.h`) and the other three now call it —
the Rule of Threes had fired one caller earlier and nobody had noticed.

### Verification

- `scripts/golden.sh check release` — **all 12 cases match**, byte-identical.
- `./test.sh debug` and `./test.sh asan` — 626 tests, both green. Nine are new: five over
  the panel, four over the save.
- `--dump-settings` — 96 rows, values and provenance unchanged. The only edits to the
  X-macro list are in the `flags` column, which the dump does not print.
- `./run.sh demo release -- --headless --locked --frames 90 --panel --validation on` — a
  clean run with the generated panel open and the validation layers on.

The scaffold half of the stated verification could not be run: `./new_game.sh` is G1b and
is still in `backlog/`. What was checked instead is the property it stands for — nothing
under `game/` was needed by anything under `engine/`, and the demo builds and runs against
the new API without an engine change of its own.
