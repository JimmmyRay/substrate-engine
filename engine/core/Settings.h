#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace gfx {
class Renderer;
}

namespace core {

/**
 * @file engine/core/Settings.h
 * @brief The settings table: one list, four consumers, and a value that knows where it
 *        came from.
 *
 * A row's key with a module prefix *is* its JSON path in `substrate.json`, so renaming one
 * half of the key breaks every existing config file. An `engine.` row has no JSON key at
 * all: it is engine-owned live state, and the string door refuses it.
 *
 * Refresh is polling, not dispatch -- there is no observer list. `bindLive` points a row at
 * the field it belongs in, so a second copy of a value anywhere is a copy free to drift.
 */
namespace settings {

/// The C++ type behind a row, and what the string door parses into.
enum class Type : uint8_t { Bool, Int, Uint, Uint64, Float, String };

/**
 * @brief Where the live value came from, in increasing precedence.
 *
 * The precedence is the order `Engine::init` opens the doors in -- file, `Game::configure`,
 * flags -- and is deliberately not a rule inside the setter. A rule would also refuse a
 * panel toggle or a keypress, both `Source::Game`, over a flag set at startup.
 *
 * The one case ordering cannot express is a game's *default* losing to a file read that
 * came before it; that is `Settings::setDefault`.
 */
enum class Source : uint8_t { Default, Config, Game, Cli };

/// Row flags. `kEngine` is live state with no JSON key, refused by the string setter.
/// `kInitOnly` marks a row whose value sizes something at init, so a later write is refused
/// with a reason rather than clamped in silence.
enum Flags : uint8_t { kNone = 0, kEngine = 1u << 0, kInitOnly = 1u << 1 };

// clang-format off
/**
 * @brief The list. Everything else in this file and its `.cpp` is generated from it.
 *
 * `X(module, name, TypeTag, ctype, default, minimum, maximum, flags, label)`
 *
 * `minimum` and `maximum` bound a slider and clamp a parse, are ignored for `Bool` and
 * `String`, and mean unbounded when equal.
 *
 * Before adding a row, apply principles.md rule 7 -- a setting is a property of the person
 * running the program. Authored decisions belong in `GameSetup` and developer controls on
 * `Config`. A key removed from here has to be added to `removedKeys()`, or a config file
 * still carrying it gets silence.
 */
#define SUBSTRATE_SETTINGS(X)                                                                                          \
    X(window, width,  Int, int, 1600, 320, 7680, kNone, "Window width")                                                \
    X(window, height, Int, int,  900, 240, 4320, kNone, "Window height")                                               \
    X(window, vsync,  Bool, bool, false, 0, 0, kNone, "VSync")                                                         \
    X(render, msaaSamples,        Uint,   uint32_t, 4u, 1, 8, kNone, "MSAA samples")                                   \
    /* `kInitOnly` because the atlas is baked when the renderer starts; a later write is   */                          \
    /* inert. One atlas serves the overlay, the panel, the inspector and every game string.*/                          \
    X(render, debugFont,          String, std::string, "", 0, 0, kInitOnly, "UI font (empty = embedded bitmap)")       \
    X(render, debugFontHeight,    Float,  float, 16.0f, 8.0f, 64.0f, kInitOnly, "UI font height")                      \
    X(render, ssao,               Bool,   bool, true, 0, 0, kNone, "SSAO")                                             \
    X(render, ssaoRadius,         Float,  float, 0.5f, 0.0f, 16.0f, kNone, "SSAO hemisphere radius")                   \
    X(render, ssaoBias,           Float,  float, 0.025f, 0.0f, 1.0f, kNone, "SSAO depth bias, in metres")              \
    X(render, shadows,            Bool,   bool, true, 0, 0, kNone, "Shadow maps (non-RT path)")                        \
    X(render, punctualShadows,    Bool,   bool, true, 0, 0, kNone, "Point and spot light shadows")                 \
    X(render, shadowCache,        Bool,   bool, true, 0, 0, kNone, "Reuse unchanged shadow atlas layers")          \
    X(render, shadowDistance,     Float,  float, 0.0f, 0.0f, 500.0f, kNone, "Shadow box cap (0 = scene bounds)")       \
    /* Post-exposure radiance, so it means the same in a game with a different            */                           \
    /* `GameSetup::look.exposure`. 0 drops no light at all. The 0.1 ceiling is a tenth of  */                          \
    /* display white; above it this stops being a cutoff and starts turning lights off.    */                          \
    X(render, lightCutoff,        Float,  float, 0.0f, 0.0f, 0.1f, kNone, "Light cutoff (post-exposure radiance)")      \
    X(render, bloom,              Bool,   bool, true, 0, 0, kNone, "Bloom")                                            \
    X(render, bloomThreshold,     Float,  float, 1.1f, 0.0f, 64.0f, kNone, "Bloom threshold")                          \
    X(render, bloomSoftKnee,      Float,  float, 0.6f, 0.0f, 8.0f, kNone, "Bloom soft knee")                           \
    X(render, bloomStrength,      Float,  float, 0.05f, 0.0f, 1.0f, kNone, "Bloom strength")                           \
    X(render, edgeMsaa,           Bool,   bool, true, 0, 0, kNone, "Edge-detect MSAA")                                 \
    X(render, culling,            Bool,   bool, true, 0, 0, kNone, "GPU frustum culling")                              \
    X(render, occlusionCulling,   Bool,   bool, true, 0, 0, kNone, "Two-pass Hi-Z occlusion culling")                 \
    X(render, meshLod,            Bool,   bool, true, 0, 0, kNone, "Screen-coverage mesh LOD")                         \
    /* Fraction of the viewport an instance must cover to stay at LOD 0; each level below */                           \
    /* it is a quarter of this. 0.000244 is 1/4096 -- a 20x20 pixel footprint at 1600x900.*/                           \
    X(render, lodThreshold,       Float,  float, 0.000244f, 0.0f, 1.0f, kNone, "LOD 1 coverage threshold")             \
    /* An optimisation that must stay exact: the same frame with this off has to match it */                           \
    /* on, to the byte. That is what the escape hatch is for.                             */                           \
    X(render, lightTiles,      Bool,   bool, true, 0, 0, kNone, "Tiled light assignment")                       \
    X(render, rt,                 Bool,   bool, true, 0, 0, kNone, "Ray tracing")                                      \
    X(render, rtShadows,          Bool,   bool, true, 0, 0, kNone, "Traced shadow rays")                               \
    /* An approximation, not an optimisation: it moves pixels at silhouettes, six golden */                             \
    /* cases' worth. Inert at 1x and wherever the two rows above are off. See            */                             \
    /* rendering.md, "The shadow mask".                                                  */                             \
    X(render, rtShadowMask,       Bool,   bool, false, 0, 0, kNone, "Per-fragment traced shadow mask")                 \
    X(render, ssr,                Bool,   bool, true, 0, 0, kNone, "Screen-space reflections")                         \
    X(render, ssrRoughnessCutoff, Float,  float, 0.4f, 0.0f, 1.0f, kNone, "SSR roughness cutoff")                      \
    X(render, ssrIntensity,       Float,  float, 1.0f, 0.0f, 4.0f, kNone, "SSR intensity")                             \
    /* Fraction of the render extent the reflection pass runs at. Anything below 1.0       */                          \
    /* rebuilds `ssrTarget` and switches the composite to a joint-bilateral upsample; 1.0   */                         \
    /* is bit-for-bit the full-resolution path.                                             */                         \
    X(render, ssrScale,           Float,  float, 1.0f, 0.25f, 1.0f, kNone, "SSR resolution scale")                     \
    /* The 500 ceiling is shared with `shadowDistance` and `fogMaxDistance`; changing one   */                         \
    /* of the three alone puts them out of agreement.                                       */                         \
    X(render, ssrMaxDistance,     Float,  float, 8.0f, 0.0f, 500.0f, kNone, "SSR march distance")                      \
    X(render, ssrThickness,       Float,  float, 0.5f, 0.0f, 16.0f, kNone, "SSR hit thickness")                        \
    X(render, rtMaxDistance,      Float,  float, 200.0f, 0.0f, 10000.0f, kNone, "RT reflection ray distance")          \
    X(render, fog,                Bool,   bool, false, 0, 0, kNone, "Volumetric fog")                                  \
    X(render, fogDensity,         Float,  float, 0.03f, 0.0f, 1.0f, kNone, "Fog density")                              \
    X(render, fogAnisotropy,      Float,  float, 0.6f, -1.0f, 1.0f, kNone, "Fog anisotropy")                           \
    X(render, fogMaxDistance,     Float,  float, 60.0f, 0.0f, 500.0f, kNone, "Fog max distance")                       \
    X(render, fogHeightFalloff,   Float,  float, 6.0f, 0.0f, 500.0f, kNone, "Fog height falloff")                      \
    X(render, particles,          Bool,   bool, true, 0, 0, kNone, "GPU particles")                                    \
    X(render, particleSort,       Bool,   bool, true, 0, 0, kNone, "Sort blended particles")                           \
    X(render, taa,                Bool,   bool, false, 0, 0, kNone, "Temporal antialiasing")                           \
    X(render, taaBlend,           Float,  float, 0.1f, 0.0f, 1.0f, kNone, "TAA current-frame weight")                  \
    X(input, gamepadDeadzone, Float, float, 0.15f, 0.0f, 0.9f, kNone, "Gamepad deadzone")                              \
    X(input, textRepeatDelay, Float, float, 0.4f, 0.05f, 2.0f, kNone, "Text repeat delay")                             \
    X(input, textRepeatRate,  Float, float, 0.03f, 0.005f, 1.0f, kNone, "Text repeat rate")                            \
    X(camera, fovDegrees,       Float, float, 60.0f, 20.0f, 120.0f, kNone, "Field of view")                            \
    X(camera, moveSpeedScale,   Float, float, 1.0f, 0.05f, 20.0f, kNone, "Move speed")                                 \
    X(camera, orbitSensitivity, Float, float, 0.005f, 0.0005f, 0.05f, kNone, "Orbit sensitivity")                      \
    X(camera, zoomStep,         Float, float, 0.9f, 0.5f, 0.99f, kNone, "Zoom step")                                   \
    X(physics, maxStepsPerFrame, Uint, uint32_t, 4u, 1, 32, kNone, "Max steps per frame")                              \
    X(physics, workerThreads,    Uint, uint32_t, 0u, 0, 64, kInitOnly, "Physics worker threads")                       \
    X(audio, enabled,                Bool,   bool, true, 0, 0, kNone, "Audio")                                         \
    X(audio, sampleRate,             Uint,   uint32_t, 48000u, 8000, 192000, kInitOnly, "Sample rate")                 \
    X(audio, channels,               Uint,   uint32_t, 2u, 1, 2, kInitOnly, "Output channels")                         \
    X(audio, masterVolume,           Float,  float, 1.0f, 0.0f, 1.0f, kNone, "Master volume")                          \
    X(audio, streamThresholdSeconds, Float,  float, 5.0f, 0.0f, 120.0f, kNone, "Stream threshold")                     \
    X(audio, decodeBudgetBytes,      Uint64, uint64_t, 67108864ull, 0, 0, kNone, "Decode budget")                      \
    X(ui, scale,       Float, float, 0.0f, 0.0f, 4.0f, kInitOnly, "UI scale (0 asks the window)")                      \
    X(engine, current_scene_path, String, std::string, "", 0, 0, kEngine, "Current scene")                             \
    X(engine, game_name,          String, std::string, "", 0, 0, kEngine, "Game")
// clang-format on

/**
 * @brief A row's built-in, carrying the type the literal in the list actually had.
 *
 * `4` and `4u` are different defaults here: the tag comes from the *literal*, and the
 * `static_assert` below compares it against the row's declared type, so
 * `X(render, x, Float, float, 0, ...)` fails to build rather than landing in the integer
 * field and reading back correctly for the wrong reason.
 *
 * Adding a `double` constructor breaks that: `1.0` on a Float row would stop being
 * ambiguous and reach the assert as an integer.
 */
struct Default {
    Type type = Type::Bool;
    int64_t integer = 0;             ///< Bool as 0/1, Int, Uint, Uint64
    double real = 0.0;               ///< Float
    const char* text = nullptr;      ///< String; owned elsewhere and outliving the row

    /// For a declared row, filled in after its owned strings exist.
    constexpr Default() = default;

    constexpr Default(bool v) : type(Type::Bool), integer(v ? 1 : 0) {}
    constexpr Default(int v) : type(Type::Int), integer(v) {}
    constexpr Default(unsigned v) : type(Type::Uint), integer(v) {}
    constexpr Default(unsigned long v) : type(Type::Uint64), integer(static_cast<int64_t>(v)) {}
    constexpr Default(unsigned long long v) : type(Type::Uint64), integer(static_cast<int64_t>(v)) {}
    constexpr Default(float v) : type(Type::Float), real(static_cast<double>(v)) {}
    constexpr Default(const char* v) : type(Type::String), text(v) {}
};

// One assertion per row, naming the row: "somewhere in a ninety-row list the default is the
// wrong type" is not a diagnostic anybody can act on.
#define SUBSTRATE_SETTING_TYPECHECK(mod, name, tag, ctype, def, lo, hi, flags, label)                                   \
    static_assert(Default(def).type == Type::tag, #mod "." #name ": the default literal is not the row's own type");
SUBSTRATE_SETTINGS(SUBSTRATE_SETTING_TYPECHECK)
#undef SUBSTRATE_SETTING_TYPECHECK

/// One id per row, spelled `module_name` -- the underscore stands in for the dot an
/// enumerator cannot contain.
enum class Id : uint16_t {
#define SUBSTRATE_SETTING_ID(mod, name, tag, ctype, def, lo, hi, flags, label) mod##_##name,
    SUBSTRATE_SETTINGS(SUBSTRATE_SETTING_ID)
#undef SUBSTRATE_SETTING_ID
    /// One past the engine's last row. A boundary, not a sentinel: the first row a game
    /// declares takes this value as its id.
    Count,
    /// No row, as `find` answers for an unclaimed key. Must not share `Count`'s value, or
    /// the first declared row reads as unknown everywhere `!= Count` is the test -- in
    /// every build that ships a game, and none the unit suite runs.
    None = 0xFFFFu
};

/// How many rows the engine's own list declares, and the bound on `kRows`. A table's total
/// is `rowCount()`, which only an instance can answer.
[[nodiscard]] constexpr uint16_t count() {
    return static_cast<uint16_t>(Id::Count);
}

/// A `uint16_t` in a type wrapper, so `set(exposure, true)` does not compile and a
/// misspelled handle is a build error rather than a runtime no-op.
template <typename T>
struct Setting {
    Id id;
};

/// The inverse of the `ctype` column, and the same six cases.
template <typename T>
[[nodiscard]] constexpr Type typeOf() {
    if constexpr (std::is_same_v<T, bool>) return Type::Bool;
    else if constexpr (std::is_same_v<T, int>) return Type::Int;
    else if constexpr (std::is_same_v<T, uint32_t>) return Type::Uint;
    else if constexpr (std::is_same_v<T, uint64_t>) return Type::Uint64;
    else if constexpr (std::is_same_v<T, float>) return Type::Float;
    else return Type::String;
}

struct Row {
    const char* key;      ///< "render.msaaSamples" -- the JSON path, mirrored exactly
    const char* label;    ///< what a panel writes beside the widget
    Type type;
    uint8_t flags;
    double minimum;       ///< ignored for Bool and String; equal bounds mean unbounded
    double maximum;
    Default builtIn;
};

/// A row's built-in, spelled exactly as `Settings::valueString` spells a live value, so
/// "does this differ from its default" stays a string comparison against the row.
[[nodiscard]] std::string defaultString(const Row& r);

/// Human name for a provenance, as the dump prints it: `default | config | game | cli`.
[[nodiscard]] const char* sourceName(Source s);

/// A key that used to be a setting, and the sentence explaining where it went. Without an
/// entry here, a user's setting stops working with no message at all.
struct RemovedKey {
    const char* key;
    const char* message;
};
[[nodiscard]] const std::vector<RemovedKey>& removedKeys();

/**
 * @brief Every setting's value, its provenance, and where its live copy lives.
 *
 * One instance, owned by `Engine`. Not copyable: a second table holding the same live
 * pointers is two writers of one field.
 *
 * `slots` and `declared` must stay `std::deque`. `getString` and `origin` hand out a
 * `const std::string&` into a slot, `row` hands out a `const Row&`, and a declared row's
 * `key` is a `const char*` into a string the table owns -- a `std::vector` moves all of
 * those out from under their holders on the reallocation a later `declare` causes.
 */
class Settings {
  public:
    Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    /// The row behind an id. Out of range -- `Id::None`, or a handle from a refused
    /// `declare` -- yields the first row rather than reading past the table, and warns once
    /// rather than once a frame.
    [[nodiscard]] const Row& row(Id id) const;

    /// `Id::None` when nothing claims the key. Searches the engine's rows first, so a game
    /// cannot shadow an engine key.
    [[nodiscard]] Id find(std::string_view key) const;

    /// Every row this table has, engine and declared. `count()` is the engine's half.
    [[nodiscard]] uint16_t rowCount() const;

    /**
     * @brief Add a row at run time, and answer with a handle carrying its type.
     *
     * What `Game::declareSettings` calls. `minimum == maximum` means unbounded.
     *
     * Ownership is by *module*, not by key: a `render.` key a game added is
     * indistinguishable from an engine row in the file, the panel and
     * `--write-default-config`, and a later engine release adding that key takes over the
     * game's value.
     *
     * @return a handle naming `Id::None` when refused, which reads as the first row and
     *         writes nothing. Refused after `freezeRows()`; for a key that does not parse
     *         as `module.name`; for a module the engine names; for a key that used to be an
     *         engine setting, since a config file may still carry it; for `kEngine`, which
     *         a game cannot own; and for a key some row already claims.
     */
    template <typename T>
    Setting<T> declare(std::string_view key, T builtIn, std::string_view label, double minimum = 0.0,
                       double maximum = 0.0, uint8_t flags = kNone) {
        static_assert(std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, uint32_t> ||
                          std::is_same_v<T, uint64_t> || std::is_same_v<T, float> || std::is_same_v<T, std::string>,
                      "a row is one of the six types the table holds, and a String row's built-in is a std::string -- "
                      "declare(key, std::string(\"aces\"), ...) rather than a literal");
        if constexpr (std::is_same_v<T, std::string>) {
            return {declareRow(key, Type::String, Default(""), builtIn, minimum, maximum, flags, label)};
        } else {
            return {declareRow(key, typeOf<T>(), Default(builtIn), {}, minimum, maximum, flags, label)};
        }
    }

    /// Refuse any further declaration. `Engine::init` calls it the moment
    /// `Game::declareSettings` returns and before the config file is read, which is what
    /// keeps `declareSettings` adding rows and `configure` only writing them.
    void freezeRows() { schemaFrozen = true; }
    [[nodiscard]] bool rowsFrozen() const { return schemaFrozen; }

    template <typename T>
    [[nodiscard]] T get(Setting<T> s) const {
        if constexpr (std::is_same_v<T, bool>) return getBool(s.id);
        else if constexpr (std::is_same_v<T, int>) return getInt(s.id);
        else if constexpr (std::is_same_v<T, uint32_t>) return getUint(s.id);
        else if constexpr (std::is_same_v<T, uint64_t>) return getUint64(s.id);
        else if constexpr (std::is_same_v<T, float>) return getFloat(s.id);
        else return getString(s.id);
    }

    /// Writes over whatever stands. A game stating a *default* wants `setDefault`, which
    /// loses to the user's `substrate.json` where this would silently beat it.
    template <typename T>
    bool set(Setting<T> s, T value, Source from = Source::Game) {
        if constexpr (std::is_same_v<T, std::string>) return setValue(s.id, std::move(value), from, "");
        else return setValue(s.id, value, from, "");
    }

    /// @brief Write only where nothing has claimed the row yet, recording `Source::Game`.
    ///
    /// @return false when the row is already claimed. The ordinary case, not an error, and
    ///         nothing is logged; a refusal `set` would also have made logs why.
    template <typename T>
    bool setDefault(Setting<T> s, T value) {
        // Against `Source::Default` specifically rather than "anything below `Game`", so a
        // tier inserted below `Game` later is not silently overwritten.
        if (source(s.id) != Source::Default) return false;
        return set(s, std::move(value));
    }

    // The one place a value is written, and the one place every refusal lives. A second
    // write path is a second refusal policy.
    bool setValue(Id id, bool value, Source from, std::string_view origin);
    bool setValue(Id id, int value, Source from, std::string_view origin);
    bool setValue(Id id, uint32_t value, Source from, std::string_view origin);
    bool setValue(Id id, uint64_t value, Source from, std::string_view origin);
    bool setValue(Id id, float value, Source from, std::string_view origin);
    bool setValue(Id id, std::string value, Source from, std::string_view origin);

    /**
     * @brief Set by name, parsing `value` according to the row's type.
     *
     * @param origin the file or the flag the value arrived on, for the dump's last column.
     * @return false for an unknown key, an `engine.` key, a value that does not parse, or
     *         an `initOnly` row after `freezeInitOnly()`. Every refusal logs why.
     */
    bool setFromString(std::string_view key, std::string_view value, Source from, std::string_view origin = {});

    /**
     * @brief Read every row this document names, and account for every key it does not.
     *
     * Walks the *file*, not the rows: walking the rows applies the file just as well and
     * turns a key nobody reads into silence. A key in a module nothing declares is an
     * orphan -- refused but kept, because `saveJson` merges into the file it read.
     *
     * @param doc a `rapidjson::Value` object, type-erased so this header does not put
     *        rapidjson in front of everything that reads a setting.
     * @param origin the file's path, for the dump's last column.
     */
    void loadJson(const void* doc, std::string_view origin);

    /**
     * @brief Write the live values back into `path`, keeping everything else in it.
     *
     * Writes what differs from the default, plus every key already in the file. A key set
     * back to its default is updated where present and not introduced where absent, so the
     * file stays diffable and a hand-typed key is never dropped. `engine.` rows are never
     * written; they have no JSON key.
     *
     * @return false, with nothing written, if the existing file is not valid JSON --
     *         overwriting it would lose whatever the parser could not read.
     */
    bool saveJson(const std::string& path) const;

    /// Write an `engine.` row, which `setFromString` refuses. For the API that owns the
    /// side effect, not for a string door: loading a scene is not assigning a string.
    void setEngineOwned(Id id, std::string_view value);

    [[nodiscard]] bool getBool(Id id) const;
    [[nodiscard]] int getInt(Id id) const;
    [[nodiscard]] uint32_t getUint(Id id) const;
    [[nodiscard]] uint64_t getUint64(Id id) const;
    [[nodiscard]] float getFloat(Id id) const;
    [[nodiscard]] const std::string& getString(Id id) const;

    /// The value as the dump prints it.
    [[nodiscard]] std::string valueString(Id id) const;

    [[nodiscard]] Source source(Id id) const;
    /// The file or flag that supplied the current value; empty for a default.
    [[nodiscard]] const std::string& origin(Id id) const;

    /// @brief Point a row at the field its value belongs in, and push the value there now.
    ///
    /// `address` must point at the row's `ctype` and the table cannot check it, which is
    /// why the only caller is `bindRenderer` and not anything a game reaches.
    ///
    /// An id naming no row is refused rather than clamped to the first row: a wrong read is
    /// one wrong answer, a wrong bind is a wrong address written through every frame.
    void bindLive(Id id, void* address);

    /// Refuse `initOnly` rows from here on, and freeze the schema with them -- by the time
    /// `Engine::init` returns, the dump, the panel and `bindRenderer` have all read the
    /// table, and a row appearing behind them has no defined answer.
    void freezeInitOnly() {
        initFrozen = true;
        freezeRows();
    }
    [[nodiscard]] bool initOnlyFrozen() const { return initFrozen; }

    void dumpTable(std::FILE* out) const;
    void dumpJson(std::FILE* out) const;

  private:
    struct Slot {
        int64_t integer = 0;  ///< Bool as 0/1, Int, Uint, Uint64 reinterpreted
        double real = 0.0;    ///< Float
        std::string text;     ///< String
        void* live = nullptr; ///< the bound field, or null
        Source src = Source::Default;
        std::string from;
    };

    /// A row `declare` added, and the strings it has to own. `Row::key`, `Row::label` and a
    /// String row's `Row::builtIn.text` point into these three, so whatever holds a
    /// `Declared` may never move it.
    struct Declared {
        std::string key;
        std::string label;
        std::string text;
        Row row;
    };

    /// The one place an id becomes storage, and the one place an id naming no row is
    /// answered. Indexing `slots` directly is only safe while every `Id` is an enumerator,
    /// which `declare` ended.
    [[nodiscard]] Slot& slot(Id id);
    [[nodiscard]] const Slot& slot(Id id) const;

    /// Whether any row of this table lives in `module`. What separates a misspelled engine
    /// key from an orphaned game section when `loadJson` walks a key nothing claims.
    [[nodiscard]] bool claimsModule(std::string_view module) const;

    /// `declare`'s untyped half, so the refusals are not instantiated once per type. `text`
    /// is the built-in for a String row and ignored otherwise.
    Id declareRow(std::string_view key, Type type, Default builtIn, std::string_view text, double minimum,
                  double maximum, uint8_t flags, std::string_view label);

    /// The one place that knows which of a slot's three fields a type lives in.
    static void store(Slot& s, bool v);
    static void store(Slot& s, int v);
    static void store(Slot& s, uint32_t v);
    static void store(Slot& s, uint64_t v);
    static void store(Slot& s, float v);
    static void store(Slot& s, std::string v);

    static void storeDefault(Slot& s, const Row& r);

    /// One row, from one type-erased `rapidjson::Value`. A value of the wrong type keeps
    /// the default and says so.
    void applyJson(Id id, const void* value, std::string_view origin);

    /// Whether a write of `type` to `id` is allowed, and why not when it is not. Every
    /// `setValue` overload must ask this first, or there are two refusal policies.
    bool writable(Id id, Type type, Source from);

    /// Record the provenance and push the value into the bound field, if there is one.
    void published(Id id, Source from, std::string_view origin);

    // Deques, not vectors -- see the class comment.
    std::deque<Slot> slots;
    std::deque<Declared> declared;
    bool initFrozen = false;
    bool schemaFrozen = false;
    mutable bool warnedUnknownId = false;
};

/// @brief Point every renderer-backed row at the renderer's own field, so default, config
///        file and command line all reach the renderer by one path.
///
/// Defined in `engine/gfx/SettingsBind.cpp`, and a free function rather than a method:
/// `Settings.cpp` is in `SUBSTRATE_HOSTED_SOURCES`, so a member touching `gfx::Renderer`
/// would drag Vulkan into it and drop it out of `scripts/test.sh tsan`.
void bindRenderer(Settings& settings, gfx::Renderer& renderer);

} // namespace settings

/// The typed handles, one per row. The constant path mirrors the JSON path exactly:
/// `options::render::msaaSamples` against `render.msaaSamples`.
namespace options {
#define SUBSTRATE_SETTING_HANDLE(mod, name, tag, ctype, def, lo, hi, flags, label)                                      \
    namespace mod {                                                                                                    \
    inline constexpr settings::Setting<ctype> name{settings::Id::mod##_##name};                                        \
    }
SUBSTRATE_SETTINGS(SUBSTRATE_SETTING_HANDLE)
#undef SUBSTRATE_SETTING_HANDLE
} // namespace options

/**
 * @brief Each row's built-in, as a constant the field it binds to can initialise from.
 *
 * A field `bindRenderer` binds must initialise from here. Spelling the literal out on the
 * field instead writes the value twice and lets the two drift, and nothing catches it --
 * `bindRenderer` runs before the first frame reads the field.
 *
 * `auto` rather than the row's `ctype`, the one place this does not mirror `options`,
 * because `constexpr std::string` does not exist.
 */
namespace defaults {
#define SUBSTRATE_SETTING_DEFAULT(mod, name, tag, ctype, def, lo, hi, flags, label)                                     \
    namespace mod {                                                                                                    \
    inline constexpr auto name = def;                                                                                  \
    }
SUBSTRATE_SETTINGS(SUBSTRATE_SETTING_DEFAULT)
#undef SUBSTRATE_SETTING_DEFAULT
} // namespace defaults

} // namespace core
