#include "core/Settings.h"

#include "core/FileWrite.h"
#include "core/Logger.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace core {

namespace settings {
namespace {

/// The metadata, one row per entry in the list, in id order -- `Id` indexes it directly.
/// Nothing a game declares can reach it: a declared row lives in the table's own deque with
/// an id at or above `Id::Count`.
constexpr Row kRows[] = {
#define SUBSTRATE_SETTING_ROW(mod, name, tag, ctype, def, lo, hi, flags, label)                                         \
    {#mod "." #name, label,          Type::tag,           static_cast<uint8_t>(flags),                                  \
     static_cast<double>(lo), static_cast<double>(hi), def},
    SUBSTRATE_SETTINGS(SUBSTRATE_SETTING_ROW)
#undef SUBSTRATE_SETTING_ROW
};

static_assert(std::size(kRows) == count(), "the row table and the id enum come from one list");

/// Whether `key` is `module.something`. The separator is part of the match, or `render`
/// would claim `renderer.width` and `render.ssr` would claim `render.ssrIntensity`.
bool keyInModule(std::string_view key, std::string_view module) {
    return key.size() > module.size() && key.compare(0, module.size(), module) == 0 && key[module.size()] == '.';
}

/// @brief Whether the engine's own list names this module.
///
/// The refusal is by module, not by key, and has to stay that way: "not a row today" is not
/// "not the engine's", so a key-by-key rule would let a game claim a key a later engine
/// release adds, and take over the value silently.
bool engineOwnsModule(std::string_view module) {
    for (const Row& r : kRows) {
        if (keyInModule(r.key, module)) return true;
    }
    return false;
}

std::string lowered(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// `true|false|on|off|1|0|yes|no`. Anything else is a refusal, never a false -- a typo that
/// quietly read as "off" is the failure this whole table exists to stop.
bool parseBool(std::string_view text, bool& out) {
    const std::string v = lowered(text);
    if (v == "true" || v == "on" || v == "1" || v == "yes") { out = true; return true; }
    if (v == "false" || v == "off" || v == "0" || v == "no") { out = false; return true; }
    return false;
}

bool parseDouble(std::string_view text, double& out) {
    const std::string v(text);
    char* end = nullptr;
    const double parsed = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0') return false;
    out = parsed;
    return true;
}

/// Clamp to the row's stated bounds and say so. Equal bounds mean unbounded, so a byte
/// budget does not get a ceiling invented for it.
double clamped(const Row& r, double value) {
    if (r.minimum == r.maximum) return value;
    const double result = std::min(std::max(value, r.minimum), r.maximum);
    if (result != value) {
        Logger::warn(LogCategory::Core, "%s: %g is outside [%g, %g]; using %g", r.key, value, r.minimum, r.maximum,
                     result);
    }
    return result;
}

} // namespace

const char* typeName(Type t);

std::string defaultString(const Row& r) {
    char buffer[64];
    switch (r.type) {
        case Type::Bool: return r.builtIn.integer != 0 ? "true" : "false";
        case Type::Int: std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(r.builtIn.integer)); return buffer;
        case Type::Uint:
            std::snprintf(buffer, sizeof(buffer), "%u", static_cast<uint32_t>(r.builtIn.integer));
            return buffer;
        case Type::Uint64:
            std::snprintf(buffer, sizeof(buffer), "%" PRIu64, static_cast<uint64_t>(r.builtIn.integer));
            return buffer;
        case Type::Float:
            // Narrowed to `float` and then `%g`, matching `valueString` exactly. The two
            // spellings must agree to the character, or "differs from its default" -- a
            // string comparison of the two -- answers wrongly.
            std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(static_cast<float>(r.builtIn.real)));
            return buffer;
        case Type::String: return r.builtIn.text != nullptr ? r.builtIn.text : "";
    }
    return {};
}

const Row& Settings::row(Id id) const {
    const auto index = static_cast<uint16_t>(id);
    if (index < count()) return kRows[index];
    if (const size_t offset = index - count(); offset < declared.size()) return declared[offset].row;

    // Warned once, not per call: this can be reached from a per-frame read, and a message
    // every frame is one nobody reads.
    if (!warnedUnknownId) {
        warnedUnknownId = true;
        Logger::warn(LogCategory::Core, "a settings handle names no row (%u); reading `%s` instead", index,
                     kRows[0].key);
    }
    return kRows[0];
}

uint16_t Settings::rowCount() const {
    return static_cast<uint16_t>(slots.size());
}

Id Settings::find(std::string_view key) const {
    for (uint16_t i = 0; i < count(); ++i) {
        if (key == kRows[i].key) return static_cast<Id>(i);
    }
    for (size_t i = 0; i < declared.size(); ++i) {
        if (key == declared[i].key) return static_cast<Id>(count() + i);
    }
    return Id::None;
}

bool Settings::claimsModule(std::string_view module) const {
    if (engineOwnsModule(module)) return true;
    for (const Declared& d : declared) {
        if (keyInModule(d.key, module)) return true;
    }
    return false;
}

const char* sourceName(Source s) {
    switch (s) {
        case Source::Default: return "default";
        case Source::Config: return "config";
        case Source::Game: return "game";
        case Source::Cli: return "cli";
    }
    return "default";
}

const char* typeName(Type t) {
    switch (t) {
        case Type::Bool: return "bool";
        case Type::Int: return "int";
        case Type::Uint: return "uint32";
        case Type::Uint64: return "uint64";
        case Type::Float: return "float";
        case Type::String: return "string";
    }
    return "?";
}

const std::vector<RemovedKey>& removedKeys() {
    // Every entry's message has to name the flag that replaced the key or the field that
    // holds it now. "It moved" leaves the reader exactly where a silent removal would.
    static const std::vector<RemovedKey> kRemoved = {
        {"scene.path", "moved into game code -- a scene is authored, not configured; pass a path on the command line "
                       "for a one-off, and see `engine.current_scene_path` in --dump-settings"},
        {"scene.characters", "moved into game code; --characters still works for a one-off"},
        {"lighting.sun", "moved into game code -- authored lighting"},
        {"lighting.autoPlacePointLights", "moved into game code -- authored lighting"},
        {"lighting.pointLights", "moved into game code -- authored lighting"},
        {"window.title", "moved into game code -- the program's name is not a preference"},
        {"render.exposure", "moved into game code -- a look decision, authored with the lighting it balances"},
        {"render.debugView", "a developer control, not a user setting; --debug-view still works"},
        {"physics.clock", "a developer control, not a user setting; --locked and --realtime still work"},
        {"physics.gravity", "moved into game code -- a simulation constant"},
        {"physics.step", "moved into game code -- a simulation constant, and load-bearing for determinism"},
        {"audio.buses", "moved into game code -- a mix graph the game designed"},
        {"audio.sources", "moved into game code -- content placement; scenes declare these through substrate_audio"},
        {"audio.occlusion", "moved into game code -- tuned against one scene's geometry"},
        {"audio.occlusionCutoffHz", "moved into game code -- tuned against one scene's geometry"},
        {"audio.occlusionGain", "moved into game code -- tuned against one scene's geometry"},
        {"audio.occlusionAttack", "moved into game code -- tuned against one scene's geometry"},
        {"audio.occlusionRelease", "moved into game code -- tuned against one scene's geometry"},
        {"audio.occlusionRayMargin", "moved into game code -- tuned against one scene's geometry"},
        {"benchmark.capturePath", "a tool's output path, not a preference; --capture still works"},
        {"benchmark.rdocCapturePath", "a tool's output path, not a preference; --rdoc-capture-path still works"},
        {"decals", "moved into game code -- content placement"},

        // Developer controls: no JSON key, a named flag.
        {"render.validation", "is now --validation auto|on|off; validation layers are a developer control, so it has "
                              "no config key"},
        {"render.syncValidation", "is now --sync-validation, which implies --validation on; it is a developer "
                                  "control, so it has no config key"},
        {"render.rayQuery", "is now --no-ray-query; requesting a device extension is a developer control, so it has "
                            "no config key"},
        {"render.shaderHotReload", "is now --hot-reload auto|on|off; a shader recompile loop is a developer control, "
                                   "so it has no config key"},
        {"render.debugOverlay", "is now --overlay / --no-overlay, and F6 still toggles it; the frame-stats overlay is "
                                "a developer readout, so it has no config key"},
        {"scene.bakeCache", "is now the substrate-bake tool, which build_release.sh runs once per scene; baking "
                            "is a build step, and since D9 a game binary cannot write a sidecar at all"},
        {"physics.enabled", "is now --no-physics; skipping the world is a measurement rather than a preference, so it "
                            "has no config key. audio.enabled stayed a row, because muting is a preference"},
        {"physics.debugDraw", "is now --physics-debug, and B still toggles it; it is a developer control, so it has "
                              "no config key"},
        {"physics.debugContacts", "is now --physics-contacts, which implies --physics-debug; it is a developer "
                                  "control, so it has no config key"},
        {"audio.backend", "is now --audio-null for the test mode; `auto` takes the device if there is one and needs "
                          "no user choice, so it has no config key"},
        {"audio.debugDraw", "is now --audio-debug, and K still toggles it; it is a developer control, so it has no "
                            "config key"},
        {"ui.panel", "is now --panel, and I still toggles it; a debug window is a developer control, so it has no "
                     "config key"},
        {"ui.inspector", "is now --inspector, and O still toggles it; a debug window is a developer control, so it "
                         "has no config key"},
        {"profiler.enabled", "is now --no-profiler; the profiler is a developer's instrument, so it has no config key"},
        {"profiler.averagingWindow", "moved into core::ProfilerConfig, which the engine fills in Config.h -- the "
                                     "profiler's own defaults are the only spelling of them now"},
        {"profiler.maxFrames", "moved into core::ProfilerConfig, which the engine fills in Config.h"},
        {"profiler.outputFile", "is now --trace <path>; where a tool writes is not a preference, so it has no config "
                                "key"},
        {"profiler.autoFlushFrames", "moved into core::ProfilerConfig, which the engine fills in Config.h"},
        {"profiler.clearAfterFlush", "moved into core::ProfilerConfig, which the engine fills in Config.h"},
        {"record.enabled", "is now --record [seconds]; a capture control is not a preference, so it has no config "
                           "key. Engine::startRecording is public, so a game that ships clip capture holds its own "
                           "setting for it"},
        {"record.seconds", "is now the optional number --record takes -- --record 60 keeps the last minute"},
        {"record.fps", "moved into core::Recorder::Options, which the engine fills in Config.h"},
        {"record.file", "is now --record-file <path>; where a tool writes is not a preference, so it has no config "
                        "key"},
        {"logging.file", "is now --log-file <path>; where the log goes is a developer control, so it has no config "
                         "key"},
        {"logging.level", "is now --log-level <name>, which is what a bug report already asks you to type; it has no "
                          "config key"},
        {"logging.output", "is now --log-output terminal|file|both; it is a developer control, so it has no config "
                           "key"},
        {"logging.categories", "is now --log-categories a,b,c; it left with the three logging rows beside it rather "
                               "than being stranded as the only key of a module no row claims"},
        {"benchmark.exitAfterFrames", "is now --frames N, and it always was; every sibling of it was already a flag "
                                      "with no config key"},

        // Authored by the game: a `GameSetup` field, with nothing to type anywhere.
        {"render.tonemap", "moved into game code as GameSetup::look.tonemap -- a look decision authored with the lighting "
                           "it balances, beside GameSetup::look.exposure. --tonemap <name> still overrides it for one run"},
        {"render.shadowDepthBias", "moved into game code as GameSetup::look.shadowDepthBias -- tuned against a scene's "
                                   "geometry, which is game feel rather than taste"},
        {"render.shadowNormalBias", "moved into game code as GameSetup::look.shadowNormalBias -- tuned against a "
                                    "scene's geometry, which is game feel rather than taste"},
        {"render.lightBudget", "gone: the light buffer sizes itself and grows (C40)"},
        {"render.particleBudget", "gone: the particle pool sizes itself and grows (C40)"},
        {"physics.bodyBudget", "gone: the physics world sizes itself and grows (C40)"},
        {"audio.voiceBudget", "gone: the voice list sizes itself and grows (C40)"},

        // Declared by the game: a preference belonging to the game that draws it.
        {"ui.panelX", "belongs to the game that draws the panel; the demo declares demo.panelX, and --set "
                      "demo.panelX=<n> still reaches it"},
        {"ui.panelY", "belongs to the game that draws the panel; the demo declares demo.panelY"},
        {"ui.panelWidth", "belongs to the game that draws the panel; the demo declares demo.panelWidth"},
        {"ui.panelHeight", "belongs to the game that draws the panel; the demo declares demo.panelHeight"},
    };
    return kRemoved;
}

namespace {

/**
 * @brief Keys the file may legitimately carry that are not rows, and who reads each. An
 *        aggregate parsed elsewhere must be listed here or `loadJson` reports it as a typo.
 *
 * An entry whose module no row claims is a hazard: `claimsModule` answers false, so a game
 * may declare into that module while the engine is still parsing a key out of it -- two
 * owners of one JSON section. `input` keeps three rows of its own, so `input.bindings` is
 * safe.
 */
constexpr const char* kParsedElsewhere[] = {
    "input.bindings", // input::applyBindings, via Config::input.bindings
};

/**
 * @brief Say what became of a key nothing claims.
 *
 * @param moduleClaimed whether any row of this table lives in the key's module -- what
 *        separates a typo from an orphan.
 *
 * @return true if the key is an orphan, which the caller counts and reports once per module.
 *         An orphan must not warn: two games sharing a `substrate.json` is a designed-for
 *         state, and every run of the other game would open with warnings nobody can act on.
 *         A typo does warn, because that one is a mistake to correct.
 */
[[nodiscard]] bool reportUnclaimed(const std::string& key, bool moduleClaimed) {
    for (const char* known : kParsedElsewhere) {
        if (key == known) return false;
    }
    for (const RemovedKey& removed : removedKeys()) {
        if (key == removed.key) {
            Logger::error(LogCategory::Core, "`%s` %s", removed.key, removed.message);
            return false;
        }
    }
    if (!moduleClaimed) return true;
    // `error`, not `warn`: a warning is advisory, and a key that silently does nothing is
    // not advisory -- the value somebody wrote is not being set.
    Logger::error(LogCategory::Core, "unknown setting `%s` -- see --dump-settings for every key there is", key.c_str());
    return false;
}

} // namespace

void Settings::loadJson(const void* doc, std::string_view origin) {
    const auto& root = *static_cast<const rapidjson::Value*>(doc);
    if (!root.IsObject()) return;

    // The *file* is walked, not the table: walking the table applies the config just as
    // well and turns a key nobody reads into silence.
    for (auto section = root.MemberBegin(); section != root.MemberEnd(); ++section) {
        if (!section->name.IsString()) continue;
        const std::string module = section->name.GetString();

        // A bare top-level key names no `module.key` at all, so `true`: there is no module
        // to have been claimed, and the orphan sentence would answer a question nobody
        // asked.
        if (!section->value.IsObject()) {
            (void)reportUnclaimed(module, true);
            continue;
        }

        const bool moduleClaimed = claimsModule(module);
        uint32_t orphans = 0;

        for (auto member = section->value.MemberBegin(); member != section->value.MemberEnd(); ++member) {
            if (!member->name.IsString()) continue;
            const std::string key = module + "." + member->name.GetString();

            const Id id = find(key);
            if (id == Id::None) {
                // A row declared *after* this runs is unclaimed here too -- the file is
                // walked once. `Engine::init` declares before reading the file for that
                // reason.
                if (reportUnclaimed(key, moduleClaimed)) ++orphans;
                continue;
            }
            applyJson(id, &member->value, origin);
        }

        // Once per section, not per key. The values are refused but kept: `saveJson` merges
        // into the file it read, so a setting a game drops and later restores does not cost
        // the user their answer, and two games sharing one file do not erase each other.
        if (orphans > 0) {
            Logger::debug(LogCategory::Core,
                          "`%s.` is not a module in this build: %u key%s left in the file untouched -- a section "
                          "another game declares, or one this game no longer does, is not this run's to delete",
                          module.c_str(), orphans, orphans == 1 ? "" : "s");
        }
    }
}

void Settings::applyJson(Id id, const void* value, std::string_view origin) {
    const auto& v = *static_cast<const rapidjson::Value*>(value);
    const Row& r = row(id);

    // The wrong type keeps the default rather than failing the load, so an old config file
    // still loads against a new build. It must still say so: silent, this is
    // indistinguishable from the key not being read at all.
    const auto refuse = [&r]() {
        Logger::error(LogCategory::Core, "%s wants a %s; the value in the file is not one, so the default stands",
                      r.key, typeName(r.type));
    };

    switch (r.type) {
        case Type::Bool:
            if (!v.IsBool()) return refuse();
            (void)setValue(id, v.GetBool(), Source::Config, origin);
            return;
        case Type::Int:
            if (!v.IsInt()) return refuse();
            (void)setValue(id, v.GetInt(), Source::Config, origin);
            return;
        case Type::Uint:
            if (!v.IsUint()) return refuse();
            (void)setValue(id, v.GetUint(), Source::Config, origin);
            return;
        case Type::Uint64:
            if (!v.IsUint64()) return refuse();
            (void)setValue(id, v.GetUint64(), Source::Config, origin);
            return;
        case Type::Float:
            // IsNumber, not IsDouble: rapidjson types `1` as an int, so `"exposure": 1`
            // would be refused as the wrong type.
            if (!v.IsNumber()) return refuse();
            (void)setValue(id, static_cast<float>(v.GetDouble()), Source::Config, origin);
            return;
        case Type::String:
            if (!v.IsString()) return refuse();
            (void)setValue(id, std::string(v.GetString()), Source::Config, origin);
            return;
    }
}

bool Settings::saveJson(const std::string& path) const {
    rapidjson::Document doc;
    doc.SetObject();

    if (std::ifstream in(path); in.is_open()) {
        rapidjson::IStreamWrapper stream(in);
        rapidjson::Document existing;
        existing.ParseStream(stream);
        if (existing.HasParseError()) {
            // Refuse rather than overwrite: a config that fails to parse still holds
            // settings, and replacing it loses every one the parser could not reach.
            Logger::error(LogCategory::Core, "Cannot save settings: %s is not valid JSON (%s at %zu)", path.c_str(),
                          rapidjson::GetParseError_En(existing.GetParseError()), existing.GetErrorOffset());
            return false;
        }
        if (existing.IsObject()) doc.Swap(existing);
    }

    auto& alloc = doc.GetAllocator();

    uint32_t written = 0;
    for (uint16_t i = 0; i < rowCount(); ++i) {
        const auto id = static_cast<Id>(i);
        const Row& r = row(id);
        if ((r.flags & kEngine) != 0) continue;

        const std::string key = r.key;
        const size_t dot = key.find('.');
        const std::string module = key.substr(0, dot);
        const std::string name = key.substr(dot + 1);

        rapidjson::Value::MemberIterator section = doc.FindMember(module.c_str());
        const bool haveSection = section != doc.MemberEnd() && section->value.IsObject();
        const bool present = haveSection && section->value.HasMember(name.c_str());
        if (!present && valueString(id) == defaultString(r)) continue;

        if (!haveSection) {
            doc.RemoveMember(module.c_str());
            doc.AddMember(rapidjson::Value(module.c_str(), static_cast<rapidjson::SizeType>(module.size()), alloc),
                          rapidjson::Value(rapidjson::kObjectType), alloc);
            section = doc.FindMember(module.c_str());
        }
        rapidjson::Value& object = section->value;

        rapidjson::Value value;
        switch (r.type) {
            case Type::Bool: value.SetBool(getBool(id)); break;
            case Type::Int: value.SetInt(getInt(id)); break;
            case Type::Uint: value.SetUint(getUint(id)); break;
            case Type::Uint64: value.SetUint64(getUint64(id)); break;
            case Type::Float: value.SetDouble(static_cast<double>(getFloat(id))); break;
            case Type::String: {
                const std::string& text = getString(id);
                value.SetString(text.c_str(), static_cast<rapidjson::SizeType>(text.size()), alloc);
                break;
            }
        }

        object.RemoveMember(name.c_str());
        object.AddMember(rapidjson::Value(name.c_str(), static_cast<rapidjson::SizeType>(name.size()), alloc), value,
                         alloc);
        ++written;
    }

    rapidjson::StringBuffer text;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(text);
    writer.SetIndent(' ', 2);
    // Without the cap, a float widened to a double writes 0.1 as 0.10000000149011612 --
    // the same number the file said, and unreadable as a diff against it. Six places is
    // more than a float carries.
    writer.SetMaxDecimalPlaces(6);
    doc.Accept(writer);

    if (!writeFileAtomically(path, std::string(text.GetString()) + '\n', LogCategory::Core, "settings")) return false;

    Logger::status(LogCategory::Core, "Saved %u setting%s to %s", written, written == 1 ? "" : "s", path.c_str());
    return true;
}

Settings::Settings() {
    for (const Row& r : kRows) {
        storeDefault(slots.emplace_back(), r);
    }
}

void Settings::store(Slot& s, bool v) { s.integer = v ? 1 : 0; }
void Settings::store(Slot& s, int v) { s.integer = v; }
void Settings::store(Slot& s, uint32_t v) { s.integer = static_cast<int64_t>(v); }
void Settings::store(Slot& s, uint64_t v) { s.integer = static_cast<int64_t>(v); }
void Settings::store(Slot& s, float v) { s.real = static_cast<double>(v); }
void Settings::store(Slot& s, std::string v) { s.text = std::move(v); }

void Settings::storeDefault(Slot& s, const Row& r) {
    switch (r.type) {
        case Type::Bool: store(s, r.builtIn.integer != 0); return;
        case Type::Int: store(s, static_cast<int>(r.builtIn.integer)); return;
        case Type::Uint: store(s, static_cast<uint32_t>(r.builtIn.integer)); return;
        case Type::Uint64: store(s, static_cast<uint64_t>(r.builtIn.integer)); return;
        case Type::Float: store(s, static_cast<float>(r.builtIn.real)); return;
        case Type::String: store(s, std::string(r.builtIn.text != nullptr ? r.builtIn.text : "")); return;
    }
}

Id Settings::declareRow(std::string_view key, Type type, Default builtIn, std::string_view text, double minimum,
                        double maximum, uint8_t flags, std::string_view label) {
    const std::string name(key);

    if (schemaFrozen) {
        Logger::error(LogCategory::Core,
                      "`%s` cannot be declared: the settings schema is frozen. A row is declared from "
                      "`Game::declareSettings`, which runs before the config file is read; `configure` is where a "
                      "game *writes* a row it has already declared",
                      name.c_str());
        return Id::None;
    }
    // `module.name`, both halves non-empty and exactly one dot -- the shape every consumer
    // assumes when it splits a key to find its JSON section.
    const size_t dot = name.find('.');
    if (dot == 0 || dot == std::string::npos || dot + 1 == name.size() || name.find('.', dot + 1) != std::string::npos) {
        Logger::error(LogCategory::Core, "`%s` is not a settings key: a key is <module>.<name>", name.c_str());
        return Id::None;
    }
    // Before the duplicate check below, so `render.ssao` is refused for owning the module
    // rather than for the accident that a row happens to exist today.
    if (const std::string_view module = std::string_view(name).substr(0, dot); engineOwnsModule(module)) {
        Logger::error(LogCategory::Core,
                      "`%s` cannot be declared: `%.*s` is an engine module. A game owns every module the engine does "
                      "not name -- `%.*s.%s` would be the engine's key, and an engine release adding it would take "
                      "over a value the user wrote for the game",
                      name.c_str(), static_cast<int>(module.size()), module.data(), static_cast<int>(module.size()),
                      module.data(), name.c_str() + dot + 1);
        return Id::None;
    }
    // A key that *used to be* an engine setting gets through the module test whenever the
    // engine no longer names that module -- `lighting.sun` is the shape. Without this, every
    // config file still carrying one starts feeding a row it was never written for.
    for (const RemovedKey& removed : removedKeys()) {
        if (name == removed.key) {
            Logger::error(LogCategory::Core,
                          "`%s` cannot be declared: it used to be an engine setting, and a file still carrying it "
                          "would feed a row it was never written for (%s)",
                          name.c_str(), removed.message);
            return Id::None;
        }
    }
    // A game declaring `kEngine` would get a row no door can write and no save can keep.
    if ((flags & kEngine) != 0) {
        Logger::error(LogCategory::Core,
                      "`%s` cannot be declared as engine-owned: that flag marks live state the engine reports, and a "
                      "row a game cannot write through any door is a row nothing applies",
                      name.c_str());
        return Id::None;
    }
    if (find(name) != Id::None) {
        Logger::error(LogCategory::Core, "`%s` is already a setting; declaring it again would shadow it", name.c_str());
        return Id::None;
    }

    Declared& d = declared.emplace_back();
    d.key = name;
    d.label = label;
    d.text = text;
    d.row = Row{d.key.c_str(),
                d.label.c_str(),
                type,
                flags,
                minimum,
                maximum,
                builtIn};
    if (type == Type::String) d.row.builtIn.text = d.text.c_str();

    storeDefault(slots.emplace_back(), d.row);
    return static_cast<Id>(count() + declared.size() - 1);
}

Settings::Slot& Settings::slot(Id id) {
    const auto index = static_cast<uint16_t>(id);
    if (index < slots.size()) return slots[index];
    (void)row(id); // for its once-only warning
    return slots[0];
}

const Settings::Slot& Settings::slot(Id id) const {
    const auto index = static_cast<uint16_t>(id);
    if (index < slots.size()) return slots[index];
    (void)row(id);
    return slots[0];
}

bool Settings::getBool(Id id) const {
    const Slot& s = slot(id);
    return s.live != nullptr ? *static_cast<const bool*>(s.live) : s.integer != 0;
}

int Settings::getInt(Id id) const {
    const Slot& s = slot(id);
    return s.live != nullptr ? *static_cast<const int*>(s.live) : static_cast<int>(s.integer);
}

uint32_t Settings::getUint(Id id) const {
    const Slot& s = slot(id);
    return s.live != nullptr ? *static_cast<const uint32_t*>(s.live) : static_cast<uint32_t>(s.integer);
}

uint64_t Settings::getUint64(Id id) const {
    const Slot& s = slot(id);
    return s.live != nullptr ? *static_cast<const uint64_t*>(s.live) : static_cast<uint64_t>(s.integer);
}

float Settings::getFloat(Id id) const {
    const Slot& s = slot(id);
    return s.live != nullptr ? *static_cast<const float*>(s.live) : static_cast<float>(s.real);
}

const std::string& Settings::getString(Id id) const {
    const Slot& s = slot(id);
    return s.live != nullptr ? *static_cast<const std::string*>(s.live) : s.text;
}

Source Settings::source(Id id) const { return slot(id).src; }

const std::string& Settings::origin(Id id) const { return slot(id).from; }

std::string Settings::valueString(Id id) const {
    char buffer[64];
    switch (row(id).type) {
        case Type::Bool: return getBool(id) ? "true" : "false";
        case Type::Int: std::snprintf(buffer, sizeof(buffer), "%d", getInt(id)); return buffer;
        case Type::Uint: std::snprintf(buffer, sizeof(buffer), "%u", getUint(id)); return buffer;
        case Type::Uint64:
            std::snprintf(buffer, sizeof(buffer), "%" PRIu64, getUint64(id));
            return buffer;
        case Type::Float:
            // %g, matching `defaultString` to the character -- "differs from its default"
            // is a string comparison of the two. A fixed precision would also print 60 as
            // 60.000000 and stop the dump diffing against the file it came from.
            std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(getFloat(id)));
            return buffer;
        case Type::String: return getString(id);
    }
    return {};
}

bool Settings::writable(Id id, Type type, Source from) {
    // First, because every question below is asked of a row. A read through an unknown
    // handle answers the first row, but a *write* through one has to change nothing -- a
    // refused `declare` would otherwise hand back a handle that quietly assigns
    // `window.width`.
    if (static_cast<uint16_t>(id) >= rowCount()) {
        Logger::error(LogCategory::Core, "a settings handle names no row; refusing the write");
        return false;
    }

    const Row& r = row(id);
    if (r.type != type) {
        Logger::error(LogCategory::Core, "%s is a %s; refusing a %s", r.key, typeName(r.type), typeName(type));
        return false;
    }
    // `setEngineOwned` is the only writer of an `engine.` row, and does not come through
    // here: assigning a scene path is not loading a scene.
    if ((r.flags & kEngine) != 0) {
        Logger::warn(LogCategory::Core, "%s is engine-owned and cannot be set; it reports state rather than taking it",
                     r.key);
        return false;
    }
    if ((r.flags & kInitOnly) != 0 && initFrozen) {
        // A stated refusal, never a silent clamp: what these rows size is already sized, so
        // a later write reaches nothing and a quiet one leaves no way to find that out.
        Logger::warn(LogCategory::Core, "%s is applied at startup and cannot change afterwards; ignoring", r.key);
        return false;
    }
    (void)from;
    return true;
}

void Settings::published(Id id, Source from, std::string_view origin) {
    Slot& s = slot(id);
    s.src = from;
    // Through a temporary: `bindLive` passes the slot's own `from` back in, and assigning a
    // string from a view of itself is a self-overlapping copy.
    s.from = std::string(origin);

    if (s.live == nullptr) return;
    switch (row(id).type) {
        case Type::Bool: *static_cast<bool*>(s.live) = s.integer != 0; break;
        case Type::Int: *static_cast<int*>(s.live) = static_cast<int>(s.integer); break;
        case Type::Uint: *static_cast<uint32_t*>(s.live) = static_cast<uint32_t>(s.integer); break;
        case Type::Uint64: *static_cast<uint64_t*>(s.live) = static_cast<uint64_t>(s.integer); break;
        case Type::Float: *static_cast<float*>(s.live) = static_cast<float>(s.real); break;
        case Type::String: *static_cast<std::string*>(s.live) = s.text; break;
    }
}

bool Settings::setValue(Id id, bool value, Source from, std::string_view origin) {
    if (!writable(id, Type::Bool, from)) return false;
    store(slot(id), value);
    published(id, from, origin);
    return true;
}

bool Settings::setValue(Id id, int value, Source from, std::string_view origin) {
    if (!writable(id, Type::Int, from)) return false;
    store(slot(id), static_cast<int>(clamped(row(id), value)));
    published(id, from, origin);
    return true;
}

bool Settings::setValue(Id id, uint32_t value, Source from, std::string_view origin) {
    if (!writable(id, Type::Uint, from)) return false;
    store(slot(id), static_cast<uint32_t>(clamped(row(id), value)));
    published(id, from, origin);
    return true;
}

bool Settings::setValue(Id id, uint64_t value, Source from, std::string_view origin) {
    if (!writable(id, Type::Uint64, from)) return false;
    store(slot(id), value);
    published(id, from, origin);
    return true;
}

bool Settings::setValue(Id id, float value, Source from, std::string_view origin) {
    if (!writable(id, Type::Float, from)) return false;
    store(slot(id), static_cast<float>(clamped(row(id), value)));
    published(id, from, origin);
    return true;
}

bool Settings::setValue(Id id, std::string value, Source from, std::string_view origin) {
    if (!writable(id, Type::String, from)) return false;
    store(slot(id), std::move(value));
    published(id, from, origin);
    return true;
}

void Settings::setEngineOwned(Id id, std::string_view value) {
    const Row& r = row(id);
    if ((r.flags & kEngine) == 0 || r.type != Type::String) {
        Logger::error(LogCategory::Core, "%s is not an engine-owned string", r.key);
        return;
    }
    store(slot(id), std::string(value));
    published(id, Source::Game, "");
}

bool Settings::setFromString(std::string_view key, std::string_view value, Source from, std::string_view origin) {
    const Id id = find(key);
    if (id == Id::None) {
        const std::string name(key);
        for (const RemovedKey& removed : removedKeys()) {
            if (name == removed.key) {
                Logger::warn(LogCategory::Core, "`%s` %s", removed.key, removed.message);
                return false;
            }
        }
        Logger::warn(LogCategory::Core, "unknown setting `%s`", name.c_str());
        return false;
    }

    switch (row(id).type) {
        case Type::Bool: {
            bool parsed = false;
            if (!parseBool(value, parsed)) break;
            return setValue(id, parsed, from, origin);
        }
        case Type::String: return setValue(id, std::string(value), from, origin);
        default: {
            double parsed = 0.0;
            if (!parseDouble(value, parsed)) break;
            switch (row(id).type) {
                case Type::Int: return setValue(id, static_cast<int>(parsed), from, origin);
                case Type::Uint: return setValue(id, static_cast<uint32_t>(parsed), from, origin);
                case Type::Uint64: return setValue(id, static_cast<uint64_t>(parsed), from, origin);
                default: return setValue(id, static_cast<float>(parsed), from, origin);
            }
        }
    }

    const std::string text(value);
    Logger::warn(LogCategory::Core, "%s wants a %s; `%s` is not one", row(id).key, typeName(row(id).type),
                 text.c_str());
    return false;
}

void Settings::bindLive(Id id, void* address) {
    // Refused rather than clamped, unlike a read: binding the first row would send every
    // later write to `window.width` through an address the caller never meant.
    if (static_cast<uint16_t>(id) >= rowCount()) {
        Logger::error(LogCategory::Core, "a settings handle names no row; refusing to bind it");
        return;
    }
    Slot& s = slot(id);
    s.live = address;
    // Pushes what the table already holds, so binding *applies* the config rather than
    // merely remembering where it should have gone.
    published(id, s.src, s.from);
}

void Settings::dumpTable(std::FILE* out) const {
    size_t keyWidth = 0;
    size_t valueWidth = 0;
    for (uint16_t i = 0; i < rowCount(); ++i) {
        keyWidth = std::max(keyWidth, std::strlen(row(static_cast<Id>(i)).key));
        valueWidth = std::max(valueWidth, valueString(static_cast<Id>(i)).size());
    }

    for (uint16_t i = 0; i < rowCount(); ++i) {
        const auto id = static_cast<Id>(i);
        const Row& r = row(id);
        std::fprintf(out, "%-*s %-7s %-*s %-8s %s\n", static_cast<int>(keyWidth), r.key, typeName(r.type),
                     static_cast<int>(valueWidth), valueString(id).c_str(), sourceName(source(id)),
                     origin(id).c_str());
    }
}

void Settings::dumpJson(std::FILE* out) const {
    const auto quoted = [out](const std::string& s) {
        std::fputc('"', out);
        for (const char c : s) {
            switch (c) {
                case '"': std::fputs("\\\"", out); break;
                case '\\': std::fputs("\\\\", out); break;
                case '\n': std::fputs("\\n", out); break;
                default: std::fputc(c, out); break;
            }
        }
        std::fputc('"', out);
    };

    std::fputs("{\n", out);
    for (uint16_t i = 0; i < rowCount(); ++i) {
        const auto id = static_cast<Id>(i);
        const Row& r = row(id);
        std::fputs("  ", out);
        quoted(r.key);
        std::fputs(": {\"value\": ", out);
        // A bool and a number stay unquoted, so a diff of two dumps is a diff of values
        // rather than of their spellings.
        if (r.type == Type::String) {
            quoted(valueString(id));
        } else {
            std::fputs(valueString(id).c_str(), out);
        }
        std::fputs(", \"type\": ", out);
        quoted(typeName(r.type));
        std::fputs(", \"source\": ", out);
        quoted(sourceName(source(id)));
        std::fputs(", \"origin\": ", out);
        quoted(origin(id));
        std::fputs(i + 1 < rowCount() ? "},\n" : "}\n", out);
    }
    std::fputs("}\n", out);
}

} // namespace settings

} // namespace core
