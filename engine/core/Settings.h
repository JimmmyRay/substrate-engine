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
 * ## Two namespaces, and the prefix says which door a value came in through
 *
 * | Prefix | Meaning | Spelling |
 * |---|---|---|
 * | *(module)* | Present in `substrate.json`; the key **is** the JSON path | the JSON key's own, camelCase |
 * | `engine.` | Engine-owned live state; no JSON key at all | snake_case |
 *
 * The casing difference carries the distinction the prefix already makes: a mirrored key's
 * second half is *forced* to be the JSON key's own spelling, so it cannot be respelled.
 *
 * **`engine.` rows are readable and dumpable, and writable only through the API that owns
 * the side effect.** The string door refuses them -- loading a scene is not assigning a
 * string.
 *
 * ## Refresh is polling, not dispatch
 *
 * No `switch (module)`, no notification, no observer list. `Renderer::drawFrame` compares
 * `featureKey()` against the built one every frame; everything else is a live field read
 * per frame. `bindLive` points each row at the field it belongs in, so config, command line
 * and default all arrive by one path and no copy of a value can drift from another.
 *
 * An engine row is an enumerator into a `constexpr` array, so
 * `core::options::render::msaaSamples` is a compile-time typed handle and a misspelling is
 * a build error. A row `declare` adds gets its id at run time and is typed the same way.
 */
namespace settings {

/// The C++ type behind a row, and what the string door parses into.
enum class Type : uint8_t { Bool, Int, Uint, Uint64, Float, String };

/**
 * @brief Where the live value came from, in increasing precedence.
 *
 * **The precedence is the order the doors are opened in, not a rule inside the setter.**
 * `Engine::init` reads `substrate.json`, calls `Game::configure`, then applies the flags,
 * so a higher tier winning is simply a later write. A rule in the setter would also refuse
 * a panel toggle or a keypress -- both `Source::Game` -- over a flag set at startup, and a
 * runtime write beating a startup one is why the table binds live fields at all.
 *
 * The one predicate an ordering cannot express is a game's *default*, which has to lose to
 * a file read before it. That is `Settings::setDefault`.
 */
enum class Source : uint8_t { Default, Config, Game, Cli };

/// Row flags. `Engine` marks an `engine.` row -- no JSON key, refused by the string
/// setter. `InitOnly` marks a row whose value sizes something at init, so a later write is
/// refused *with a reason* rather than clamped in silence.
enum Flags : uint8_t { kNone = 0, kEngine = 1u << 0, kInitOnly = 1u << 1 };

// clang-format off
/**
 * @brief The list. Everything else in this file and its `.cpp` is generated from it.
 *
 * `X(module, name, TypeTag, ctype, default, minimum, maximum, flags, label)`
 *
 * `minimum` and `maximum` bound a slider and clamp a parse; they are ignored for `Bool`
 * and `String`. A row whose two bounds are equal is unbounded.
 *
 * Authored decisions live in `GameSetup` and developer controls live on `Config` as
 * flags; neither gets a key here. **Apply that test before adding a row** -- see
 * principles.md rule 7, "A setting is a property of the person running the program". A row
 * is one line and comes with a parser, a
 * flag, a panel widget and persistence for free, which is how thirty-eight rows that were
 * not settings once got in. Every departed key is in `removedKeys()`.
 */
#define SUBSTRATE_SETTINGS(X)                                                                                          \
    /* --------------------------------------------------------------------------- window */                          \
    X(window, width,  Int, int, 1600, 320, 7680, kNone, "Window width")                                                \
    X(window, height, Int, int,  900, 240, 4320, kNone, "Window height")                                               \
    X(window, vsync,  Bool, bool, false, 0, 0, kNone, "VSync")                                                         \
    /* --------------------------------------------------------------------------- render */                          \
    X(render, msaaSamples,        Uint,   uint32_t, 4u, 1, 8, kNone, "MSAA samples")                                   \
    /* One atlas serves the overlay, the panel, the inspector and every game-drawn string; */                          \
    /* the name says `debug` only because the overlay asked for it first. `initOnly`       */                          \
    /* because the atlas is baked when the renderer starts, so a later write is inert.     */                          \
    X(render, debugFont,          String, std::string, "", 0, 0, kInitOnly, "UI font (empty = embedded bitmap)")       \
    X(render, debugFontHeight,    Float,  float, 16.0f, 8.0f, 64.0f, kInitOnly, "UI font height")                      \
    X(render, ssao,               Bool,   bool, true, 0, 0, kNone, "SSAO")                                             \
    /* Bounded by what the value is *for*, not by how large a scene may be: an occlusion   */                          \
    /* hemisphere is contact scale at every scene size, so neither derives from the bounds.*/                          \
    X(render, ssaoRadius,         Float,  float, 0.5f, 0.0f, 16.0f, kNone, "SSAO hemisphere radius")                   \
    X(render, ssaoBias,           Float,  float, 0.025f, 0.0f, 1.0f, kNone, "SSAO depth bias, in metres")              \
    X(render, shadows,            Bool,   bool, true, 0, 0, kNone, "Shadow maps (non-RT path)")                        \
    X(render, punctualShadows,    Bool,   bool, true, 0, 0, kNone, "Point and spot light shadows")                 \
    X(render, shadowCache,        Bool,   bool, true, 0, 0, kNone, "Reuse unchanged shadow atlas layers")          \
    X(render, shadowDistance,     Float,  float, 0.0f, 0.0f, 500.0f, kNone, "Shadow box cap (0 = scene bounds)")       \
    /* Below this the deferred light loop drops a light and its shadow lookup entirely.   */                           \
    /* Post-exposure radiance, so the number means the same thing in a game that authored */                           \
    /* a different `GameSetup::look.exposure`; 0 is today's behaviour, bit for bit. The ceiling */                           \
    /* is a tenth of display white -- far past anything a viewer would call imperceptible, */                          \
    /* and above it this is not a cutoff but a way of turning lights off.                  */                          \
    X(render, lightCutoff,        Float,  float, 0.0f, 0.0f, 0.1f, kNone, "Light cutoff (post-exposure radiance)")      \
    X(render, bloom,              Bool,   bool, true, 0, 0, kNone, "Bloom")                                            \
    X(render, bloomThreshold,     Float,  float, 1.1f, 0.0f, 64.0f, kNone, "Bloom threshold")                          \
    X(render, bloomSoftKnee,      Float,  float, 0.6f, 0.0f, 8.0f, kNone, "Bloom soft knee")                           \
    X(render, bloomStrength,      Float,  float, 0.05f, 0.0f, 1.0f, kNone, "Bloom strength")                           \
    X(render, edgeMsaa,           Bool,   bool, true, 0, 0, kNone, "Edge-detect MSAA")                                 \
    X(render, culling,            Bool,   bool, true, 0, 0, kNone, "GPU frustum culling")                              \
    X(render, occlusionCulling,   Bool,   bool, true, 0, 0, kNone, "Two-pass Hi-Z occlusion culling")                 \
    /* On by default, so every golden run re-proves that no reference camera selects a    */                           \
    /* level; off, the code rots.                                                         */                           \
    X(render, meshLod,            Bool,   bool, true, 0, 0, kNone, "Screen-coverage mesh LOD")                         \
    /* Fraction of the viewport an instance must cover to stay at LOD 0; each level below */                           \
    /* it is a quarter of this. 1/4096 is a 20x20 pixel footprint at 1600x900, measured   */                           \
    /* rather than chosen.                                                                */                           \
    X(render, lodThreshold,       Float,  float, 0.000244f, 0.0f, 1.0f, kNone, "LOD 1 coverage threshold")             \
    /* Assign lights to 16x16 screen tiles in compute, so the deferred loop iterates the  */                           \
    /* lights that can reach the pixel rather than the lights in the view. On by default  */                           \
    /* for the reason the three rows above it are: it is faster *and* equivalent, and an  */                           \
    /* escape hatch is how the second half gets proven rather than asserted -- the same   */                           \
    /* frame with this off must match it on, to the byte.                                 */                           \
    X(render, lightTiles,      Bool,   bool, true, 0, 0, kNone, "Tiled light assignment")                       \
    X(render, rt,                 Bool,   bool, true, 0, 0, kNone, "Ray tracing")                                      \
    X(render, rtShadows,          Bool,   bool, true, 0, 0, kNone, "Traced shadow rays")                               \
    /* Trace once per distinct fragment into a mask instead of once per MSAA sample     */                             \
    /* inside the light loop. Inert at 1x and wherever the two rows above are off.      */                             \
    /* Off by default because it is an approximation: it moves pixels at silhouettes,   */                             \
    /* six golden cases' worth. See rendering.md, "The shadow mask".                    */                             \
    X(render, rtShadowMask,       Bool,   bool, false, 0, 0, kNone, "Per-fragment traced shadow mask")                 \
    X(render, ssr,                Bool,   bool, true, 0, 0, kNone, "Screen-space reflections")                         \
    X(render, ssrRoughnessCutoff, Float,  float, 0.4f, 0.0f, 1.0f, kNone, "SSR roughness cutoff")                      \
    X(render, ssrIntensity,       Float,  float, 1.0f, 0.0f, 4.0f, kNone, "SSR intensity")                             \
    /* Fraction of the render extent the reflection pass runs at. Rebuilds `ssrTarget` and  */                         \
    /* switches the composite to a joint-bilateral upsample below 1.0; at 1.0 the full-     */                         \
    /* resolution path is bit-for-bit what it was. The floor is 0.25 because a quarter-axis */                         \
    /* reflection is already sixteen rays per sixteen pixels.                               */                         \
    X(render, ssrScale,           Float,  float, 1.0f, 0.25f, 1.0f, kNone, "SSR resolution scale")                     \
    /* 500 is the world-unit ceiling `shadowDistance` and `fogMaxDistance` also use, so the */                         \
    /* three agree. `rtMaxDistance` keeps 10000 because a ray query costs the same at any   */                         \
    /* range and so is not the same kind of number.                                         */                         \
    X(render, ssrMaxDistance,     Float,  float, 8.0f, 0.0f, 500.0f, kNone, "SSR march distance")                      \
    /* How far behind a surface a march may be and still count as hitting it.               */                         \
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
    /* ---------------------------------------------------------------------------- input */                          \
    X(input, gamepadDeadzone, Float, float, 0.15f, 0.0f, 0.9f, kNone, "Gamepad deadzone")                              \
    X(input, textRepeatDelay, Float, float, 0.4f, 0.05f, 2.0f, kNone, "Text repeat delay")                             \
    X(input, textRepeatRate,  Float, float, 0.03f, 0.005f, 1.0f, kNone, "Text repeat rate")                            \
    /* --------------------------------------------------------------------------- camera */                          \
    X(camera, fovDegrees,       Float, float, 60.0f, 20.0f, 120.0f, kNone, "Field of view")                            \
    X(camera, moveSpeedScale,   Float, float, 1.0f, 0.05f, 20.0f, kNone, "Move speed")                                 \
    X(camera, orbitSensitivity, Float, float, 0.005f, 0.0005f, 0.05f, kNone, "Orbit sensitivity")                      \
    X(camera, zoomStep,         Float, float, 0.9f, 0.5f, 0.99f, kNone, "Zoom step")                                   \
    /* -------------------------------------------------------------------------- physics */                          \
    X(physics, maxStepsPerFrame, Uint, uint32_t, 4u, 1, 32, kNone, "Max steps per frame")                              \
    X(physics, workerThreads,    Uint, uint32_t, 0u, 0, 64, kInitOnly, "Physics worker threads")                       \
    /* ---------------------------------------------------------------------------- audio */                          \
    X(audio, enabled,                Bool,   bool, true, 0, 0, kNone, "Audio")                                         \
    X(audio, sampleRate,             Uint,   uint32_t, 48000u, 8000, 192000, kInitOnly, "Sample rate")                 \
    X(audio, channels,               Uint,   uint32_t, 2u, 1, 2, kInitOnly, "Output channels")                         \
    X(audio, masterVolume,           Float,  float, 1.0f, 0.0f, 1.0f, kNone, "Master volume")                          \
    X(audio, streamThresholdSeconds, Float,  float, 5.0f, 0.0f, 120.0f, kNone, "Stream threshold")                     \
    X(audio, decodeBudgetBytes,      Uint64, uint64_t, 67108864ull, 0, 0, kNone, "Decode budget")                      \
    /* ------------------------------------------------------------------------------- ui */                          \
    X(ui, scale,       Float, float, 0.0f, 0.0f, 4.0f, kInitOnly, "UI scale (0 asks the window)")                      \
    /* ------------------------------------------- engine-owned; no JSON key, read-only */                             \
    X(engine, current_scene_path, String, std::string, "", 0, 0, kEngine, "Current scene")                             \
    X(engine, game_name,          String, std::string, "", 0, 0, kEngine, "Game")
// clang-format on

/**
 * @brief A row's built-in, carrying the type the literal in the list actually had.
 *
 * The tag is what the *literal* is, not what the row declares, and the `static_assert`
 * below compares the two -- so `X(render, x, Float, float, 0, ...)` is caught rather than
 * landing the 0 in the integer field and reading back correctly for the wrong reason.
 * **`4` and `4u` are different defaults here.**
 *
 * There is deliberately no `double` constructor: a Float row written `1.0` is ambiguous
 * between the `float` and integer constructors and fails to compile, rather than reaching
 * the assert as an integer.
 */
struct Default {
    Type type = Type::Bool;
    int64_t integer = 0;             ///< Bool as 0/1, Int, Uint, Uint64
    double real = 0.0;               ///< Float
    const char* text = nullptr;      ///< String; owned elsewhere and outliving the row

    /// For a declared row, which is filled in after its owned strings exist. An engine row
    /// never uses it -- the list always writes a literal.
    constexpr Default() = default;

    constexpr Default(bool v) : type(Type::Bool), integer(v ? 1 : 0) {}
    constexpr Default(int v) : type(Type::Int), integer(v) {}
    constexpr Default(unsigned v) : type(Type::Uint), integer(v) {}
    constexpr Default(unsigned long v) : type(Type::Uint64), integer(static_cast<int64_t>(v)) {}
    constexpr Default(unsigned long long v) : type(Type::Uint64), integer(static_cast<int64_t>(v)) {}
    constexpr Default(float v) : type(Type::Float), real(static_cast<double>(v)) {}
    constexpr Default(const char* v) : type(Type::String), text(v) {}
};

// One assertion per row, naming the row, because "somewhere in a ninety-row list the
// default is the wrong type" is not a diagnostic anybody can act on.
#define SUBSTRATE_SETTING_TYPECHECK(mod, name, tag, ctype, def, lo, hi, flags, label)                                   \
    static_assert(Default(def).type == Type::tag, #mod "." #name ": the default literal is not the row's own type");
SUBSTRATE_SETTINGS(SUBSTRATE_SETTING_TYPECHECK)
#undef SUBSTRATE_SETTING_TYPECHECK

/// One id per row, spelled `module_name`. The underscore is what makes `engine.` keys and
/// mirrored keys share one enum without the dot they cannot contain.
enum class Id : uint16_t {
#define SUBSTRATE_SETTING_ID(mod, name, tag, ctype, def, lo, hi, flags, label) mod##_##name,
    SUBSTRATE_SETTINGS(SUBSTRATE_SETTING_ID)
#undef SUBSTRATE_SETTING_ID
    /// One past the engine's last row, and **only** that. A row a game declares takes this
    /// value as its id, so it is a boundary rather than a sentinel.
    Count,
    /// No row. `find` answers this for a key nothing claims. **Deliberately not `Count`**
    /// -- sharing that value makes the first declared row read as *unknown* everywhere
    /// `!= Count` is the test, in every build that ships a game and none the unit suite runs.
    None = 0xFFFFu
};

/// How many rows the engine's own list declares. The boundary between a compile-time id
/// and a declared one, and the bound on `kRows`; a table's *total* is `rowCount()`, which
/// only an instance can answer because only an instance knows what has been declared.
[[nodiscard]] constexpr uint16_t count() {
    return static_cast<uint16_t>(Id::Count);
}

/// A `uint16_t` in a type wrapper. The wrapper is the whole point: `set(exposure, true)`
/// does not compile, and a misspelled handle is a build error rather than a runtime no-op.
/// A handle for an engine row is `constexpr` and one for a declared row is not, and that is
/// the *only* difference between them -- the type is carried the same way either way.
template <typename T>
struct Setting {
    Id id;
};

/// The row type behind a C++ type, so `declare` needs no tag argument beside the value it
/// is already given. The inverse of the `ctype` column, and the same six cases.
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
    Default builtIn;      ///< D16. The default lives here, not in the constructor
};

/// A row's built-in, spelled exactly as `Settings::valueString` spells a live value, so
/// *"does this differ from its default"* is a string comparison against the row rather
/// than against a second table built to answer it.
[[nodiscard]] std::string defaultString(const Row& r);

/// Human name for a provenance, as the dump prints it: `default | config | game | cli`.
[[nodiscard]] const char* sourceName(Source s);

/// A key that used to be a setting, and the sentence that explains where it went. The
/// alternative is a user whose setting stopped working with no message at all.
struct RemovedKey {
    const char* key;
    const char* message;
};
[[nodiscard]] const std::vector<RemovedKey>& removedKeys();

/**
 * @brief Every setting's value, its provenance, and where its live copy lives.
 *
 * One instance, owned by `Engine`. Not copyable: a second table holding the same live
 * pointers would be two writers of one field.
 *
 * Storage is two containers, which is what lets the table grow without costing the
 * compile-time handles:
 *
 * - The engine's rows are a `constexpr Row kRows[]` in the `.cpp`, generated from
 *   `SUBSTRATE_SETTINGS` and indexed by an `Id` enumerator. Growth cannot touch them, so
 *   `core::options::render::msaaSamples` stays `constexpr` and a typo stays a compile error.
 * - A row `declare` adds goes in a second container, with an id at or above `Id::Count`.
 *
 * **Both growable containers are `std::deque`, and it is load-bearing.** `getString` and
 * `origin` hand out a `const std::string&` into a slot, `row` hands out a `const Row&`, and
 * a declared row's `key` is a `const char*` into a string the table owns. A `std::vector`
 * would move every one of those on the reallocation a later `declare` causes.
 */
class Settings {
  public:
    Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    // ------------------------------------------------------------------ the rows
    /// The row behind an id. Out of range -- `Id::None`, or a handle from a refused
    /// `declare` -- yields the first row rather than reading past the table, and warns
    /// once rather than once a frame.
    [[nodiscard]] const Row& row(Id id) const;

    /// `Id::None` when nothing claims the key. Searches the engine's rows before the
    /// declared ones, so a game cannot shadow an engine key.
    [[nodiscard]] Id find(std::string_view key) const;

    /// Every row this table has, engine and declared -- the bound for walking the table.
    /// `count()` is the engine's own half of it.
    [[nodiscard]] uint16_t rowCount() const;

    /**
     * @brief Add a row at run time, and answer with a handle carrying its type.
     *
     * What `Game::declareSettings` calls. Everything downstream treats the row as
     * ordinary -- it loads, saves, dumps, clamps, takes `--set`, records provenance and
     * draws in the generated panel. `minimum == maximum` means unbounded.
     *
     * **A game owns every module the engine does not name**, and the unit of ownership is
     * the module rather than the key: a `render.` key a game added would be
     * indistinguishable from an engine row in the file, the panel and
     * `--write-default-config`, and a later engine release adding that key would take over
     * the game's value.
     *
     * @return a handle naming `Id::None` when the declaration is refused. The whole ladder,
     *         in the order it is applied: anything at all after `freezeRows()`; a key
     *         nothing could parse as `module.name`; a module the engine names; a key that
     *         *used to be* an engine setting, which a config file may still carry; a row
     *         asking for `kEngine`, which is live state a game cannot own; and a key some
     *         row already claims. Every refusal logs why, and the refused handle reads as
     *         the first row and writes nothing.
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

    /**
     * @brief Refuse any further declaration, because the schema is now something other
     *        code holds references into.
     *
     * `Engine::init` calls it the moment `Game::declareSettings` returns, **before the
     * config file is read** -- which is what makes the split between the two hooks
     * enforced rather than documented: `declareSettings` may only add rows and `configure`
     * may only write them.
     */
    void freezeRows() { schemaFrozen = true; }
    [[nodiscard]] bool rowsFrozen() const { return schemaFrozen; }

    // ------------------------------------------------------------------- typed door
    // For engine and game code. The handle carries the type, so the compiler checks the
    // pairing rather than the runtime discovering it.

    template <typename T>
    [[nodiscard]] T get(Setting<T> s) const {
        if constexpr (std::is_same_v<T, bool>) return getBool(s.id);
        else if constexpr (std::is_same_v<T, int>) return getInt(s.id);
        else if constexpr (std::is_same_v<T, uint32_t>) return getUint(s.id);
        else if constexpr (std::is_same_v<T, uint64_t>) return getUint64(s.id);
        else if constexpr (std::is_same_v<T, float>) return getFloat(s.id);
        else return getString(s.id);
    }

    /// **My answer regardless.** Writes over whatever stands. What a fixed-resolution
    /// game, a benchmark harness and every runtime write want; a game stating a default
    /// wants `setDefault` below.
    template <typename T>
    bool set(Setting<T> s, T value, Source from = Source::Game) {
        if constexpr (std::is_same_v<T, std::string>) return setValue(s.id, std::move(value), from, "");
        else return setValue(s.id, value, from, "");
    }

    /**
     * @brief **My answer unless you said otherwise.** Writes only where nothing has
     *        claimed the row yet, and records `Source::Game` when it does.
     *
     * The door a game reaches for: a game shipping its own answer for vsync or MSAA must
     * lose to the user's `substrate.json`. The dump names `game` for both doors.
     *
     * @return false when the row is already claimed -- the ordinary case, not an error,
     *         and nothing is logged for it. A refusal `set` would also have made returns
     *         false too and logs why.
     */
    template <typename T>
    bool setDefault(Setting<T> s, T value) {
        // Against `Source::Default` specifically rather than "anything below `Game`", so a
        // tier inserted below `Game` later is not silently overwritten.
        if (source(s.id) != Source::Default) return false;
        return set(s, std::move(value));
    }

    // The one place a value is written, once per type. Both doors land here, and so does
    // every refusal: an unknown key, an `engine.` key from the string door, a type that
    // does not match the row, and an `initOnly` row after the thing it sized was sized.
    bool setValue(Id id, bool value, Source from, std::string_view origin);
    bool setValue(Id id, int value, Source from, std::string_view origin);
    bool setValue(Id id, uint32_t value, Source from, std::string_view origin);
    bool setValue(Id id, uint64_t value, Source from, std::string_view origin);
    bool setValue(Id id, float value, Source from, std::string_view origin);
    bool setValue(Id id, std::string value, Source from, std::string_view origin);

    // ------------------------------------------------------------------ string door
    // For the console, JSON parsing and the inspector, all of which are inherently
    // dynamic. Both doors land in `applyById`.

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
     * **Walks the file, not the rows** -- walking the rows alone would apply the file but
     * turn a key nobody reads into silence. A departed key is answered with where it went;
     * a key in a module nothing declares is an **orphan** (a different game is running, or
     * this one dropped the setting), refused but *kept*, since `saveJson` merges into the
     * file it read; anything else is a typo.
     *
     * @param doc a `rapidjson::Value` object, taken as `const void*` so this header does
     *        not put rapidjson in front of everything that reads a setting. The one caller
     *        is `Config::loadFromFile`.
     * @param origin the file's path, for the dump's last column.
     */
    void loadJson(const void* doc, std::string_view origin);

    /**
     * @brief Write the live values back into `path`, keeping everything else in it.
     *
     * **What it writes is what differs from the default, plus every key already in the
     * file** -- the same rule `saveBindings` follows. A key set back to its default is
     * updated where it is present and not introduced where it is absent, so the file stays
     * diffable and a hand-typed key is never dropped.
     *
     * `engine.` rows are never written; they have no JSON key.
     *
     * @return false, with nothing written, if the existing file is not valid JSON --
     *         overwriting it would lose whatever the parser could not read.
     */
    bool saveJson(const std::string& path) const;

    /// Write an `engine.` row. Deliberately separate from `setFromString`, which refuses
    /// them: this is the API that owns the side effect calling in, not a string door.
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

    // ------------------------------------------------------------------ live fields
    /**
     * @brief Point a row at the field its value belongs in, and push the value there now.
     *
     * **`address` must point at the row's `ctype` and the table cannot check that**, which
     * is why the only caller is `bindRenderer` in `engine/gfx/SettingsBind.cpp` rather than
     * anything a game reaches. After binding, `get` reads through the pointer, so a value a
     * keypress toggled at runtime is what the dump reports.
     *
     * An id that names no row is refused rather than clamped: reading the first row for an
     * unknown id is a wrong answer, binding it would be a wrong address written through
     * every frame.
     */
    void bindLive(Id id, void* address);

    /// Refuse `initOnly` rows from here on, because what they size has now been sized. It
    /// freezes the schema too: by the time `Engine::init` returns, the dump, the panel and
    /// `bindRenderer` have all read the table, and a row appearing behind them would have
    /// no defined answer.
    void freezeInitOnly() {
        initFrozen = true;
        freezeRows();
    }
    [[nodiscard]] bool initOnlyFrozen() const { return initFrozen; }

    // ------------------------------------------------------------------------- dump
    // The table has to be *enumerable*, not merely addressable by string: a setter alone
    // would let you write any key and read none.

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

    /// A row `declare` added, and the strings it has to own. `Row::key`, `Row::label` and
    /// a String row's `Row::builtIn.text` are `const char*` because an engine row's are
    /// string literals; a declared row's point into these three, which is why the deque
    /// holding them may never move what it holds.
    struct Declared {
        std::string key;
        std::string label;
        std::string text;
        Row row;
    };

    /// The one place an id becomes storage, and the one place an id that names no row is
    /// answered. Indexing unchecked is safe only while every `Id` is an enumerator, which
    /// `declare` ended.
    [[nodiscard]] Slot& slot(Id id);
    [[nodiscard]] const Slot& slot(Id id) const;

    /// Whether any row of *this* table -- engine or declared -- lives in `module`. What
    /// separates a misspelled engine key from an orphaned game section when `loadJson`
    /// walks a key nothing claims.
    [[nodiscard]] bool claimsModule(std::string_view module) const;

    /// `declare`'s untyped half, so the template is three lines and the refusals are not
    /// instantiated once per type. `text` is the built-in for a String row and ignored
    /// otherwise.
    Id declareRow(std::string_view key, Type type, Default builtIn, std::string_view text, double minimum,
                  double maximum, uint8_t flags, std::string_view label);

    /// One overload per row type, so the constructor's defaults and the setters share the
    /// single place that knows which of a slot's three fields a type lives in.
    static void store(Slot& s, bool v);
    static void store(Slot& s, int v);
    static void store(Slot& s, uint32_t v);
    static void store(Slot& s, uint64_t v);
    static void store(Slot& s, float v);
    static void store(Slot& s, std::string v);

    /// A fresh slot holding the row's built-in. The same operation for the constructor's
    /// ninety-odd expansions and for `declare`'s one row.
    static void storeDefault(Slot& s, const Row& r);

    /// One row, from one `rapidjson::Value` (again type-erased, for the reason `loadJson`
    /// takes one). A value of the wrong type keeps the default and says so.
    void applyJson(Id id, const void* value, std::string_view origin);

    /// Whether a write of `type` to `id` is allowed at all, and why not when it is not.
    /// Every `setValue` overload asks this first, which is what keeps one refusal policy.
    bool writable(Id id, Type type, Source from);

    /// Record the provenance and push the value into the bound field, if there is one.
    void published(Id id, Source from, std::string_view origin);

    // Deques, not vectors: both hand out references that a reallocation would move. See
    // the class comment.
    std::deque<Slot> slots;
    std::deque<Declared> declared;
    bool initFrozen = false;
    bool schemaFrozen = false;
    mutable bool warnedUnknownId = false;
};

/**
 * @brief Point every renderer-backed row at the renderer's own field, so default, config
 *        file and command line all reach the renderer by one path.
 *
 * **A free function in `engine/gfx/SettingsBind.cpp`, not a method.** `Settings.cpp` is in
 * `SUBSTRATE_HOSTED_SOURCES` -- it pulls in neither Vulkan nor a window, which is what lets
 * `./test.sh tsan` cover it -- and a member touching `gfx::Renderer` would end that.
 */
void bindRenderer(Settings& settings, gfx::Renderer& renderer);

} // namespace settings

/// The typed handles, one per row, in a namespace per module. The constant path mirrors the
/// JSON path exactly: `options::render::msaaSamples` against `render.msaaSamples`.
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
 * The same path again -- `defaults::render::ssrMaxDistance` beside
 * `options::render::ssrMaxDistance`. **A field `bindRenderer` binds must initialise from
 * here**, or its value is written twice and drifts; `render.debugOverlay` was `true` in
 * the table and `false` on the field, caught by nothing because `bindRenderer` runs before
 * the first frame reads it.
 *
 * `auto` rather than the row's `ctype`, the one place this does not mirror `options`:
 * `constexpr std::string` does not exist. So a `String` row's default is the `const char*`
 * the macro wrote and every other row's is its literal's own type, all usable in a constant
 * expression.
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
