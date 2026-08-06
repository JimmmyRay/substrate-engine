# Settings arc (S1–S4) Implementation Plan

> **Executed and complete.** `docs/ROADMAP-SETTINGS.md` has since been retired into the
> reference, as every finished plan in this repository is: the rule it established is
> [principles.md §7](../../architecture/principles.md#7-a-setting-is-a-property-of-the-person-running-the-program)
> and the mechanism is [tooling.md](../../architecture/tooling.md#configuration). This file
> is kept as the plan it was, not updated to match the result.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this
> plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land every row of `docs/ROADMAP-SETTINGS.md` (since retired) — separate
configuration from authored content in `substrate.json`, give the settings table a key
namespace that survives a setting with no JSON key, make the table enumerable so the whole
map can be dumped with provenance, and migrate the tree onto it without a key ever parsing
and doing nothing.

**Architecture:** One X-macro list in `engine/core/Settings.h` generates an id enum, a name
string, a metadata row (type, default, range, label, live binder) and a `constexpr` typed
handle per setting. `Settings` owns the value and its provenance; a row may additionally
carry a **binder** — a lambda that hands back the address of the live field the value
belongs in — so `Settings::bindLive(Engine&)` publishes the whole table into the renderer in
one generated loop, which is what deletes the 34 hand-written `renderer.x = config.render.x`
assignments rather than moving them. `Config` keeps only what is not a scalar setting
(bindings, buses, sources, point lights, decals, the camera-start block) and the derived
accessors.

**Tech Stack:** C++20, rapidjson, googletest. No new dependency.

## Global Constraints

- Never invoke `cmake` directly and never launch a binary directly — `scripts/build.sh`,
  `scripts/build_game.sh`, `scripts/run.sh`, `scripts/test.sh` only.
- Each configuration is built and run as its **own** invocation, never chained in one shell
  command.
- Any GUI launch is wrapped in `timeout -s TERM N`.
- `engine/` must build with nothing under `game/` in the tree. `Settings.cpp` joins
  `SUBSTRATE_HOSTED_SOURCES` (it pulls in neither Vulkan nor a window), so `scripts/test.sh tsan`
  covers it.
- **The golden image set must be byte-identical.** This arc adds no capability; a moved
  pixel is a defect and a re-snap is not an available answer.
- No abstraction layers over Vulkan. `Settings` is a table, not a registry of objects: no
  base class, no `virtual`, no observer list. Refresh stays polling, per G2.
- Mirrored keys keep the JSON spelling (camelCase); `engine.` keys are snake_case (S2).
- The engine defines exactly two base classes. This arc adds none.

---

## File Structure

| File | Responsibility |
|---|---|
| `engine/core/Settings.h` (new) | The X-macro list, `Id`, `Type`, `Value`, `Setting<T>` handles, the `options::` namespace, the `Settings` class declaration |
| `engine/core/Settings.cpp` (new) | The metadata table, `applyById`, string parse/format, JSON load and save, the dump, the live-bind loop |
| `engine/core/Config.h` | Loses every scalar that became a row; keeps the aggregates and the derived accessors |
| `engine/core/Config.cpp` | Load and CLI shrink onto `Settings`; the three folded cleanups land here |
| `engine/Engine.cpp` | The 34 assignments become one `bindLive` call; `--dump-settings` early exit |
| `engine/Engine.h` | `Settings& settings()` accessor |
| `game/demo/DemoGame.h/.cpp` | Owns the scene path, the sun, the exposure, the capture path — what S1 moved out of the config |
| `tests/SettingsTests.cpp` (new) | The table's own contract: round-trip, provenance, unknown key, removed key, dump |
| `tests/ConfigTests.cpp` | Follows `Config` down to what is left of it |
| `substrate.json` | Loses the authored sections |
| `docs/ROADMAP-SETTINGS.md` | Gains a stage table with status, and a verification section (in the event: retired into the reference instead) |
| `docs/architecture/limitations.md`, `tooling.md`, `docs/README.md` | The reference updates a landed stage owes |

---

## Task 1 — The table, alongside `Config` (S4 step 1, S2)

**Files:**
- Create: `engine/core/Settings.h`, `engine/core/Settings.cpp`, `tests/SettingsTests.cpp`
- Modify: `CMakeLists.txt` (add to `SUBSTRATE_HOSTED_SOURCES` and the test target)

**Interfaces produced:**

```cpp
namespace settings {

enum class Id : uint16_t { /* one per row */ Count };

enum class Type : uint8_t { Bool, Int, Uint, Uint64, Float, String };

/// Where the live value came from, in increasing precedence (S3).
enum class Source : uint8_t { Default, Config, Game, Cli };

template <typename T> struct Setting { Id id; };

struct Row {
    const char* key;        ///< "render.msaaSamples" -- the JSON path, mirrored exactly
    Type type;
    const char* label;
    double minimum, maximum; ///< meaningless for Bool/String; 0,0 means unbounded
    bool engineOwned;        ///< an `engine.` key: no JSON, read-only through the string door
    bool initOnly;           ///< refused after init, with a reason
};

const Row& row(Id);
Id find(std::string_view key);          ///< Id::Count when unknown
constexpr uint16_t count();

class Settings {
  public:
    template <typename T> T get(Setting<T> s) const;
    template <typename T> bool set(Setting<T> s, T v, Source from = Source::Game);
    bool setFromString(std::string_view key, std::string_view value, Source from);
    [[nodiscard]] std::string valueString(Id) const;
    [[nodiscard]] Source source(Id) const;
    [[nodiscard]] std::string origin(Id) const;   ///< the file or the flag that set it
    void dumpTable(std::FILE* out) const;
    void dumpJson(std::FILE* out) const;
};

} // namespace settings

namespace options::render { inline constexpr settings::Setting<float> exposure{settings::Id::render_exposure}; }
```

- [ ] **Step 1: Write the failing test** — `tests/SettingsTests.cpp`

```cpp
TEST(SettingsTest, EveryRowHasAUniqueKeyAndAFiniteType) {
    for (uint16_t i = 0; i < settings::count(); ++i) {
        const auto& r = settings::row(static_cast<settings::Id>(i));
        EXPECT_NE(r.key, nullptr);
        EXPECT_EQ(settings::find(r.key), static_cast<settings::Id>(i)) << r.key;
    }
}

TEST(SettingsTest, ADefaultReadsAsDefaultAndAStringSetChangesTheSource) {
    settings::Settings s;
    EXPECT_EQ(s.source(settings::Id::render_msaaSamples), settings::Source::Default);
    EXPECT_TRUE(s.setFromString("render.msaaSamples", "8", settings::Source::Config));
    EXPECT_EQ(s.get(options::render::msaaSamples), 8u);
    EXPECT_EQ(s.source(settings::Id::render_msaaSamples), settings::Source::Config);
}

TEST(SettingsTest, AnUnknownKeyIsRefusedRatherThanCreated) {
    settings::Settings s;
    EXPECT_FALSE(s.setFromString("render.nonsense", "1", settings::Source::Config));
}

TEST(SettingsTest, AnEngineKeyHasNoJsonSpellingAndIsSnakeCase) {
    const auto& r = settings::row(settings::Id::engine_current_scene_path);
    EXPECT_TRUE(r.engineOwned);
    EXPECT_STREQ(r.key, "engine.current_scene_path");
}
```

- [ ] **Step 2:** `scripts/test.sh debug -- --gtest_filter=SettingsTest.*` — expect a compile failure.
- [ ] **Step 3:** Write `Settings.h` / `Settings.cpp`. The list covers exactly S1's *"stays
      configuration"* set plus the `render.*` quality toggles that never appeared in the
      shipped `substrate.json` (they are properties of the machine, not of the game), plus
      one `engine.` row.
- [ ] **Step 4:** `scripts/test.sh debug -- --gtest_filter=SettingsTest.*` — expect PASS.
- [ ] **Step 5:** `scripts/build.sh debug` and `scripts/test.sh debug` — the whole suite still passes.

## Task 2 — JSON in, through the table (S4 step 1)

**Files:** Modify `engine/core/Settings.cpp`, `engine/core/Config.cpp`, `tests/SettingsTests.cpp`

- [ ] **Step 1:** Test that `Settings::loadJson(doc, path)` reads `render.msaaSamples` out of
      a nested document, marks it `Source::Config`, and leaves an absent key at `Default`.
- [ ] **Step 2:** Test that a key which *was* a setting and is not one now warns rather than
      being ignored — the S4 obligation. `removedKeys()` is a static list of
      `{key, message}` and `loadJson` reports each one it finds.
- [ ] **Step 3:** Implement; `Config::loadFromFile` calls it for every scalar section and
      keeps its hand-written parse only for the aggregates.
- [ ] **Step 4:** `scripts/test.sh debug` — PASS.

## Task 3 — Export the map (S3)

**Files:** Modify `engine/core/Settings.cpp`, `engine/core/Config.cpp` (flag), `engine/Engine.cpp` (early exit), `tests/SettingsTests.cpp`

- [ ] **Step 1:** Test `dumpTable` emits one line per row, aligned, with the four columns the
      roadmap specifies — key, type, value, source — and the origin where there is one.
- [ ] **Step 2:** Test `dumpJson` emits an object keyed by setting name whose members carry
      `value`, `type`, `source` and `origin`, and that it parses back with rapidjson.
- [ ] **Step 3:** Implement. `--dump-settings` and `--dump-settings=json` are parsed in
      `applyCommandLine`, which records the request; `Engine::init` services it after the
      config and the command line have both been applied and exits, because a dump taken
      before the CLI would report the wrong provenance for the thing being debugged.
- [ ] **Step 4:** `scripts/test.sh debug` — PASS. Then `scripts/build_game.sh demo debug` and
      `timeout -s TERM 60 scripts/run.sh demo debug -- --dump-settings` — eyeball the four sources.

## Task 4 — Live binding, and the 34 assignments (S4 step 2)

**Files:** Modify `engine/core/Settings.h/.cpp`, `engine/Engine.cpp`, `engine/Engine.h`

Each row that has a live home gains a binder in the metadata table:

```cpp
// Settings.cpp
void* bindRender(Id id, gfx::Renderer& r) { switch (id) { case Id::render_ssao: return &r.ssaoEnabled; ... } }
```

- [ ] **Step 1:** `Settings::bindLive(gfx::Renderer&)` walks every row, resolves its binder,
      and writes the current value through it — so config, CLI and default all arrive by one
      path. `set()` writes the bound location too, when one is bound.
- [ ] **Step 2:** Delete the 34 `render.x = configData.render.x` lines in `Engine.cpp` and
      call `bindLive` in their place. The three that are not plain fields —
      `setSampleCount`, `setDebugView`, `lightBudget` — stay explicit; `lightBudget` becomes
      an `initOnly` row that `set()` refuses *with a reason* after init.
- [ ] **Step 3:** `scripts/build_game.sh demo release` then `scripts/golden.sh` — **byte-identical,
      all cases.**

## Task 5 — Move the authored settings into game code (S4 step 3)

**Files:** Modify `engine/core/Config.h/.cpp`, `game/demo/DemoGame.h/.cpp`, `substrate.json`,
`scripts/manifest.py` interaction check, `tests/ConfigTests.cpp`

One group at a time, each removing the JSON key in the same change so no key ever parses and
does nothing:

- [ ] **Step 1:** `scene.path` / `scene.characters` → `DemoGame`. `engine.current_scene_path`
      becomes the readable record of what was loaded. `--scene` and the bare-path argument
      stay, because a scene named on the command line is how every golden case runs.
- [ ] **Step 2:** `lighting.*` → `DemoGame::placeLights` and its sun.
- [ ] **Step 3:** `window.title`, `render.exposure`, `benchmark.capturePath`,
      `benchmark.rdocCapturePath` → game code.
- [ ] **Step 4:** `audio.buses`, `audio.sources`, `audio.occlusion*` → game code.
- [ ] **Step 5:** `physics.gravity`, `physics.step` → game code.
- [ ] **Step 6:** `physics.clock` and `render.debugView` leave `substrate.json` and keep their
      CLI flags — `--locked`, `--realtime`, `--debug-view`. **Do not remove the flags.**
- [ ] **Step 7:** After each group: `scripts/build_game.sh demo release` + `scripts/golden.sh`.

## Task 6 — Delete what has no key, and the three cleanups (S4 step 4)

**Files:** Modify `engine/core/Config.h/.cpp`, `tests/ConfigTests.cpp`

- [ ] **Step 1:** Delete the `Config` members that no longer have a key.
- [ ] **Step 2:** The tri-state `on|true` / `off|false` parse appears three times
      (`rayQueryAllowed`, `validationEnabled`, `shaderHotReloadEnabled`) — extract one
      file-local helper, the narrowest scope that reaches all three.
- [ ] **Step 3:** The four string-to-index ladders (`logLevel`, `logOutput`,
      `debugViewIndex`, `tonemapIndex`) become tables in the form `logCategoryMask` already
      uses. `debugViewIndex` names `gfx::DebugView` instead of returning magic integers.
- [ ] **Step 4:** The ~20 bool-assigning command-line branches become one table; the six
      numeric flags that re-state their default fall back to the current value like the
      other nine.
- [ ] **Step 5:** `scripts/test.sh debug` and `scripts/test.sh tsan`, separate invocations.

## Task 7 — The documentation a landed stage owes

**Files:** Modify `docs/ROADMAP-SETTINGS.md`, `docs/architecture/limitations.md`,
`docs/architecture/tooling.md`, `docs/README.md`, `README.md` if the config section names a
moved key

- [ ] **Step 1:** `ROADMAP-SETTINGS.md` gains a stage table with a Status column and a
      verification section, matching the other two roadmaps' shape, and each row is marked
      done with what it actually landed.
- [ ] **Step 2:** The open question is answered in the document: `engine.` keys are readable
      and dumpable, writable only through the API that owns the side effect.
- [ ] **Step 3:** `tooling.md`'s configuration section documents `--dump-settings` and the
      four provenance sources.
- [ ] **Step 4:** `limitations.md`'s property-registry refusal links to the settings table
      and states why the two do not conflict.

## Verification, in full, at the end

Each its own invocation, never chained:

- [ ] `scripts/build.sh debug` / `scripts/test.sh debug`
- [ ] `scripts/test.sh asan`
- [ ] `scripts/test.sh tsan`
- [ ] `scripts/build_game.sh demo release` / `scripts/golden.sh` — byte-identical, all cases
- [ ] `timeout -s TERM 120 scripts/run.sh demo release -- --frames 300 --validation on` — zero
      validation errors
- [ ] `substrate bench` — `Lighting` and `Frame` medians unchanged
