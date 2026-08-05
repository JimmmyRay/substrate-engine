#include "core/Config.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace core;

namespace fs = std::filesystem;

/**
 * @file tests/ConfigTests.cpp
 * @brief JSON configuration and its command-line overrides (5.1).
 *
 * The contract worth defending is "every key is optional": an absent key keeps the
 * default rather than failing, which is what lets an old config file load against a new
 * build. A parser that quietly zeroed an absent field instead would look identical at
 * the call site and produce a black screen three subsystems later.
 *
 * Since S4 most of a config file is rows in `settings::Settings`, and the table's own
 * contract is defended in SettingsTests.cpp. What is tested here is the part `Config`
 * still owns: the file-to-table plumbing, the two aggregates, the flags that have no key,
 * and the derived values.
 */

namespace {

class ConfigTest : public ::testing::Test {
  protected:
    fs::path dir;

    void SetUp() override {
        dir = fs::temp_directory_path() / "substrate_config_tests";
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    fs::path write(const char* name, const std::string& text) {
        const fs::path p = dir / name;
        std::ofstream out(p);
        out << text;
        return p;
    }
};

/// argv wants char*, and a string literal is not one. Owns its storage.
class Argv {
  public:
    explicit Argv(std::vector<std::string> args) : storage(std::move(args)) {
        for (auto& s : storage) pointers.push_back(s.data());
    }
    int argc() const { return static_cast<int>(pointers.size()); }
    char** argv() { return pointers.data(); }

  private:
    std::vector<std::string> storage;
    std::vector<char*> pointers;
};

} // namespace

// =================================================================== loading

TEST_F(ConfigTest, MissingFileKeepsDefaultsAndIsNotAnError) {
    Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(dir / "does-not-exist.json"));
    EXPECT_TRUE(cfg.sourcePath.empty()) << "sourcePath names where the config came from, and it came from nowhere";
    EXPECT_EQ(cfg.settings.get(options::window::width), 1600);
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 4u);
}

TEST_F(ConfigTest, MalformedJsonFails) {
    const fs::path p = write("broken.json", "{ \"window\": { \"width\": }");
    Config cfg;
    EXPECT_FALSE(cfg.loadFromFile(p));
}

TEST_F(ConfigTest, NonObjectTopLevelFails) {
    const fs::path p = write("array.json", "[1, 2, 3]");
    Config cfg;
    EXPECT_FALSE(cfg.loadFromFile(p));
}

TEST_F(ConfigTest, EmptyObjectLoadsAndChangesNothing) {
    const fs::path p = write("empty.json", "{}");
    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(p));

    const Config defaults;
    EXPECT_EQ(cfg.settings.get(options::window::width), defaults.settings.get(options::window::width));
    EXPECT_EQ(cfg.settings.get(options::render::debugFont), defaults.settings.get(options::render::debugFont));
    EXPECT_EQ(cfg.settings.get(options::audio::sampleRate), defaults.settings.get(options::audio::sampleRate));
    EXPECT_EQ(cfg.sourcePath, p);
}

TEST_F(ConfigTest, APartialFileOverridesOnlyWhatItNames) {
    const fs::path p = write("partial.json", R"({
      "window": { "width": 800 },
      "render": { "msaaSamples": 8, "bloomThreshold": 2.5 }
    })");

    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(p));

    EXPECT_EQ(cfg.settings.get(options::window::width), 800);
    EXPECT_EQ(cfg.settings.get(options::window::height), 900) << "an absent sibling key keeps its default";
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 8u);
    EXPECT_FLOAT_EQ(cfg.settings.get(options::render::bloomThreshold), 2.5f);
    EXPECT_TRUE(cfg.settings.get(options::render::ssao)) << "an absent section keeps every default in it";
}

TEST_F(ConfigTest, AKeyOfTheWrongTypeKeepsItsDefault) {
    // The contract that lets an old file load against a new build, applied one key at a
    // time rather than to the file as a whole.
    const fs::path p = write("mistyped.json", R"({
      "render": { "msaaSamples": "eight", "ssao": 1 },
      "window": { "width": 640 }
    })");

    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(p));
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 4u);
    EXPECT_TRUE(cfg.settings.get(options::render::ssao)) << "1 is not a bool as far as JSON is concerned";
    EXPECT_EQ(cfg.settings.get(options::window::width), 640) << "and a well-typed sibling still loads";
}

TEST_F(ConfigTest, ProvenanceNamesTheFileAKeyCameFrom) {
    // The column that earns --dump-settings: knowing a value is 8 does not tell you
    // whether your edit took.
    const fs::path p = write("sourced.json", R"({"render": {"msaaSamples": 8}})");

    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(p));
    EXPECT_EQ(cfg.settings.source(settings::Id::render_msaaSamples), settings::Source::Config);
    EXPECT_EQ(cfg.settings.origin(settings::Id::render_msaaSamples), p.string());
    EXPECT_EQ(cfg.settings.source(settings::Id::render_ssao), settings::Source::Default);
}

TEST_F(ConfigTest, TheOneRemainingAggregateStillParses) {
    // Bindings are what a table of typed scalars cannot hold, so they stay hand-parsed --
    // and stay tested. `logging.categories` was the second and left the file with D14: the
    // three `logging` rows beside it went, and an aggregate parsed out of a module no row
    // claims is a section a game could declare into while the engine still reads a key
    // from it. It is `--log-categories` now, and the case below drives that door.
    const fs::path p = write("aggregates.json", R"({
      "input": { "bindings": { "Camera.Forward": "W", "Player.Jump": ["Space", "Pad.A"] } }
    })");

    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(p));

    ASSERT_EQ(cfg.input.bindings.size(), 2u);
    EXPECT_EQ(cfg.input.bindings[0].first, "Camera.Forward");
    EXPECT_EQ(cfg.input.bindings[0].second, "W");
    EXPECT_EQ(cfg.input.bindings[1].second, "Space Pad.A") << "an array joins with spaces";
}

TEST_F(ConfigTest, LogCategoriesArriveOnTheCommandLineNow) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--log-categories", "vulkan,render"});
    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));

    EXPECT_EQ(cfg.logCategoryMask(),
              static_cast<uint32_t>(LogCategory::Vulkan) | static_cast<uint32_t>(LogCategory::Render));
}

TEST_F(ConfigTest, WrittenDefaultsRoundTripToTheBuiltInDefaults) {
    // writeDefaultConfig is generated from the table now, so it cannot claim a default the
    // table does not hold -- but it is still a hand-written *emitter*, and a file whose
    // "defaults" do not parse back is worse than no file at all.
    const fs::path p = dir / "written.json";
    const Config defaults;
    ASSERT_TRUE(writeDefaultConfig(defaults.settings, p));

    Config loaded;
    ASSERT_TRUE(loaded.loadFromFile(p));

    for (uint16_t i = 0; i < defaults.settings.rowCount(); ++i) {
        const auto id = static_cast<settings::Id>(i);
        const settings::Row& r = defaults.settings.row(id);
        if ((r.flags & settings::kEngine) != 0) continue;
        EXPECT_EQ(loaded.settings.valueString(id), defaults.settings.valueString(id)) << r.key;
        EXPECT_EQ(loaded.settings.source(id), settings::Source::Config) << r.key << " is missing from the written file";
    }
}

TEST_F(ConfigTest, TheWrittenDefaultConfigNamesEachModuleExactlyOnce) {
    // It was failing before that card. The emitter opened a section whenever the
    // module changed while walking the rows in list order, and the list interleaves, so
    // `"render"` was written twice. The round trip above passed anyway -- rapidjson
    // tolerates a duplicate member and the loader walks all of them -- while every other
    // JSON reader rejects the file outright.
    const fs::path p = dir / "sections.json";
    const Config defaults;
    ASSERT_TRUE(writeDefaultConfig(defaults.settings, p));

    std::ifstream in(p);
    std::map<std::string, int> opened;
    for (std::string line; std::getline(in, line);) {
        // A section heading is the only thing at two spaces of indent that ends in `: {`.
        if (line.size() > 3 && line[0] == ' ' && line[1] == ' ' && line[2] == '"' && line.back() == '{') {
            ++opened[line.substr(3, line.find('"', 3) - 3)];
        }
    }

    ASSERT_FALSE(opened.empty()) << "no sections were written at all";
    for (const auto& [module, times] : opened) {
        EXPECT_EQ(times, 1) << module << " opens " << times << " times";
    }
}

// ================================================ a game's own rows

/**
 * @brief A row a game declared is an ordinary row at every door `Config` owns.
 *
 * `Engine::init` is not hosted, so what this drives is the *sequence* rather than the
 * engine: declare, freeze, read the file, then the flags -- which is exactly the order
 * `Engine::init` opens them in and the order the whole design rests on. The row loads from
 * its own JSON section, takes `--set` by its key with no flag table anywhere, and reports
 * `cli` with `--set` in the origin column like any engine row.
 */
TEST_F(ConfigTest, ADeclaredRowLoadsFromTheFileAndTakesSetFromTheCommandLine) {
    const fs::path p = write("declared.json", R"({
      "demo": {"impactVolume": 0.25, "impactDust": false},
      "render": {"ssao": false}
    })");

    Config cfg;
    const settings::Setting<float> volume = cfg.settings.declare("demo.impactVolume", 1.0f, "Volume", 0.0, 1.0);
    const settings::Setting<bool> dust = cfg.settings.declare("demo.impactDust", true, "Dust");
    ASSERT_NE(volume.id, settings::Id::None);
    ASSERT_NE(dust.id, settings::Id::None);
    cfg.settings.freezeRows();

    ASSERT_TRUE(cfg.loadFromFile(p));
    EXPECT_FLOAT_EQ(cfg.settings.get(volume), 0.25f);
    EXPECT_FALSE(cfg.settings.get(dust));
    EXPECT_EQ(cfg.settings.source(volume.id), settings::Source::Config);
    EXPECT_EQ(cfg.settings.origin(volume.id), p.string());

    // The game's own default loses to the file it was read after, and the flags beat both.
    EXPECT_FALSE(cfg.settings.setDefault(volume, 0.9f));

    Argv args({"substrate", "--set", "demo.impactVolume=0.75"});
    int exitCode = 0;
    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));

    EXPECT_FLOAT_EQ(cfg.settings.get(volume), 0.75f);
    EXPECT_EQ(cfg.settings.source(volume.id), settings::Source::Cli);
    EXPECT_EQ(cfg.settings.origin(volume.id), "--set");
}

/// `--write-default-config` writes a game's section too, which it has to: the file it emits
/// is the one a user edits, and a game's settings missing from it would be settings that
/// exist everywhere except where somebody would look for them. It can only do that because
/// the schema is complete before any flag is parsed -- this flag exits from inside
/// `applyCommandLine`, which is after `declareSettings` and after the freeze.
TEST_F(ConfigTest, TheWrittenDefaultConfigCarriesAGamesOwnSection) {
    const fs::path p = dir / "with-game.json";
    Config cfg;
    ASSERT_NE(cfg.settings.declare("demo.impactVolume", 0.5f, "Volume", 0.0, 1.0).id, settings::Id::None);
    ASSERT_TRUE(writeDefaultConfig(cfg.settings, p));

    std::ifstream in(p);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("\"demo\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"impactVolume\""), std::string::npos) << text;

    // And it still parses, with the section landing on the row rather than beside it.
    Config back;
    const settings::Setting<float> volume = back.settings.declare("demo.impactVolume", 1.0f, "Volume", 0.0, 1.0);
    ASSERT_TRUE(back.loadFromFile(p));
    EXPECT_FLOAT_EQ(back.settings.get(volume), 0.5f);
    EXPECT_EQ(back.settings.source(volume.id), settings::Source::Config);
}

// =========================================================== derived values

// ==================================================== one name list per enum

/**
 * @brief Every enumerator is reachable by some name, and the round trip is total.
 *
 * A loop over `Count` rather than a list of names, and that is the whole point of the
 * test: a list of names is exactly what stops being maintained. `TonemapOperator` had an
 * enumerator that was in `tonemapKey` and not in the alias table, reachable from code and
 * from no file, and a test spelling out three names would have passed.
 *
 * The two directions are checked together because either alone is satisfiable by a broken
 * list: `nameOf` non-null says a value has *a* name, and `parseName` round-tripping says
 * that name is *its own*.
 */
template <typename E>
void expectNamesCoverEveryValue(core::Names<E> names) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(E::Count); ++i) {
        const auto value = static_cast<E>(i);
        const char* name = core::nameOf(names, value);
        ASSERT_NE(name, nullptr) << "value " << i << " has no name";
        EXPECT_EQ(core::parseName(names, name), value) << name;
    }
    EXPECT_EQ(core::nameOf(names, E::Count), nullptr) << "Count is not a value and must not be named";
}

TEST_F(ConfigTest, EveryEnumeratorIsReachableByExactlyOneCanonicalName) {
    expectNamesCoverEveryValue<core::DebugView>(core::debugViewNames());
    expectNamesCoverEveryValue<core::TonemapOperator>(core::tonemapNames());
    expectNamesCoverEveryValue<LogLevel>(logLevelNames());
    expectNamesCoverEveryValue<Tristate>(tristateNames());
    expectNamesCoverEveryValue<core::AudioBackend>(core::audioBackendNames());

    // The two masks have no `Count` to walk, so they are walked over the mask instead --
    // which is the thing a new bit has to be added to anyway. `LogOutput::Both` is every
    // bit there is, so 1..Both is every legal combination.
    for (uint8_t mask = 1; mask <= static_cast<uint8_t>(LogOutput::Both); ++mask) {
        const auto value = static_cast<LogOutput>(mask);
        const char* name = core::nameOf(logOutputNames(), value);
        ASSERT_NE(name, nullptr) << "log output " << static_cast<int>(mask) << " has no name";
        EXPECT_EQ(core::parseName(logOutputNames(), name), value) << name;
    }
    for (uint32_t bit = 1; bit <= AllLogCategories; bit <<= 1) {
        const auto value = static_cast<LogCategory>(bit);
        const char* name = core::nameOf(logCategoryNames(), value);
        ASSERT_NE(name, nullptr) << "log category " << bit << " has no name";
        EXPECT_EQ(core::parseName(logCategoryNames(), name), value) << name;
    }
}

TEST_F(ConfigTest, ADefaultConfigHoldsEveryNamedValueAsItsOwnEnumerator) {
    // This used to pin a *string* default against the enumerator an accessor fell back
    // to, because seven named values were `String` rows and were resolved on every read.
    // D14 took all seven out of the table, so there is one spelling left and the check is
    // simply that a default Config is the one the program documents.
    const Config cfg;
    EXPECT_EQ(cfg.logging.level, LogLevel::Status);
    EXPECT_EQ(cfg.logging.output, LogOutput::Both);
    EXPECT_EQ(cfg.logging.file, "debug_frames/substrate.log");
    EXPECT_EQ(cfg.render.tonemap, core::TonemapOperator::Aces);
    EXPECT_FALSE(cfg.render.tonemapNamed) << "nothing named one, so GameSetup's curve stands";
    EXPECT_EQ(cfg.audio.backend, core::AudioBackend::Auto);
    EXPECT_EQ(cfg.render.debugView, core::DebugView::Lit);
    EXPECT_TRUE(cfg.render.debugOverlay);
    EXPECT_FALSE(cfg.render.syncValidation);
    EXPECT_TRUE(cfg.physics.enabled);
    EXPECT_FALSE(cfg.ui.panel);
    EXPECT_FALSE(cfg.record.enabled);
    EXPECT_EQ(cfg.benchmark.exitAfterFrames, 0u);
    EXPECT_TRUE(cfg.validationEnabled(true)) << "auto follows the build type";
    EXPECT_FALSE(cfg.validationEnabled(false));
    EXPECT_TRUE(cfg.rayQueryAllowed()) << "auto means wherever the device offers it";
}

TEST_F(ConfigTest, EveryKeyD14RemovedIsAnnouncedRatherThanIgnored) {
    // The load-bearing half of that card, and the reason it is a test rather than a
    // review: forty-ish keys are in people's config files today, and a row that left
    // silently is exactly the failure section 7 forbids, committed by the fix for it.
    //
    // The loop is over `removedKeys()` rather than a list written here, because a list
    // written in a test is the thing that stops being maintained. Each key is written into
    // a file, loaded, and asserted not to have become a row again.
    for (const settings::RemovedKey& removed : settings::removedKeys()) {
        const std::string key = removed.key;
        const size_t dot = key.find('.');
        if (dot == std::string::npos) continue; // `decals`, a bare top-level key

        Config cfg;
        EXPECT_EQ(cfg.settings.find(key), settings::Id::None)
            << key << " is in removedKeys and is also a row; one of the two is wrong";
        ASSERT_NE(removed.message, nullptr);
        EXPECT_GT(std::string(removed.message).size(), 20u) << key << " has no sentence saying where it went";

        const fs::path p = write("removed.json", "{\"" + key.substr(0, dot) + "\": {\"" + key.substr(dot + 1) +
                                                     "\": 0}}");
        EXPECT_TRUE(cfg.loadFromFile(p)) << key << ": the run continues";
    }
}

TEST_F(ConfigTest, EveryRemainingRowsModuleIsOneOfTheEightThatSurvivedTheAudit) {
    // D14's other half. The table kept absorbing developer controls because a row is one
    // line and comes with a parser, a flag, a panel widget and persistence for free, and
    // the drift is invisible one row at a time. Naming the eight modules makes the next
    // one visible: a row in a ninth does not compile past this case without an argument.
    static constexpr std::string_view kModules[] = {"window", "render", "input",  "camera",
                                                    "physics", "audio", "ui",     "engine"};
    const Config cfg;
    for (uint16_t i = 0; i < cfg.settings.rowCount(); ++i) {
        const std::string_view key = cfg.settings.row(static_cast<settings::Id>(i)).key;
        const std::string_view module = key.substr(0, key.find('.'));
        EXPECT_NE(std::find(std::begin(kModules), std::end(kModules), module), std::end(kModules))
            << key << " is in a module the settings audit removed";
    }
}

TEST_F(ConfigTest, AFrameBudgetUnmapsTheWindowAndThreeThingsOptOut) {
    // Deleting any arm of this leaves a harness able to take the keyboard off whoever is
    // working, in a run that reports nothing wrong -- which is how it was found.
    {
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--frames", "60"});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_TRUE(cfg.window.headless);
    }
    {
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--frames", "60", "--windowed"});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_FALSE(cfg.window.headless);
    }
    {
        // `startRecording` refuses a headless run, so implying it here would turn every
        // recorded benchmark into an error message and an empty file.
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--frames", "60", "--record"});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_FALSE(cfg.window.headless);
    }
    {
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate"});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_FALSE(cfg.window.headless) << "an interactive run is untouched";
    }
}

TEST_F(ConfigTest, AnAliasParsesAndIsCanonicalisedByTheFlagThatCarriesIt) {
    // The first entry naming a value is canonical, so an alias is an input spelling and
    // nothing downstream ever sees it. D12 had to say this about the *table*, because
    // seven rows held a name as a string and a save would otherwise have written the alias
    // back; D14 removed those rows, so the alias stops at the parse and what is stored is
    // a value of the enum's own type. There is nothing left for a save to spell wrongly.
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--tonemap", "none", "--validation", "true", "--hot-reload", "FALSE", "--log-level",
               "WARNING"});
    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));

    EXPECT_EQ(cfg.render.tonemap, core::TonemapOperator::Clamp);
    EXPECT_TRUE(cfg.render.tonemapNamed);
    EXPECT_EQ(cfg.logging.level, LogLevel::Warn);
    EXPECT_TRUE(cfg.validationEnabled(false)) << "true is on";
    EXPECT_FALSE(cfg.shaderHotReloadEnabled(true)) << "names are matched case-insensitively";
}

TEST_F(ConfigTest, AConfigFileCarryingTheOldNamedRowsIsAnnouncedAndTheDefaultStands) {
    // What this case used to be was the file's half of D12's refusal, over the seven
    // `String` rows that held a name. Every one of them is a removed key now, so the same
    // file exercises the *other* half of the contract D14 owes: a value that used to work
    // gets a sentence rather than silence, and nothing about the program changes.
    const fs::path p = write("typos.json", R"({
      "render": { "tonemap": "reinhardt", "validation": "of", "rayQuery": "yes",
                  "shaderHotReload": "sometimes" },
      "logging": { "level": "verbse", "output": "terminl" },
      "audio":  { "backend": "devcie" }
    })");

    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(p)) << "the value is refused, not the run";

    const Config defaults;
    EXPECT_EQ(cfg.render.tonemap, defaults.render.tonemap);
    EXPECT_FALSE(cfg.render.tonemapNamed);
    EXPECT_EQ(cfg.render.validation, defaults.render.validation);
    EXPECT_EQ(cfg.render.rayQuery, defaults.render.rayQuery);
    EXPECT_EQ(cfg.render.shaderHotReload, defaults.render.shaderHotReload);
    EXPECT_EQ(cfg.logging.level, defaults.logging.level);
    EXPECT_EQ(cfg.logging.output, defaults.logging.output);
    EXPECT_EQ(cfg.audio.backend, defaults.audio.backend);

    for (const char* key : {"render.tonemap", "render.validation", "render.rayQuery", "render.shaderHotReload",
                            "logging.level", "logging.output", "audio.backend"}) {
        EXPECT_EQ(cfg.settings.find(key), settings::Id::None) << key << " must not be a row again";
    }
}

TEST_F(ConfigTest, AFlagNameThatIsNotOneIsRefusedAndLeavesThePreviousValueStanding) {
    // The half that matters to a benchmark sweep: the arm that was asked for is refused
    // out loud rather than silently measuring whatever was already configured.
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--tonemap", "reinhard", "--tonemap", "reinhardt", "--log-level", "verbse",
               "--validation", "of", "--msaa", "2"});
    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));

    EXPECT_EQ(cfg.render.tonemap, core::TonemapOperator::Reinhard) << "what the first flag said, not the typo";
    EXPECT_TRUE(cfg.render.tonemapNamed) << "the accepted one still overrides the game";
    EXPECT_EQ(cfg.logging.level, LogLevel::Status);
    EXPECT_TRUE(cfg.validationEnabled(true));
    EXPECT_FALSE(cfg.validationEnabled(false)) << "still auto";
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 2u) << "and the rest of the line still applies";
}

TEST_F(ConfigTest, ARefusedTonemapDoesNotClaimToHaveOverriddenTheGame) {
    // `tonemapNamed` follows the *parse* rather than the flag, and this is why: the curve
    // is GameSetup's, so a flag that was refused must leave the game's answer standing
    // rather than pin the previous value over it.
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--tonemap", "reinhardt"});
    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_FALSE(cfg.render.tonemapNamed);
}

TEST_F(ConfigTest, ADebugViewNameThatIsNotOneIsRefusedByTheFlag) {
    // `--debug-view` is the one named value with no row behind it. It used to accept any
    // string and let `Config::debugView()` decide an unrecognised one meant `lit`, so four
    // golden cases were one typo away from photographing the lit buffer and passing.
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--debug-view", "ssao", "--debug-view", "sao"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_EQ(cfg.render.debugView, core::DebugView::Ssao);
}

TEST_F(ConfigTest, EveryDebugViewNameStillReachesItsView) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(core::DebugView::Count); ++i) {
        const auto view = static_cast<core::DebugView>(i);
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--debug-view", core::debugViewKey(view)});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_EQ(cfg.render.debugView, view) << core::debugViewKey(view);
    }
}

TEST_F(ConfigTest, CategoryMaskCollapsesToAllForAllOrForNothingRecognised) {
    Config cfg;

    cfg.logging.categories = {"vulkan", "render"};
    EXPECT_EQ(cfg.logCategoryMask(),
              static_cast<uint32_t>(LogCategory::Vulkan) | static_cast<uint32_t>(LogCategory::Render));

    cfg.logging.categories = {"vulkan", "all", "render"};
    EXPECT_EQ(cfg.logCategoryMask(), AllLogCategories);

    // Everything unrecognised is the same as saying nothing, and saying nothing must
    // not mean "log nothing" -- a typo that silenced the log would be invisible.
    cfg.logging.categories = {"nonsense"};
    EXPECT_EQ(cfg.logCategoryMask(), AllLogCategories);
}

TEST_F(ConfigTest, EveryTonemapNameStillReachesItsOperator) {
    // They return `core::TonemapOperator` rather than a magic integer since S4, and come
    // out of one list rather than two since D12. A loop over `Count` rather than three
    // spelled-out names, because an operator added to the enum and to `tonemapKey` and to
    // nothing else would have passed a list.
    for (uint32_t i = 0; i < static_cast<uint32_t>(core::TonemapOperator::Count); ++i) {
        const auto op = static_cast<core::TonemapOperator>(i);
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--tonemap", core::tonemapKey(op)});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_EQ(cfg.render.tonemap, op) << core::tonemapKey(op);
    }
}

TEST_F(ConfigTest, EveryLogLevelAndDestinationNameStillReachesItsValue) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(LogLevel::Count); ++i) {
        const auto level = static_cast<LogLevel>(i);
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--log-level", core::nameOf(logLevelNames(), level)});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_EQ(cfg.logging.level, level) << core::nameOf(logLevelNames(), level);
    }

    for (uint8_t mask = 1; mask <= static_cast<uint8_t>(LogOutput::Both); ++mask) {
        const auto output = static_cast<LogOutput>(mask);
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--log-output", core::nameOf(logOutputNames(), output)});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_EQ(cfg.logging.output, output) << core::nameOf(logOutputNames(), output);
    }
}

TEST_F(ConfigTest, TristateFlagsFollowTheBuildTypeOnlyWhenSetToAuto) {
    // Every state of the enum, reached by its own name through a door -- not by writing
    // three strings a reader hopes the parser agrees with.
    for (uint32_t i = 0; i < static_cast<uint32_t>(Tristate::Count); ++i) {
        const auto state = static_cast<Tristate>(i);
        const char* name = core::nameOf(tristateNames(), state);
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--validation", name});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));

        EXPECT_EQ(cfg.validationEnabled(true), enabled(state, true)) << name;
        EXPECT_EQ(cfg.validationEnabled(false), enabled(state, false)) << name;
    }

    {
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--hot-reload", "auto"});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_TRUE(cfg.shaderHotReloadEnabled(true));
        EXPECT_FALSE(cfg.shaderHotReloadEnabled(false));
    }

    // rayQuery is the odd one: auto means "wherever the device offers it", so the only
    // value that turns it off here is an explicit off. That difference is the whole
    // reason `enabled` takes what `auto` means as an argument, and the reason there is a
    // `--no-ray-query` and no `--ray-query`.
    {
        Config cfg;
        EXPECT_TRUE(cfg.rayQueryAllowed()) << "auto";
        int exitCode = -1;
        Argv args({"substrate", "--no-ray-query"});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_FALSE(cfg.rayQueryAllowed());
    }
}

// =============================================================== command line

TEST_F(ConfigTest, FlagsOverrideTheLoadedFile) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--msaa", "8", "--frames", "600", "--no-ssao", "--taa", "--tonemap", "reinhard"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_EQ(exitCode, 0);
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 8u);
    EXPECT_EQ(cfg.benchmark.exitAfterFrames, 600u);
    EXPECT_FALSE(cfg.settings.get(options::render::ssao));
    EXPECT_TRUE(cfg.settings.get(options::render::taa));
    EXPECT_EQ(cfg.render.tonemap, core::TonemapOperator::Reinhard);
    EXPECT_EQ(cfg.settings.source(settings::Id::render_msaaSamples), settings::Source::Cli);
    EXPECT_EQ(cfg.settings.origin(settings::Id::render_msaaSamples), "--msaa");
}

/**
 * The startup sequence, through the doors `Engine::init` actually opens and in the order
 * it opens them: the file, then `Game::configure`, then the flags.
 *
 * `Engine.cpp` is not hosted, so what this pins is the half that is: given that order, a
 * game's default loses to the file, a game's override beats it, and the command line beats
 * both. The middle call stands in for `configure`, which is all `configure` does to a
 * setting -- it is handed this same `Settings&`.
 */
TEST_F(ConfigTest, TheStartupSequenceIsTheFileThenTheGameThenTheFlags) {
    const fs::path p = write("substrate.json", R"({
        "render": {"msaaSamples": 8, "bloomStrength": 0.2},
        "window": {"vsync": true}
    })");

    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(p));

    // What `Game::configure` gets to do, and both doors of it.
    EXPECT_FALSE(cfg.settings.setDefault(options::render::msaaSamples, 2u)) << "the file claimed this row";
    EXPECT_TRUE(cfg.settings.setDefault(options::render::bloomThreshold, 2.0f)) << "and did not claim this one";
    EXPECT_TRUE(cfg.settings.set(options::window::vsync, false)) << "an override is not a default";

    int exitCode = -1;
    Argv args({"substrate", "--set", "render.msaaSamples=1", "--set", "render.bloomThreshold=3", "--set",
               "window.vsync=true"});
    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));

    // The user's file, where the game only offered a default.
    EXPECT_FLOAT_EQ(cfg.settings.get(options::render::bloomStrength), 0.2f);
    EXPECT_EQ(cfg.settings.source(settings::Id::render_bloomStrength), settings::Source::Config);

    // The command line, over a game default and over a game override alike.
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 1u);
    EXPECT_EQ(cfg.settings.source(settings::Id::render_msaaSamples), settings::Source::Cli);
    EXPECT_FLOAT_EQ(cfg.settings.get(options::render::bloomThreshold), 3.0f);
    EXPECT_EQ(cfg.settings.source(settings::Id::render_bloomThreshold), settings::Source::Cli);
    EXPECT_TRUE(cfg.settings.get(options::window::vsync));
    EXPECT_EQ(cfg.settings.source(settings::Id::window_vsync), settings::Source::Cli);
    EXPECT_EQ(cfg.settings.origin(settings::Id::window_vsync), "--set");
}

// ------------------------------------------------------------------------ --set
//
// One door onto every row the table holds. What is checked here is coverage -- each of
// the six row types, reached by key -- and that the refusals are the ones the table and
// `setFromString` already owns rather than a second policy this branch invented.

TEST_F(ConfigTest, SetReachesEveryTypeInTheTable) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate",
               "--set", "render.ssao=false",                        // Bool
               "--set", "window.width=1280",                        // Int
               "--set", "render.msaaSamples=8",                     // Uint
               "--set", "audio.decodeBudgetBytes=1048576",          // Uint64
               "--set", "render.fogHeightFalloff=12",               // Float
               "--set", "render.debugFont=res:/mono.ttf"});         // String

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_FALSE(cfg.settings.get(options::render::ssao));
    EXPECT_EQ(cfg.settings.get(options::window::width), 1280);
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 8u);
    EXPECT_EQ(cfg.settings.get(options::audio::decodeBudgetBytes), 1048576u);
    EXPECT_FLOAT_EQ(cfg.settings.get(options::render::fogHeightFalloff), 12.0f);
    EXPECT_EQ(cfg.settings.get(options::render::debugFont), "res:/mono.ttf");
}

TEST_F(ConfigTest, SetRecordsCliProvenanceAndNamesTheDoor) {
    // The dump's last two columns are why it exists, and a value that arrived through
    // `--set` has to be as traceable as one that arrived through a named flag.
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--set", "render.bloomStrength=0.2", "--set", "audio.masterVolume=0.5"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_EQ(cfg.settings.source(settings::Id::render_bloomStrength), settings::Source::Cli);
    EXPECT_EQ(cfg.settings.origin(settings::Id::render_bloomStrength), "--set");
    EXPECT_EQ(cfg.settings.source(settings::Id::audio_masterVolume), settings::Source::Cli)
        << "a second row through the same door reports the same way";
    EXPECT_EQ(cfg.settings.origin(settings::Id::audio_masterVolume), "--set");
}

TEST_F(ConfigTest, SetRefusesTheKeysTheTableAlreadyRefuses) {
    // No policy of its own: an unknown key and an `engine.` key are refused by
    // `setFromString`, with the messages it already had. The branch parses nothing.
    Config cfg;
    const std::string sceneBefore = cfg.settings.get(options::engine::current_scene_path);

    int exitCode = -1;
    Argv args({"substrate", "--set", "render.noSuchThing=1", "--set", "engine.current_scene_path=res:/nope.gltf",
               "--set", "render.msaaSamples=8"});
    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));

    EXPECT_EQ(cfg.settings.get(options::engine::current_scene_path), sceneBefore)
        << "loading a scene is not assigning a string";
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 8u) << "and the rest of the line still applies";
}

TEST_F(ConfigTest, SetWithoutAnEqualsChangesNothingAndDoesNotSwallowTheNextFlag) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--set", "render.ssao", "--set", "--msaa", "2"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_TRUE(cfg.settings.get(options::render::ssao)) << "a key with no value assigns nothing";
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 4u)
        << "`--set --msaa` consumed --msaa as its argument, so 2 is a bare token, not a sample count";
}

TEST_F(ConfigTest, SetSplitsOnTheFirstEqualsSoAValueMayHoldOne) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--set", "render.debugFont=a=b.ttf"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_EQ(cfg.settings.get(options::render::debugFont), "a=b.ttf");
}

TEST_F(ConfigTest, TheSameKeyTwiceIsLastWins) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--set", "render.msaaSamples=2", "--set", "render.msaaSamples=8"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 8u);
}

TEST_F(ConfigTest, ANameThatIsNotOneIsRefusedAtTheOneDoorLeft) {
    // This used to drive `--tonemap x` and `--set render.tonemap=x` against each other,
    // because they were one setting reached through two doors and a rule that differed by
    // door would be a rule nobody would remember. D14 removed the row, so `--set`
    // has no name-valued key to reach and there is exactly one door: the flag. What the
    // case pins now is the half that survives -- the refusal leaves the previous value
    // standing, and `--set` reports the departed key rather than assigning anything.
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--tonemap", "reinhard", "--tonemap", "reinhardt", "--set", "render.tonemap=clamp"});
    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));

    EXPECT_EQ(cfg.render.tonemap, core::TonemapOperator::Reinhard) << "the typo takes neither the value nor the door";
    EXPECT_EQ(cfg.settings.find("render.tonemap"), settings::Id::None) << "--set found no row to assign";

    // And an alias is still an input spelling: `none` is `clamp`.
    Config alias;
    Argv aliasArgs({"substrate", "--tonemap", "none"});
    ASSERT_TRUE(alias.applyCommandLine(aliasArgs.argc(), aliasArgs.argv(), exitCode));
    EXPECT_EQ(alias.render.tonemap, core::TonemapOperator::Clamp);
}

// The deletions, pinned. A flag that came back as a no-op would otherwise be invisible:
// `--vsync` would warn once into a log nobody reads and the run would carry on with the
// wrong value, which is the exact failure mode D12 spent a card removing.
TEST_F(ConfigTest, TheRetiredFlagsAreNotSilentlyStillThere) {
    for (const char* retired : {"--vsync", "--ui-scale", "--stream-threshold", "--bloom-strength", "--ssr-roughness",
                                "--taa-blend", "--rt-distance", "--particle-budget", "--body-budget",
                                "--physics-threads", "--lod-threshold"}) {
        Config cfg;
        const Config defaults;
        int exitCode = -1;
        Argv args({"substrate", retired, "1"});
        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode)) << retired;

        for (uint16_t i = 0; i < cfg.settings.rowCount(); ++i) {
            const auto id = static_cast<settings::Id>(i);
            EXPECT_EQ(cfg.settings.valueString(id), defaults.settings.valueString(id))
                << retired << " still moves " << cfg.settings.row(id).key;
        }
    }
}

// Ray tracing is preferred where the device offers it, so the row starts on and
// `--no-rt` turns it off. One row and one flag pair on purpose: the traced paths share
// their shadow queries, so a per-feature split would let a surface and its reflection
// shadow the same light two different ways. There are deliberately no
// `--no-rt-shadows` / `--no-rt-reflections` / `--no-rt-ambient` rows to add back.
TEST_F(ConfigTest, RayTracingIsOnByDefault) {
    Config cfg;
    EXPECT_TRUE(cfg.settings.get(options::render::rt));
}

TEST_F(ConfigTest, NoRtTurnsOffEveryTracedPath) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--no-rt"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_FALSE(cfg.settings.get(options::render::rt));
    EXPECT_EQ(cfg.settings.origin(settings::Id::render_rt), "--no-rt");
}

// The tuning number stays independent of the on/off switch: a config that dials the
// ray range is not thereby opting into or out of tracing.
TEST_F(ConfigTest, RtTuningFlagsAreSeparateFromTheSwitch) {
    Config cfg;
    int exitCode = -1;
    // `--rt-distance` was retired by D13: the range is a preference with a JSON key, so it
    // reaches the command line through `--set`. The switch beside it is not one.
    Argv args({"substrate", "--set", "render.rtMaxDistance=500"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_TRUE(cfg.settings.get(options::render::rt));
    EXPECT_FLOAT_EQ(cfg.settings.get(options::render::rtMaxDistance), 500.0f);
}

// The reflection march and the reflection ray are bounded by different numbers, and
// sharing one is what confined a traced reflection to an eight-metre bubble. Pinned so
// they cannot quietly be merged back.
TEST_F(ConfigTest, TheMarchBudgetAndTheRayRangeAreSeparate) {
    Config cfg;
    EXPECT_FLOAT_EQ(cfg.settings.get(options::render::ssrMaxDistance), 8.0f);
    EXPECT_FLOAT_EQ(cfg.settings.get(options::render::rtMaxDistance), 200.0f);
}

// `render.rayQuery` decided `vkCreateDevice`, so it was an `initOnly` row and a write
// after the freeze was refused with a reason. D14 answers the same question by removing
// the door instead: a device extension is not something a user configures, so it is a
// `Config` field the command line writes once, before anything reads it. The check is that
// it is unreachable from every string door rather than merely refused late by one.
TEST_F(ConfigTest, RayQueryIsACommandLineControlAndNotAKeyAtAll) {
    Config cfg;
    EXPECT_EQ(cfg.settings.find("render.rayQuery"), settings::Id::None);
    EXPECT_FALSE(cfg.settings.setFromString("render.rayQuery", "off", settings::Source::Cli, "--set"));
    EXPECT_TRUE(cfg.rayQueryAllowed()) << "a refused key changed nothing";

    int exitCode = -1;
    Argv args({"substrate", "--no-ray-query"});
    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_FALSE(cfg.rayQueryAllowed());
}

TEST_F(ConfigTest, ABarePathIsTheScene) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "game/demo/assets/lights.gltf"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_EQ(cfg.scene.path, "game/demo/assets/lights.gltf");
}

TEST_F(ConfigTest, TheSceneIsEmptyUntilSomethingNamesOne) {
    // It has no key and no default: the scene is the game's, and this field carries
    // only the per-invocation override. Empty is what tells `Engine::init` to keep the
    // game's choice.
    const Config cfg;
    EXPECT_TRUE(cfg.scene.path.empty());
    EXPECT_EQ(cfg.scene.characters, 0u);
}

TEST_F(ConfigTest, CaptureFlagsImplyAStatedFrame) {
    // A capture with no frame index is a screenshot of whichever frame the run happened
    // to end on, which is not a property of the render. The flags supply one rather than
    // demanding a second flag.
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--capture", "out.png"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_EQ(cfg.benchmark.capturePath, "out.png");
    EXPECT_EQ(cfg.benchmark.captureFrame, 60u);
}

TEST_F(ConfigTest, AnExplicitCaptureFrameIsNotOverwrittenByTheDefault) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--capture-frame", "120", "--capture", "out.png"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_EQ(cfg.benchmark.captureFrame, 120u);
}

TEST_F(ConfigTest, ACaptureRunDropsTheOverlayButNotWhenItWasAskedFor) {
    // The overlay is on by default and carries a frame counter, so a capture run that
    // drew it would produce an image that differs from every other capture in the same
    // corner whatever the renderer did -- and scripts/golden.sh would report a
    // regression on every run, which is the same as reporting none. Naming --overlay is
    // still an instruction rather than a preference, so it survives.
    {
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--capture", "out.png"});

        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_FALSE(cfg.render.debugOverlay);
    }
    {
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--capture", "out.png", "--overlay"});

        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_TRUE(cfg.render.debugOverlay);
    }
    {
        // And a run that captures nothing keeps the default, which is on.
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--msaa", "2"});

        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_TRUE(cfg.render.debugOverlay);
    }
}

// D13 retired the three percent flags with the `scale` column that served them. There is
// one representation of a 0..1 knob now -- the one the file holds and the dump prints --
// and this pins that `--set` writes it rather than a hundredth of it.
TEST_F(ConfigTest, ThePercentFlagsAreGoneAndTheValueIsTheOneTheFileHolds) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--set", "render.ssrRoughnessCutoff=0.75", "--set", "render.taaBlend=0.25", "--set",
               "ui.scale=1.5"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_FLOAT_EQ(cfg.settings.get(options::render::ssrRoughnessCutoff), 0.75f);
    EXPECT_FLOAT_EQ(cfg.settings.get(options::render::taaBlend), 0.25f);
    EXPECT_FLOAT_EQ(cfg.settings.get(options::ui::scale), 1.5f);
}

TEST_F(ConfigTest, TwoFlagsThatImplyASecondSettingStillDo) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--sync-validation", "--inspector", "--rt", "--physics-contacts"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_TRUE(cfg.render.syncValidation);
    EXPECT_EQ(cfg.render.validation, Tristate::On) << "asking for the checks implies the layer";
    EXPECT_TRUE(cfg.ui.panel) << "the inspector is placed beside the panel";
    EXPECT_TRUE(cfg.ui.inspector);
    EXPECT_TRUE(cfg.settings.get(options::render::rt));
    EXPECT_TRUE(cfg.physics.debugDraw);
    EXPECT_TRUE(cfg.physics.debugContacts);
}

TEST_F(ConfigTest, TheClockIsCommandLineOnlyAndAnUnknownOneReadsAsRealtime) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--locked"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_FALSE(cfg.physicsRealtimeClock());

    cfg.physics.clock = "realtime";
    EXPECT_TRUE(cfg.physicsRealtimeClock());
    cfg.physics.clock = "nonsense";
    EXPECT_TRUE(cfg.physicsRealtimeClock()) << "a typo falls back to the default, loudly";
}

TEST_F(ConfigTest, DumpSettingsIsRecordedRatherThanServicedHere) {
    // Serviced by Engine::init, after the game has configured itself: a dump taken any
    // earlier would report the wrong provenance for the thing being debugged.
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--dump-settings"});

    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_EQ(cfg.dumpSettings, Config::Dump::Table);

    Config json;
    Argv jsonArgs({"substrate", "--dump-settings=json"});
    ASSERT_TRUE(json.applyCommandLine(jsonArgs.argc(), jsonArgs.argv(), exitCode));
    EXPECT_EQ(json.dumpSettings, Config::Dump::Json);
}

TEST_F(ConfigTest, AnUnknownFlagWarnsAndKeepsGoing) {
    // Refusing to start over a typo in a benchmark sweep would cost more than the typo.
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--not-a-flag", "--msaa", "2"});

    EXPECT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 2u);
}

TEST_F(ConfigTest, HelpAsksTheCallerToExitCleanly) {
    Config cfg;
    int exitCode = -1;
    Argv args({"substrate", "--help"});

    testing::internal::CaptureStdout();
    const bool keepGoing = cfg.applyCommandLine(args.argc(), args.argv(), exitCode);
    const std::string out = testing::internal::GetCapturedStdout();

    EXPECT_FALSE(keepGoing);
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(out.find("usage: substrate"), std::string::npos);
}

TEST_F(ConfigTest, AFlagAtTheEndWithNoValueKeepsItsPreviousValue) {
    // The parser reads the next argv entry, and there isn't one. Every numeric flag falls
    // back to what the setting already holds; six of them used to fall back to a literal
    // restating the default instead, so one at the end of a line silently reset a
    // configured value while `--msaa` in the same position did not.
    Config cfg;
    ASSERT_TRUE(cfg.settings.set(options::window::width, 1280, settings::Source::Config));

    int exitCode = -1;
    Argv args({"substrate", "--msaa", "--width"});
    ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));

    EXPECT_EQ(cfg.settings.get(options::render::msaaSamples), 4u) << "--width is not a number and is not consumed";
    EXPECT_EQ(cfg.settings.get(options::window::width), 1280);
}

TEST_F(ConfigTest, AnInputScriptIsParsedWhereEveryOtherFlagIsRefused) {
    // Parsed by the flag rather than carried as a string, so a typo is refused while
    // the person who typed it is still looking -- and a refusal empties the script rather
    // than loading the part that parsed, because a run that pressed half a scenario
    // reports against half a scenario.
    {
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--input-script", "60:Game.Save,90:Camera.Forward+"});

        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        ASSERT_EQ(cfg.input.script.steps().size(), 3u) << "a bare action is a tap, which is two steps";
        EXPECT_EQ(cfg.input.script.lastFrame(), 90u);
    }
    {
        Config cfg;
        int exitCode = -1;
        Argv args({"substrate", "--input-script", "60:Game.Save,ninety:Camera.Forward+"});

        ASSERT_TRUE(cfg.applyCommandLine(args.argc(), args.argv(), exitCode));
        EXPECT_TRUE(cfg.input.script.empty());
    }
    {
        // And it has no JSON key at all: it is a developer control by the same test
        // --locked is, so a config file cannot script the person running the program.
        Config cfg;
        const fs::path p = write("scripted.json", R"({"input": {"script": "60:Game.Save"}})");
        ASSERT_TRUE(cfg.loadFromFile(p));
        EXPECT_TRUE(cfg.input.script.empty());
    }
}
