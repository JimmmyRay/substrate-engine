#include "core/Settings.h"

#include <gtest/gtest.h>

#include <rapidjson/document.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace core;

/**
 * @file tests/SettingsTests.cpp
 * @brief The settings table: one list, two doors, and a value that knows where it came
 *        from.
 *
 * The contract worth defending is that the table is **enumerable**, not merely addressable
 * by string. A setter alone would let you write any key and read none, and the failure the
 * whole arc exists to prevent -- a config key that parses and silently does nothing -- is
 * only visible from a dump that walks every row.
 */

namespace {

/// `dumpTable` and `dumpJson` write to a `FILE*`, because their consumer is a terminal and
/// a bug report. Reading one back means giving them a real one.
std::string captured(void (settings::Settings::*dump)(std::FILE*) const, const settings::Settings& s) {
    std::FILE* f = std::tmpfile();
    EXPECT_NE(f, nullptr);
    (s.*dump)(f);
    std::fflush(f);
    std::fseek(f, 0, SEEK_SET);

    std::string text;
    char buffer[4096];
    while (const size_t n = std::fread(buffer, 1, sizeof(buffer), f)) text.append(buffer, n);
    std::fclose(f);
    return text;
}

} // namespace

// ================================================================== the list itself

TEST(SettingsTest, EveryRowIsFoundByItsOwnKey) {
    // The id enum and the row table come from one X-macro list, and a static_assert in
    // Settings.cpp already pins their lengths together. What that cannot check is that the
    // key strings are distinct -- two rows spelled alike would make `find` return the
    // first forever and the second unreachable from JSON.
    const settings::Settings s;
    for (uint16_t i = 0; i < s.rowCount(); ++i) {
        const auto id = static_cast<settings::Id>(i);
        const settings::Row& r = s.row(id);
        ASSERT_NE(r.key, nullptr);
        EXPECT_EQ(s.find(r.key), id) << r.key << " is claimed by an earlier row";
        EXPECT_NE(r.label, nullptr) << r.key;
    }
}

TEST(SettingsTest, AnUnknownKeyIsNotFound) {
    const settings::Settings s;
    EXPECT_EQ(s.find("render.nonsense"), settings::Id::None);
    EXPECT_EQ(s.find(""), settings::Id::None);
}

TEST(SettingsTest, MirroredKeysCarryTheJsonSpellingAndEngineKeysAreSnakeCase) {
    // S2's rule, and the casing difference is the signal about which kind of key you are
    // looking at: the second half of a mirrored key is *forced* to be the JSON key's own
    // spelling, so an `engine.` key -- which has no JSON key to mirror -- is spelled the
    // other way rather than pretending to a file section that does not exist.
    const settings::Settings s;
    EXPECT_STREQ(s.row(settings::Id::render_msaaSamples).key, "render.msaaSamples");

    const settings::Row& engineRow = s.row(settings::Id::engine_current_scene_path);
    EXPECT_STREQ(engineRow.key, "engine.current_scene_path");
    EXPECT_NE(engineRow.flags & settings::kEngine, 0u);
}

TEST(SettingsTest, NoEngineKeyCollidesWithAJsonSection) {
    // `engine.` cannot collide with a JSON key because no top-level `engine` section
    // exists or will -- which is only true while no row spells one.
    const settings::Settings s;
    for (uint16_t i = 0; i < s.rowCount(); ++i) {
        const settings::Row& r = s.row(static_cast<settings::Id>(i));
        const bool engineOwned = (r.flags & settings::kEngine) != 0;
        const bool enginePrefixed = std::string_view(r.key).substr(0, 7) == "engine.";
        EXPECT_EQ(engineOwned, enginePrefixed) << r.key;
    }
}

// ======================================================================= the doors

TEST(SettingsTest, ADefaultReadsAsDefaultUntilSomethingSetsIt) {
    const settings::Settings s;
    EXPECT_EQ(s.get(options::render::msaaSamples), 4u);
    EXPECT_EQ(s.source(settings::Id::render_msaaSamples), settings::Source::Default);
    EXPECT_TRUE(s.origin(settings::Id::render_msaaSamples).empty());
}

TEST(SettingsTest, TheStringDoorAndTheTypedDoorReachOneValue) {
    settings::Settings s;
    ASSERT_TRUE(s.setFromString("render.msaaSamples", "8", settings::Source::Config, "substrate.json"));

    EXPECT_EQ(s.get(options::render::msaaSamples), 8u);
    EXPECT_EQ(s.source(settings::Id::render_msaaSamples), settings::Source::Config);
    EXPECT_EQ(s.origin(settings::Id::render_msaaSamples), "substrate.json");

    ASSERT_TRUE(s.set(options::render::msaaSamples, 2u, settings::Source::Cli));
    EXPECT_EQ(s.valueString(settings::Id::render_msaaSamples), "2");
    EXPECT_EQ(s.source(settings::Id::render_msaaSamples), settings::Source::Cli);
}

TEST(SettingsTest, EveryTypeParsesFromAString) {
    settings::Settings s;

    EXPECT_TRUE(s.setFromString("render.ssao", "off", settings::Source::Config));
    EXPECT_FALSE(s.get(options::render::ssao));
    EXPECT_TRUE(s.setFromString("render.ssao", "true", settings::Source::Config));
    EXPECT_TRUE(s.get(options::render::ssao));

    EXPECT_TRUE(s.setFromString("window.width", "1280", settings::Source::Config));
    EXPECT_EQ(s.get(options::window::width), 1280);

    EXPECT_TRUE(s.setFromString("audio.decodeBudgetBytes", "134217728", settings::Source::Config));
    EXPECT_EQ(s.get(options::audio::decodeBudgetBytes), 134217728ull);

    EXPECT_TRUE(s.setFromString("camera.fovDegrees", "75.5", settings::Source::Config));
    EXPECT_FLOAT_EQ(s.get(options::camera::fovDegrees), 75.5f);

    EXPECT_TRUE(s.setFromString("render.debugFont", "res:/mono.ttf", settings::Source::Config));
    EXPECT_EQ(s.get(options::render::debugFont), "res:/mono.ttf");
}

TEST(SettingsTest, AValueThatDoesNotParseIsRefusedRatherThanReadAsZero) {
    // A typo that quietly read as `false` or as `0` is the failure this table exists to
    // stop, so a bad value keeps the old one and says so.
    settings::Settings s;
    EXPECT_FALSE(s.setFromString("render.ssao", "yes-please", settings::Source::Config));
    EXPECT_TRUE(s.get(options::render::ssao)) << "a refused write leaves the value alone";

    EXPECT_FALSE(s.setFromString("camera.fovDegrees", "wide", settings::Source::Config));
    EXPECT_FLOAT_EQ(s.get(options::camera::fovDegrees), 60.0f);
}

TEST(SettingsTest, AnUnknownKeyIsRefusedRatherThanCreated) {
    settings::Settings s;
    EXPECT_FALSE(s.setFromString("render.nonsense", "1", settings::Source::Config));
}

TEST(SettingsTest, AMovedKeyIsRefusedWithSomewhereToLook) {
    // S4's obligation: somebody's substrate.json has "scene" in it today, and the
    // alternative to a message is a user whose scene setting stopped working with no
    // message at all.
    settings::Settings s;
    EXPECT_FALSE(s.setFromString("scene.path", "res:/showcase.gltf", settings::Source::Config));
    EXPECT_FALSE(s.setFromString("render.exposure", "1.4", settings::Source::Config));

    bool sceneNamed = false;
    for (const settings::RemovedKey& removed : settings::removedKeys()) {
        EXPECT_EQ(s.find(removed.key), settings::Id::None)
            << removed.key << " is both a live row and a removed one";
        ASSERT_NE(removed.message, nullptr);
        if (std::string_view(removed.key) == "scene.path") sceneNamed = true;
    }
    EXPECT_TRUE(sceneNamed);
}

TEST(SettingsTest, ValuesAreClampedToTheirStatedRange) {
    settings::Settings s;
    EXPECT_TRUE(s.setFromString("render.msaaSamples", "64", settings::Source::Config));
    EXPECT_EQ(s.get(options::render::msaaSamples), 8u) << "clamped to the row's maximum";

    EXPECT_TRUE(s.setFromString("audio.masterVolume", "-1", settings::Source::Config));
    EXPECT_FLOAT_EQ(s.get(options::audio::masterVolume), 0.0f);

    // Equal bounds mean unbounded, which is how a byte budget avoids inventing a ceiling.
    EXPECT_TRUE(s.setFromString("audio.decodeBudgetBytes", "999999999999", settings::Source::Config));
    EXPECT_EQ(s.get(options::audio::decodeBudgetBytes), 999999999999ull);
}

// ==================================================================== precedence

/**
 * `Default < Config < Game < Cli`, and every adjacent pair is checked on its own. A test
 * that proved only "the command line beats the built-in" would pass for an implementation
 * with the middle two backwards, which is exactly the bug D15 found: `Game::configure` ran
 * *after* the flags, so a game's `set` beat the command line while every document said it
 * lost to it.
 */
TEST(SettingsTest, EachAdjacentPairOfSourcesResolvesTheWayItIsDocumented) {
    { // Default < Config -- the user's file over the built-in.
        settings::Settings s;
        ASSERT_TRUE(s.setFromString("render.msaaSamples", "8", settings::Source::Config, "substrate.json"));
        EXPECT_EQ(s.get(options::render::msaaSamples), 8u);
        EXPECT_EQ(s.source(settings::Id::render_msaaSamples), settings::Source::Config);
    }
    { // Config < Game -- but only for `set`, the door that means "regardless".
        settings::Settings s;
        ASSERT_TRUE(s.setFromString("render.msaaSamples", "8", settings::Source::Config, "substrate.json"));
        ASSERT_TRUE(s.set(options::render::msaaSamples, 2u));
        EXPECT_EQ(s.get(options::render::msaaSamples), 2u);
        EXPECT_EQ(s.source(settings::Id::render_msaaSamples), settings::Source::Game);
    }
    { // Game < Cli -- the flag beats a game that forced a value.
        settings::Settings s;
        ASSERT_TRUE(s.set(options::render::msaaSamples, 2u));
        ASSERT_TRUE(s.setFromString("render.msaaSamples", "1", settings::Source::Cli, "--set"));
        EXPECT_EQ(s.get(options::render::msaaSamples), 1u);
        EXPECT_EQ(s.source(settings::Id::render_msaaSamples), settings::Source::Cli);
        EXPECT_EQ(s.origin(settings::Id::render_msaaSamples), "--set");
    }
    { // And all four on one row, arriving in the order `Engine::init` opens the doors.
        settings::Settings s;
        EXPECT_EQ(s.source(settings::Id::render_msaaSamples), settings::Source::Default);
        ASSERT_TRUE(s.setFromString("render.msaaSamples", "8", settings::Source::Config, "substrate.json"));
        EXPECT_FALSE(s.setDefault(options::render::msaaSamples, 4u)) << "a game default loses to the file";
        ASSERT_TRUE(s.set(options::render::msaaSamples, 2u)) << "a game override does not";
        ASSERT_TRUE(s.setFromString("render.msaaSamples", "1", settings::Source::Cli, "--set"));
        EXPECT_EQ(s.get(options::render::msaaSamples), 1u) << "the command line is last and wins";
        EXPECT_EQ(s.source(settings::Id::render_msaaSamples), settings::Source::Cli);
    }
}

TEST(SettingsTest, AGameDefaultWritesOverTheBuiltInAndNothingElse) {
    // `setDefault` is the door a game author reaches for, and the whole of it is "has
    // anything claimed this row". Against `Source::Default` specifically, not "anything
    // below `Game`": D11 inserts a `Scene` tier there, and a scene-derived value quietly
    // overwritten by a game default is what both cards are trying to prevent.
    {
        settings::Settings s;
        EXPECT_TRUE(s.setDefault(options::window::vsync, true)) << "nothing had claimed it";
        EXPECT_TRUE(s.get(options::window::vsync));
        EXPECT_EQ(s.source(settings::Id::window_vsync), settings::Source::Game)
            << "and the dump names the game either way, so a user is told who won";
    }
    {
        settings::Settings s;
        ASSERT_TRUE(s.setFromString("window.vsync", "false", settings::Source::Config, "substrate.json"));
        EXPECT_FALSE(s.setDefault(options::window::vsync, true));
        EXPECT_FALSE(s.get(options::window::vsync)) << "the user's file stands";
        EXPECT_EQ(s.source(settings::Id::window_vsync), settings::Source::Config);
        EXPECT_EQ(s.origin(settings::Id::window_vsync), "substrate.json") << "and keeps its origin";
    }
    {
        // The order this one arrives in cannot happen at startup -- the flags are applied
        // after `configure` -- but the panel and the console write as `Game` at runtime,
        // so the predicate has to hold for a row a flag already claimed.
        settings::Settings s;
        ASSERT_TRUE(s.setFromString("window.vsync", "false", settings::Source::Cli, "--set"));
        EXPECT_FALSE(s.setDefault(options::window::vsync, true));
        EXPECT_FALSE(s.get(options::window::vsync));
        EXPECT_EQ(s.source(settings::Id::window_vsync), settings::Source::Cli);
    }
    { // A string row goes through the same door, since the handle carries the type.
        //
        // `render.debugFont` rather than `render.tonemap`, which is where this was written:
        // D14 moved the curve into `GameSetup` and the font is the one `String` row the
        // engine still owns -- the UI's typeface being a property of whoever is reading it.
        settings::Settings s;
        EXPECT_TRUE(s.setDefault(options::render::debugFont, std::string("res:/mono.ttf")));
        EXPECT_EQ(s.get(options::render::debugFont), "res:/mono.ttf");
        EXPECT_FALSE(s.setDefault(options::render::debugFont, std::string("res:/serif.ttf")));
        EXPECT_EQ(s.get(options::render::debugFont), "res:/mono.ttf") << "a second default does not stack";
    }
}

TEST(SettingsTest, AGameDefaultReachesAnInitOnlyRowAndTheUsersFileStillBeatsItThere) {
    // Freezing and precedence are different questions, and a change to one has to leave
    // the other alone. `Game::configure` runs long before `freezeInitOnly()`, so a game
    // default still sizes what an `initOnly` row sizes.
    // `audio.sampleRate` rather than `render.lightBudget`, which is where this was
    // written: D14 made the four budgets `GameSetup` fields, on the argument that how many
    // lights a scene holds is the author's sizing rather than the player's. The property
    // under test is the row's `initOnly` flag and not which subsystem it sizes.
    {
        settings::Settings s;
        EXPECT_TRUE(s.setDefault(options::audio::sampleRate, 96000u));
        EXPECT_EQ(s.get(options::audio::sampleRate), 96000u);
    }
    {
        settings::Settings s;
        ASSERT_TRUE(s.setFromString("audio.sampleRate", "44100", settings::Source::Config, "substrate.json"));
        EXPECT_FALSE(s.setDefault(options::audio::sampleRate, 96000u));
        EXPECT_EQ(s.get(options::audio::sampleRate), 44100u) << "the user's file wins here too";
    }
    {
        // After the freeze it is refused for the other reason, and that refusal is the
        // one that logs: nothing has claimed the row, so the answer is "too late" rather
        // than "somebody else already said".
        settings::Settings s;
        s.freezeInitOnly();
        EXPECT_FALSE(s.setDefault(options::audio::sampleRate, 96000u));
        EXPECT_EQ(s.get(options::audio::sampleRate), 48000u) << "the built-in stands";
        EXPECT_EQ(s.source(settings::Id::audio_sampleRate), settings::Source::Default);
    }
}

// ============================================================== engine-owned rows

TEST(SettingsTest, AnEngineRowIsReadableAndDumpableAndNotSettableByName) {
    // The open question the roadmap answered: a console that could `set
    // engine.current_scene_path` would be a scene-switcher wearing a setting's clothes.
    settings::Settings s;
    EXPECT_FALSE(s.setFromString("engine.current_scene_path", "res:/other.gltf", settings::Source::Cli));
    EXPECT_TRUE(s.get(options::engine::current_scene_path).empty());

    s.setEngineOwned(settings::Id::engine_current_scene_path, "res:/showcase.gltf");
    EXPECT_EQ(s.get(options::engine::current_scene_path), "res:/showcase.gltf");
}

// ================================================================ init-only rows

TEST(SettingsTest, AnInitOnlyRowIsRefusedWithAReasonOnceWhatItSizedIsSized) {
    // This replaces a silent clamp. The mixer's device is opened at the rate the row
    // names, so a rate changed afterwards would describe a stream nothing is producing --
    // and the code that clamped it quietly gave nobody a way to find that out.
    settings::Settings s;
    ASSERT_TRUE(s.set(options::audio::sampleRate, 96000u));
    EXPECT_EQ(s.get(options::audio::sampleRate), 96000u);

    s.freezeInitOnly();
    EXPECT_TRUE(s.initOnlyFrozen());
    EXPECT_FALSE(s.set(options::audio::sampleRate, 44100u));
    EXPECT_EQ(s.get(options::audio::sampleRate), 96000u);

    // An ordinary row is unaffected -- freezing is per-row policy, not a lock on the table.
    EXPECT_TRUE(s.set(options::render::ssao, false));
}

// ================================================================== live binding

TEST(SettingsTest, BindingPublishesTheCurrentValueAndKeepsFollowingIt) {
    // What deletes the 34 `renderer.x = config.render.x` assignments: after a bind, the
    // table's value *is* the field, so there is no second copy to drift.
    settings::Settings s;
    ASSERT_TRUE(s.setFromString("render.ssao", "false", settings::Source::Config, "substrate.json"));

    bool live = true;
    s.bindLive(settings::Id::render_ssao, &live);
    EXPECT_FALSE(live) << "binding applies what the table already held";
    EXPECT_EQ(s.source(settings::Id::render_ssao), settings::Source::Config) << "and does not forget where it came from";

    ASSERT_TRUE(s.set(options::render::ssao, true));
    EXPECT_TRUE(live);

    // A keypress writing the field directly is what the dump must report, which is why
    // `get` reads through the binding rather than from the slot beside it.
    live = false;
    EXPECT_FALSE(s.get(options::render::ssao));
    EXPECT_EQ(s.valueString(settings::Id::render_ssao), "false");
}

// ========================================================================== dump

TEST(SettingsTest, TheTableDumpNamesEveryRowWithItsTypeValueAndProvenance) {
    settings::Settings s;
    ASSERT_TRUE(s.setFromString("render.msaaSamples", "8", settings::Source::Config, "substrate.json"));
    ASSERT_TRUE(s.set(options::input::gamepadDeadzone, 0.25f, settings::Source::Cli));

    const std::string text = captured(&settings::Settings::dumpTable, s);

    // One line per row: the table has to be enumerable, and this is the property that
    // makes it so.
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), s.rowCount());
    EXPECT_NE(text.find("render.msaaSamples"), std::string::npos);
    EXPECT_NE(text.find("substrate.json"), std::string::npos);
    EXPECT_NE(text.find("cli"), std::string::npos);
    EXPECT_NE(text.find("default"), std::string::npos);
}

TEST(SettingsTest, TheJsonDumpIsOneObjectPerSettingAndKeepsNumbersAsNumbers) {
    // The consumer is a bug report, and a diff of two dumps is how "works on my machine"
    // gets resolved -- so a value has to diff as a value rather than as its spelling.
    settings::Settings s;
    ASSERT_TRUE(s.setFromString("render.msaaSamples", "8", settings::Source::Config, "substrate.json"));

    const std::string text = captured(&settings::Settings::dumpJson, s);
    EXPECT_EQ(text.front(), '{');
    EXPECT_NE(text.find("\"render.msaaSamples\": {\"value\": 8,"), std::string::npos);
    EXPECT_NE(text.find("\"source\": \"config\""), std::string::npos);
    EXPECT_NE(text.find("\"origin\": \"substrate.json\""), std::string::npos);
    EXPECT_NE(text.find("\"render.debugFont\": {\"value\": \"\""), std::string::npos)
        << "a string value stays quoted";
}

// ========================================================================== save

namespace {

/// A path nothing else in the suite will collide with, removed by the caller.
std::filesystem::path scratchConfig() {
    return std::filesystem::temp_directory_path() / ("substrate-settings-" + std::to_string(::rand()) + ".json");
}

void writeText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::trunc);
    out << text;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// What a fresh table makes of a file, which is the only question a save has to answer.
/// By reference because `Settings` is deliberately not copyable -- two tables holding one
/// set of live pointers would be two writers of one field.
void reload(settings::Settings& out, const std::filesystem::path& path) {
    rapidjson::Document doc;
    const std::string text = readText(path);
    doc.Parse(text.c_str());
    EXPECT_FALSE(doc.HasParseError()) << text;
    out.loadJson(&doc, path.string());
}

} // namespace

TEST(SettingsTest, ASavedValueIsWhatTheNextRunLoads) {
    const std::filesystem::path path = scratchConfig();

    settings::Settings s;
    ASSERT_TRUE(s.set(options::render::msaaSamples, 8u));
    ASSERT_TRUE(s.set(options::render::ssao, false));
    ASSERT_TRUE(s.set(options::render::bloomThreshold, 2.5f));
    ASSERT_TRUE(s.set(options::render::debugFont, std::string("res:/mono.ttf")));
    ASSERT_TRUE(s.saveJson(path.string()));

    // The round trip is the whole contract: a panel that could write a value the loader
    // then read differently would be worse than no panel, because the disagreement only
    // shows up on the next launch.
    settings::Settings back;
    reload(back, path);
    EXPECT_EQ(back.get(options::render::msaaSamples), 8u);
    EXPECT_FALSE(back.get(options::render::ssao));
    EXPECT_FLOAT_EQ(back.get(options::render::bloomThreshold), 2.5f);
    EXPECT_EQ(back.get(options::render::debugFont), "res:/mono.ttf");
    EXPECT_EQ(back.source(settings::Id::render_ssao), settings::Source::Config);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SettingsTest, OnlyWhatDiffersFromTheDefaultIsWritten) {
    const std::filesystem::path path = scratchConfig();

    settings::Settings s;
    ASSERT_TRUE(s.set(options::render::ssao, false));
    ASSERT_TRUE(s.saveJson(path.string()));

    const std::string text = readText(path);
    EXPECT_NE(text.find("\"ssao\""), std::string::npos);
    // Ninety rows written out in full is a file nobody can read a diff of, which is the
    // same argument `saveBindings` makes for writing only the rebound actions.
    EXPECT_EQ(text.find("\"camera\""), std::string::npos);
    EXPECT_EQ(text.find("\"bloomThreshold\""), std::string::npos);
    // `engine.` rows have no JSON key at all, and a file implying they could be edited is
    // exactly the confusion the prefix exists to prevent.
    EXPECT_EQ(text.find("current_scene_path"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SettingsTest, SavingKeepsEveryKeyTheFileAlreadyHad) {
    const std::filesystem::path path = scratchConfig();
    writeText(path, R"({
  "render": {"msaaSamples": 2},
  "input": {"bindings": {"jump": ["Space"]}},
  "mystery": {"future": 7}
})");

    settings::Settings s;
    ASSERT_TRUE(s.set(options::render::ssao, false));
    ASSERT_TRUE(s.saveJson(path.string()));

    const std::string text = readText(path);
    // A save that dropped the bindings section would be a settings panel that unbinds
    // your keys, and a save that dropped `mystery` would be a config editing itself.
    EXPECT_NE(text.find("bindings"), std::string::npos);
    EXPECT_NE(text.find("mystery"), std::string::npos);
    EXPECT_NE(text.find("\"ssao\""), std::string::npos);

    // A key the file already carried is updated even where the value is now the default,
    // because leaving it would be the file and the program disagreeing.
    settings::Settings back;
    reload(back, path);
    EXPECT_EQ(back.get(options::render::msaaSamples), 4u) << "the run's value, not the file's old one";

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SettingsTest, AConfigThatDoesNotParseIsRefusedRatherThanReplaced) {
    const std::filesystem::path path = scratchConfig();
    writeText(path, "{ this is not json");

    settings::Settings s;
    ASSERT_TRUE(s.set(options::render::ssao, false));
    EXPECT_FALSE(s.saveJson(path.string()));

    // Untouched. A file that fails to parse still holds settings, and replacing it with
    // the one row this table happens to know would lose all of them.
    EXPECT_EQ(readText(path), "{ this is not json");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ============================================================ D11: constants with a door

/**
 * @brief Every generated default is the value the table's own constructor stores.
 *
 * D11 made `core::defaults::<module>::<row>` the single spelling of a built-in, so that a
 * `gfx::Renderer` field can initialise from the row it binds instead of restating the
 * number beside it. That is only worth anything if the two cannot come apart, and this is
 * what says so: one comparison per row, generated from the same list both sides are.
 *
 * Exact equality on the float rows deliberately. The whole reason the constant exists is
 * that a golden image is byte-identical or it is a failure, and a default that rounded on
 * its way through a second spelling would move pixels while reading as the same number.
 */
TEST(SettingsTest, EveryGeneratedDefaultIsWhatTheTableStores) {
    const settings::Settings s;
#define SUBSTRATE_CHECK_DEFAULT(mod, name, tag, ctype, def, lo, hi, flags, label)                                      \
    EXPECT_EQ(s.get(options::mod::name), static_cast<ctype>(defaults::mod::name)) << #mod "." #name;
    SUBSTRATE_SETTINGS(SUBSTRATE_CHECK_DEFAULT)
#undef SUBSTRATE_CHECK_DEFAULT
}

/**
 * @brief The three world-unit lengths D11 promoted still hold the literals they replaced.
 *
 * `ssaoRadius`, `ssaoBias` and `ssrThickness` were literals on `gfx::Renderer` that no
 * config key, no flag and no panel could reach. Giving them rows is only neutral if the
 * row's default *is* the literal, to the bit -- so the numbers are written out here once,
 * as the pin, rather than left to be inferred from a golden set that would fail without
 * saying why.
 */
TEST(SettingsTest, ThePromotedLengthsDefaultToTheLiteralsTheyReplaced) {
    const settings::Settings s;
    EXPECT_EQ(s.get(options::render::ssaoRadius), 0.5f);
    EXPECT_EQ(s.get(options::render::ssaoBias), 0.025f);
    EXPECT_EQ(s.get(options::render::ssrThickness), 0.5f);

    // And the derivation-free claim in the other direction: nothing writes them, so a
    // fresh table reports `default` rather than a source that would mean something
    // recomputed them.
    EXPECT_EQ(s.source(settings::Id::render_ssaoRadius), settings::Source::Default);
    EXPECT_EQ(s.source(settings::Id::render_ssaoBias), settings::Source::Default);
    EXPECT_EQ(s.source(settings::Id::render_ssrThickness), settings::Source::Default);
}

/**
 * @brief A promoted length answers the string door, which is the door that was missing.
 *
 * The complaint on D11's card was not that 0.5 is wrong but that *"there is no way to say
 * otherwise short of recompiling"*. `--set render.ssaoRadius=2` is that way, and it goes
 * through the same parse, the same clamp and the same provenance column as every other
 * row -- which is the whole of what promoting a constant buys.
 */
TEST(SettingsTest, APromotedLengthIsReachableByNameAndReportsWhereItCameFrom) {
    settings::Settings s;
    EXPECT_TRUE(s.setFromString("render.ssaoRadius", "2", settings::Source::Cli, "--set"));
    EXPECT_EQ(s.get(options::render::ssaoRadius), 2.0f);
    EXPECT_EQ(s.source(settings::Id::render_ssaoRadius), settings::Source::Cli);
    EXPECT_EQ(s.origin(settings::Id::render_ssaoRadius), "--set");

    // A game default loses to it, the way D15 decided: the row is claimed.
    EXPECT_FALSE(s.setDefault(options::render::ssaoRadius, 0.75f));
    EXPECT_EQ(s.get(options::render::ssaoRadius), 2.0f);
}

/**
 * @brief The ceiling on a world-unit distance is not Sponza's size.
 *
 * `render.ssrMaxDistance` stopped at 64, which is under twice the diagonal of the one
 * scene the golden suite is mostly made of -- a bound taken from the test scene, which
 * rule 3 says is not an engineering argument. It is 500 now, the same ceiling
 * `render.shadowDistance` and `render.fogMaxDistance` already used, so a large scene gets
 * a march it can ask for rather than a warning it cannot act on.
 */
TEST(SettingsTest, AWorldUnitDistanceIsNotCappedAtTheTestScenesSize) {
    settings::Settings s;
    EXPECT_TRUE(s.setFromString("render.ssrMaxDistance", "300", settings::Source::Config, "substrate.json"));
    EXPECT_EQ(s.get(options::render::ssrMaxDistance), 300.0f);

    EXPECT_TRUE(s.setFromString("render.fogHeightFalloff", "300", settings::Source::Config, "substrate.json"));
    EXPECT_EQ(s.get(options::render::fogHeightFalloff), 300.0f);

    // Still bounded, and still loudly: past the ceiling the value is clamped and the
    // clamp says so, which is the policy every other row already had.
    EXPECT_TRUE(s.setFromString("render.ssrMaxDistance", "5000", settings::Source::Config, "substrate.json"));
    EXPECT_EQ(s.get(options::render::ssrMaxDistance), 500.0f);
}

// ================================================ D16: the default lives on the row

/**
 * @brief A fresh table holds exactly what its rows say their built-ins are.
 *
 * The default used to exist only inside the constructor's macro expansion, which is why
 * `saveJson` and `writeDefaultConfig` each built a whole throwaway `Settings` to answer
 * *does this differ from its default*. Now `defaultString(row)` answers it, and this is
 * what says the two spellings are the same one -- string equality, because that is the
 * comparison the save path actually makes.
 */
TEST(SettingsTest, EveryRowsFreshValueIsTheDefaultOnItsRow) {
    const settings::Settings s;
    for (uint16_t i = 0; i < s.rowCount(); ++i) {
        const auto id = static_cast<settings::Id>(i);
        const settings::Row& r = s.row(id);
        EXPECT_EQ(s.valueString(id), settings::defaultString(r)) << r.key;
    }
}

// ================================================ D16: the table stops being an array

/**
 * @brief A row added at run time is an ordinary row, through both doors.
 *
 * The mechanism D17 hangs `Game::declareSettings` off, and the property that makes it
 * worth having: nothing downstream of `declare` learns that this row was not in the list.
 * The handle is typed -- `Setting<uint32_t>` -- so it is only the *id* that stopped being
 * a compile-time constant, and `s.set(difficulty, true)` still does not compile.
 */
TEST(SettingsTest, ADeclaredRowIsAnOrdinaryRowThroughBothDoors) {
    settings::Settings s;
    const uint16_t before = s.rowCount();

    const settings::Setting<uint32_t> difficulty = s.declare("demo.difficulty", 1u, "Difficulty", 0, 3);
    ASSERT_NE(difficulty.id, settings::Id::None);
    EXPECT_EQ(s.rowCount(), before + 1);
    EXPECT_GE(static_cast<uint16_t>(difficulty.id), settings::count()) << "and it did not take an engine row's id";

    EXPECT_EQ(s.find("demo.difficulty"), difficulty.id);
    EXPECT_EQ(s.get(difficulty), 1u) << "a fresh declared row holds its built-in";
    EXPECT_EQ(s.source(difficulty.id), settings::Source::Default);

    // The string door, the clamp and the refusal are the row's, not the caller's.
    EXPECT_TRUE(s.setFromString("demo.difficulty", "2", settings::Source::Cli, "--set"));
    EXPECT_EQ(s.get(difficulty), 2u);
    EXPECT_TRUE(s.setFromString("demo.difficulty", "9", settings::Source::Cli, "--set"));
    EXPECT_EQ(s.get(difficulty), 3u) << "clamped to the bounds it was declared with";
    EXPECT_FALSE(s.setFromString("demo.difficulty", "hard", settings::Source::Cli, "--set"));

    // And it is enumerable, which is the contract the whole table is held to.
    const std::string text = captured(&settings::Settings::dumpTable, s);
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), s.rowCount());
    EXPECT_NE(text.find("demo.difficulty"), std::string::npos);
}

/**
 * @brief Growth moves nothing that anything else is holding.
 *
 * The hazard this card had to answer, and it has two halves that fail differently. A
 * `bindLive` address is a field *outside* the table, so no amount of growth can move it --
 * but the slot remembering it, the `const std::string&` a `getString` handed back and the
 * `const char*` a declared row's key is are all *inside*, and a `std::vector` would move
 * every one of them on the reallocation the next `declare` causes. Deques are why this
 * passes, and enough rows are declared here to force several reallocations of one.
 */
TEST(SettingsTest, GrowingTheTableMovesNothingAnythingElseIsHolding) {
    settings::Settings s;

    bool ssao = true;
    s.bindLive(settings::Id::render_ssao, &ssao);
    const settings::Setting<std::string> early = s.declare("demo.name", std::string("first"), "Name");
    ASSERT_NE(early.id, settings::Id::None);

    // References taken *before* the growth, of each kind the table hands out.
    const std::string& debugFont = s.getString(settings::Id::render_debugFont);
    const std::string& declaredText = s.getString(early.id);
    const settings::Row& earlyRow = s.row(early.id);
    const settings::Row& engineRow = s.row(settings::Id::render_ssao);

    for (int i = 0; i < 64; ++i) {
        ASSERT_NE(s.declare("demo.row" + std::to_string(i), static_cast<float>(i), "Row", 0.0, 1000.0).id,
                  settings::Id::None);
    }

    EXPECT_EQ(debugFont, "");
    EXPECT_EQ(declaredText, "first");
    EXPECT_STREQ(earlyRow.key, "demo.name");
    EXPECT_STREQ(engineRow.key, "render.ssao");

    // The binding still reaches the same field, in both directions.
    ASSERT_TRUE(s.set(options::render::ssao, false));
    EXPECT_FALSE(ssao) << "a write after growth still reaches the bound field";
    ssao = true;
    EXPECT_TRUE(s.get(options::render::ssao)) << "and a read still comes back through it";
}

/**
 * @brief A row declared after others carries provenance exactly like one that was always
 *        there, and a key read before its row existed is answered rather than swallowed.
 *
 * The second half is the defined answer the ordering question needs, and it is the whole
 * argument for D17 declaring *before* the config file is read: `loadJson` walks the file
 * once, so a key whose row arrives afterwards has already been reported as a typo, and the
 * row keeps its built-in.
 */
TEST(SettingsTest, ALateDeclaredRowTakesTheSameFourSourcesAsAnyOther) {
    {
        settings::Settings s;
        const settings::Setting<float> gain = s.declare("demo.gain", 0.5f, "Gain", 0.0, 1.0);
        ASSERT_NE(gain.id, settings::Id::None);

        rapidjson::Document doc;
        doc.Parse(R"({"demo": {"gain": 0.25}})");
        ASSERT_FALSE(doc.HasParseError());
        s.loadJson(&doc, "substrate.json");

        EXPECT_FLOAT_EQ(s.get(gain), 0.25f);
        EXPECT_EQ(s.source(gain.id), settings::Source::Config);
        EXPECT_EQ(s.origin(gain.id), "substrate.json");

        EXPECT_FALSE(s.setDefault(gain, 0.9f)) << "a game default loses to the user's file here too";
        ASSERT_TRUE(s.set(gain, 0.75f));
        EXPECT_EQ(s.source(gain.id), settings::Source::Game);
        ASSERT_TRUE(s.setFromString("demo.gain", "1", settings::Source::Cli, "--set"));
        EXPECT_EQ(s.source(gain.id), settings::Source::Cli) << "and the command line is still last";
    }
    {
        settings::Settings s;
        rapidjson::Document doc;
        doc.Parse(R"({"demo": {"gain": 0.25}})");
        ASSERT_FALSE(doc.HasParseError());
        s.loadJson(&doc, "substrate.json");

        const settings::Setting<float> gain = s.declare("demo.gain", 0.5f, "Gain", 0.0, 1.0);
        ASSERT_NE(gain.id, settings::Id::None);
        EXPECT_FLOAT_EQ(s.get(gain), 0.5f) << "the file was walked before this row existed to claim its key";
        EXPECT_EQ(s.source(gain.id), settings::Source::Default);
    }
}

/// A declared row saves under its own section and loads back as an ordinary `config` value.
TEST(SettingsTest, ADeclaredRowSavesAndReloadsLikeAnEngineRow) {
    const std::filesystem::path path = scratchConfig();

    settings::Settings s;
    const settings::Setting<uint32_t> difficulty = s.declare("demo.difficulty", 1u, "Difficulty", 0, 3);
    ASSERT_NE(difficulty.id, settings::Id::None);
    ASSERT_TRUE(s.set(difficulty, 3u));
    ASSERT_TRUE(s.saveJson(path.string()));

    const std::string text = readText(path);
    EXPECT_NE(text.find("\"demo\""), std::string::npos);
    EXPECT_NE(text.find("\"difficulty\""), std::string::npos);

    settings::Settings back;
    const settings::Setting<uint32_t> again = back.declare("demo.difficulty", 1u, "Difficulty", 0, 3);
    reload(back, path);
    EXPECT_EQ(back.get(again), 3u);
    EXPECT_EQ(back.source(again.id), settings::Source::Config);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

/**
 * @brief The schema freezes, and `freezeInitOnly` is the latest point it can.
 *
 * The symmetric twin of the init-only freeze: one refuses a *write* after the thing it
 * sized was sized, the other refuses a *row* after everything that reads rows by name has
 * read them. By the end of `Engine::init` the dump, the panel and `bindRenderer` have all
 * walked the table, so a row appearing behind them has no defined answer -- and this is
 * what makes that a refusal rather than an accident. D17 moves the call earlier.
 */
TEST(SettingsTest, NothingIsDeclaredOnceTheSchemaIsFrozen) {
    {
        settings::Settings s;
        s.freezeRows();
        EXPECT_TRUE(s.rowsFrozen());
        EXPECT_EQ(s.declare("demo.late", 1, "Late").id, settings::Id::None);
        EXPECT_EQ(s.rowCount(), settings::count()) << "and nothing was added";
    }
    {
        settings::Settings s;
        s.freezeInitOnly();
        EXPECT_TRUE(s.rowsFrozen()) << "the init-only freeze is past the last point a row can appear";
        EXPECT_EQ(s.declare("demo.late", 1, "Late").id, settings::Id::None);
    }
    { // An `initOnly` row declared *before* the freeze behaves like an engine one after it.
        settings::Settings s;
        const settings::Setting<uint32_t> budget =
            s.declare("demo.budget", 4u, "Budget", 1, 64, settings::kInitOnly);
        ASSERT_NE(budget.id, settings::Id::None);
        ASSERT_TRUE(s.set(budget, 16u));
        s.freezeInitOnly();
        EXPECT_FALSE(s.set(budget, 32u));
        EXPECT_EQ(s.get(budget), 16u);
    }
}

/// The refusals that keep a declared row from shadowing something. D17 widened this ladder;
/// what it may not do is let a key be claimed twice, because `find` answers the first.
TEST(SettingsTest, ADeclarationThatWouldShadowOrMalformAKeyIsRefused) {
    settings::Settings s;
    EXPECT_EQ(s.declare("render.ssao", true, "SSAO").id, settings::Id::None) << "an engine row already claims it";
    EXPECT_EQ(s.declare("demo", 1, "No module").id, settings::Id::None);
    EXPECT_EQ(s.declare("demo.", 1, "No name").id, settings::Id::None);
    EXPECT_EQ(s.declare(".difficulty", 1, "No module").id, settings::Id::None);
    EXPECT_EQ(s.declare("demo.a.b", 1, "Two dots").id, settings::Id::None);

    ASSERT_NE(s.declare("demo.difficulty", 1, "Difficulty").id, settings::Id::None);
    EXPECT_EQ(s.declare("demo.difficulty", 2, "Again").id, settings::Id::None) << "declared twice is once";
    EXPECT_EQ(s.rowCount(), settings::count() + 1);
}

// ============================================ D17: which namespace a game owns

/**
 * @brief A game owns every module the engine does not name, and none that it does.
 *
 * The decision this row had to make, and it is module-granular rather than key-granular on
 * purpose. A `render.` key a game invented would be indistinguishable from an engine row in
 * `substrate.json`, in the generated panel and in `--write-default-config`, all three of
 * which group by module -- and an engine release adding that key later would take over a
 * value the user wrote for the game with nothing anywhere to say so. So the test is the
 * module, and it is answerable at the moment of declaration, which *"is this a row today"*
 * is not.
 *
 * The loop is over the engine's own list rather than over a list of module names written
 * here, for the reason every totality guard in this suite is: a list of names in a test is
 * exactly the thing that stops being maintained when a module is added.
 */
TEST(SettingsTest, AGameOwnsEveryModuleTheEngineDoesNotName) {
    settings::Settings s;

    for (uint16_t i = 0; i < settings::count(); ++i) {
        const std::string_view key = s.row(static_cast<settings::Id>(i)).key;
        const std::string module(key.substr(0, key.find('.')));
        EXPECT_EQ(s.declare(module + ".somethingAGameWants", 1, "Refused").id, settings::Id::None)
            << module << " is the engine's, whether or not a row claims that key today";
    }
    EXPECT_EQ(s.rowCount(), settings::count()) << "and nothing was added by any of them";

    // `engine.` falls out of the same test rather than being special-cased -- it is an
    // engine module like any other, and the row above proved it.
    EXPECT_EQ(s.declare("engine.current_scene_path", std::string("x"), "Shadow").id, settings::Id::None);

    // And the module a game names is its own, in every type the table holds.
    EXPECT_NE(s.declare("demo.difficulty", 2, "Difficulty", 1, 3).id, settings::Id::None);
    EXPECT_NE(s.declare("accessibility.subtitleSize", 18.0f, "Subtitle size", 8.0, 48.0).id, settings::Id::None);
    EXPECT_EQ(s.rowCount(), settings::count() + 2);
}

/**
 * @brief A key that *used to be* an engine setting cannot be declared, and a game may not
 *        claim `kEngine`.
 *
 * The entry the card said was easy to miss, and the shape that gets past the module test is
 * `lighting.sun`: the engine does not name `lighting` at all any more, so nothing about the
 * module refuses it -- while every `substrate.json` still carrying that key would silently
 * start feeding a row it was never written for. D14 is about to add forty more of these.
 *
 * `kEngine` is the same failure from the other direction. It means *live state the engine
 * reports and no JSON key names*, so a game's row wearing it would be one the string door
 * refuses, the save never writes and the panel draws as a readout -- a control that reports
 * success and changes nothing.
 */
TEST(SettingsTest, AGameCannotClaimAKeyTheEngineRetiredOrAFlagOnlyTheEngineOwns) {
    settings::Settings s;

    uint32_t retired = 0;
    for (const settings::RemovedKey& removed : settings::removedKeys()) {
        EXPECT_EQ(s.declare(removed.key, 1, "Refused").id, settings::Id::None) << removed.key;
        ++retired;
    }
    EXPECT_GT(retired, 0u) << "the removed list is what this walks; an empty one would pass vacuously";
    EXPECT_EQ(s.rowCount(), settings::count());

    // `lighting.` specifically, because it is the one whose module the engine no longer
    // names -- the case the module test above cannot reach. That a *new* key in the same
    // module is accepted is what says so: the module is a game's, and only the retired key
    // itself is refused, because only that key is what a file already in the wild means.
    EXPECT_EQ(s.declare("lighting.sun", 1.0f, "Sun", 0.0, 10.0).id, settings::Id::None);
    EXPECT_NE(s.declare("lighting.torchWarmth", 1.0f, "Warmth", 0.0, 10.0).id, settings::Id::None);

    EXPECT_EQ(s.declare("demo.scene", std::string(""), "Scene", 0, 0, settings::kEngine).id, settings::Id::None);
    // `initOnly` is not refused -- a game sizing something at startup is an ordinary thing
    // to want, and the table already refuses the late write with a reason.
    EXPECT_NE(s.declare("demo.budget", 8u, "Budget", 1, 64, settings::kInitOnly).id, settings::Id::None);
}

/**
 * @brief A file key belonging to a game row nobody declared this run is refused, reported
 *        as an orphan rather than as a typo, and **left in the file**.
 *
 * The other half of the namespace decision, and the one a user notices. A game ships
 * `demo.impactVolume`, the user sets it, the game removes the setting in the next version --
 * or the same `substrate.json` is used by a second game, or by the engine's own test scene
 * with no game at all. The value has nowhere to go, so it is refused, exactly as D12 refuses
 * a name that is not one: the run continues and the message names what happened.
 *
 * What makes it an orphan rather than a typo is that *nothing declares the module*, and the
 * two get different sentences because they call for different actions. And the key survives
 * the next save, which is the part that matters: `saveJson` merges into the file it read, so
 * one game's section is not another game's to delete.
 */
TEST(SettingsTest, AnOrphanedGameKeyIsRefusedAndLeftInTheFile) {
    const std::filesystem::path path = scratchConfig();
    writeText(path, R"({
  "demo": {"impactVolume": 0.25},
  "render": {"ssao": false}
})");

    // Nothing declares `demo` in this table, which is what makes the key an orphan rather
    // than a typo.
    settings::Settings s;
    reload(s, path);

    EXPECT_EQ(s.find("demo.impactVolume"), settings::Id::None) << "an orphan does not conjure a row";
    EXPECT_EQ(s.rowCount(), settings::count());
    EXPECT_FALSE(s.get(options::render::ssao)) << "and the rest of the file still applied";

    // The save is what decides whether the user loses their answer. It writes the row it
    // moved and keeps every key it did not understand.
    ASSERT_TRUE(s.set(options::render::bloomStrength, 0.2f));
    ASSERT_TRUE(s.saveJson(path.string()));

    const std::string text = readText(path);
    EXPECT_NE(text.find("\"impactVolume\""), std::string::npos) << "an orphan survives a save by a run that ignored it";
    EXPECT_NE(text.find("0.25"), std::string::npos);

    // And the run that declares it again gets the value back, which is the whole point of
    // keeping it.
    settings::Settings back;
    const settings::Setting<float> volume = back.declare("demo.impactVolume", 1.0f, "Impact volume", 0.0, 1.0);
    ASSERT_NE(volume.id, settings::Id::None);
    reload(back, path);
    EXPECT_FLOAT_EQ(back.get(volume), 0.25f);
    EXPECT_EQ(back.source(volume.id), settings::Source::Config);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

namespace {

/// Whether `settings.set(Setting<Row>, Value)` compiles at all. The typed door's whole claim
/// is a compile-time one, so the only honest way to check it is to ask the compiler.
template <typename Row, typename Value, typename = void>
struct SetCompiles : std::false_type {};
template <typename Row, typename Value>
struct SetCompiles<Row, Value,
                   std::void_t<decltype(std::declval<settings::Settings&>().set(
                       std::declval<settings::Setting<Row>>(), std::declval<Value>()))>> : std::true_type {};

} // namespace

/**
 * @brief A declared row's handle carries its type into the compiler, exactly as an engine
 *        row's `constexpr` handle does.
 *
 * The property that had to survive ids being assigned at run time, and the reason `declare`
 * answers with a `Setting<T>` rather than an `Id`. Deduction is what enforces it: `set` takes
 * `Setting<T>` and `T`, so `set(difficulty, true)` deduces `T` as both `int` and `bool` and
 * does not compile -- it is not a conversion that happens to be checked, and there is no
 * runtime path it could fall down instead.
 */
TEST(SettingsTest, ADeclaredRowsHandleRefusesTheWrongTypeAtCompileTime) {
    static_assert(SetCompiles<int, int>::value, "the row's own type is the one that works");
    static_assert(!SetCompiles<int, bool>::value, "set(difficulty, true) on an integer row must not compile");
    static_assert(!SetCompiles<float, int>::value, "and nor may a silent int-to-float widening");
    static_assert(!SetCompiles<std::string, const char*>::value, "a String row takes a std::string, not a literal");
    static_assert(!SetCompiles<uint32_t, int>::value);

    settings::Settings s;
    const settings::Setting<int> difficulty = s.declare("demo.difficulty", 2, "Difficulty", 1, 3);
    ASSERT_NE(difficulty.id, settings::Id::None);
    static_assert(std::is_same_v<decltype(std::declval<const settings::Settings&>().get(difficulty)), int>,
                  "and the read comes back as the row's type rather than as something to cast");

    // The runtime half of the same statement, for the string door, which has no compiler to
    // ask: a type the row is not is refused rather than coerced.
    EXPECT_FALSE(s.setValue(difficulty.id, true, settings::Source::Cli, "--set"));
    EXPECT_EQ(s.get(difficulty), 2);
}

/**
 * @brief A handle naming no row reads the first row and writes nothing.
 *
 * `Id::None` exists so that `Count` can be the boundary a declared row's id starts at, and
 * a handle carrying it is what a refused `declare` answers with. Every getter used to index
 * the slot array unchecked, which was safe only while every `Id` was an enumerator.
 */
TEST(SettingsTest, AHandleNamingNoRowReadsTheFirstRowAndWritesNothing) {
    settings::Settings s;
    const settings::Setting<int> refused = s.declare("no-module", 7, "Refused");
    ASSERT_EQ(refused.id, settings::Id::None);

    EXPECT_STREQ(s.row(refused.id).key, s.row(static_cast<settings::Id>(0)).key) << "the first row, not past the end";
    EXPECT_EQ(s.get(refused), s.get(options::window::width));

    EXPECT_FALSE(s.set(refused, 42)) << "and a write through it changes nothing at all";
    EXPECT_EQ(s.get(options::window::width), 1600);
}

/**
 * @brief Two tables are independent, which is the whole reason `row` and `find` are members.
 *
 * The alternative for a declared row was a process-global registry, and the header already
 * says *"One instance, owned by `Engine`"*. A global would make that comment false and would
 * leak one test's rows into another's `saveJson` -- so this is the case that could not have
 * been written at all under the design that was refused.
 */
TEST(SettingsTest, TwoTablesDoNotShareWhatEitherDeclares) {
    settings::Settings mine;
    settings::Settings yours;

    ASSERT_NE(mine.declare("demo.difficulty", 1u, "Difficulty", 0, 3).id, settings::Id::None);
    EXPECT_EQ(yours.find("demo.difficulty"), settings::Id::None);
    EXPECT_EQ(yours.rowCount(), settings::count());
    EXPECT_EQ(mine.rowCount(), settings::count() + 1);
}
