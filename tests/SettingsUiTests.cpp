#include "ui/SettingsUi.h"

#include <gtest/gtest.h>

#include <string>

using namespace core;

/**
 * @file tests/SettingsUiTests.cpp
 * @brief The generated settings panel (G2), with no device and no window.
 *
 * What is worth testing here is not that widgets appear. It is the two claims the
 * generated panel makes that a screenshot cannot check:
 *
 * 1. **A row drawn as a control writes the table**, through `set`, with its provenance --
 *    not a copy of the value that agrees with it until something else moves.
 * 2. **A row nothing would apply is drawn as a readout.** A control over a value that is
 *    already spent is worse than no control: it moves, it reports success, and nothing
 *    happens, which is precisely the failure the settings table exists to remove.
 *
 * Driven by Tab and Enter rather than by clicking at coordinates, deliberately: what a
 * row's pixel position is depends on how many rows precede it, so a test written against
 * coordinates would fail the day a setting is added -- which is a change that must be
 * free. Traversal order is the table's order, and that is a property worth pinning anyway.
 */
namespace {

constexpr float kAdvance = 6.0f;

ui::FontMetrics testFont() {
    ui::FontMetrics f;
    f.lineSpacing = 16.0f;
    f.ascentPx = 12.0f;
    for (uint32_t i = 0; i < ui::kGlyphCount; ++i) {
        f.glyphs[i].x1 = kAdvance;
        f.glyphs[i].y1 = 12.0f;
        f.glyphs[i].advance = kAdvance;
    }
    return f;
}

/// One frame of a panel holding one module's rows, so a test reads as a sequence of
/// frames. The context is a member because focus is what survives between them.
class Panel {
  public:
    Panel() : font(testFont()) {}

    ui::Context context;
    ui::FontMetrics font;
    ui::InputState in;

    /// @return whether `drawSettings` wrote anything this frame.
    bool frame(settings::Settings& s, std::string_view module) {
        context.begin(in, 800.0f, 2000.0f, font);
        bool wrote = false;
        if (context.beginPanel("Settings", {10.0f, 10.0f}, {400.0f, 1800.0f})) {
            wrote = ui::drawSettings(context, s, module);
        }
        context.endPanel();
        context.end();
        in.tab = in.enter = false;
        return wrote;
    }

    uint32_t vertices() const { return static_cast<uint32_t>(context.draw().vertices().size()); }
};

} // namespace

TEST(SettingsUi, ACheckboxWritesTheTableAndSaysWhereTheValueCameFrom) {
    settings::Settings s;
    Panel panel;

    // A module holding exactly one row, so Tab reaches it and Enter toggles it without any
    // assumption about how many rows come before. It used to be `scene`, which held only
    // `scene.bakeCache` until D14 established that baking is a build step rather than a
    // preference and emptied the module. A declared row is the better fixture anyway: the
    // panel is supposed not to know whether a row is the engine's or a game's, and this
    // now checks that as a side effect of being written.
    const auto flag = s.declare("test.flag", false, "Flag");
    panel.in.tab = true;
    panel.frame(s, "test");

    panel.in.enter = true;
    EXPECT_TRUE(panel.frame(s, "test"));

    EXPECT_TRUE(s.get(flag));
    // Through `set`, which is what makes the panel one of the table's doors rather than a
    // fifth place a value lives.
    EXPECT_EQ(s.source(flag.id), settings::Source::Game);
    EXPECT_EQ(s.origin(flag.id), "panel");
}

TEST(SettingsUi, AFrozenInitOnlyRowIsAReadoutRatherThanAControl) {
    settings::Settings s;
    Panel panel;

    // The same row as above, after the thing it decides has been decided. The table
    // already refuses the write with a reason; what this pins is that the panel does not
    // offer it -- a slider over a frozen row would log that reason on every frame of a
    // drag.
    const auto flag = s.declare("test.flag", false, "Flag", 0.0, 0.0, settings::kInitOnly);
    s.freezeInitOnly();

    panel.in.tab = true;
    panel.frame(s, "test");
    panel.in.enter = true;
    EXPECT_FALSE(panel.frame(s, "test"));

    EXPECT_FALSE(s.get(flag));
    EXPECT_EQ(s.source(flag.id), settings::Source::Default);
}

TEST(SettingsUi, AnEngineOwnedRowIsAReadoutBecauseTheSetterRefusesIt) {
    settings::Settings s;
    s.setEngineOwned(settings::Id::engine_game_name, "demo");

    Panel panel;
    panel.in.tab = true;
    panel.frame(s, "engine");
    panel.in.enter = true;
    EXPECT_FALSE(panel.frame(s, "engine"));

    // It still *reports*: an engine row is readable and dumpable, and a panel that hid it
    // would be hiding the answer to "which scene is loaded".
    EXPECT_GT(panel.vertices(), 0u);
    EXPECT_EQ(s.get(options::engine::game_name), "demo");
}

TEST(SettingsUi, AModuleIsMatchedOnAWholeKeySegment) {
    settings::Settings s;

    Panel empty;
    empty.frame(s, "rend");
    const uint32_t chromeOnly = empty.vertices();

    Panel prefix;
    prefix.frame(s, "render.ssr");
    EXPECT_EQ(prefix.vertices(), chromeOnly) << "`render.ssr` must not claim `render.ssrIntensity`";

    Panel real;
    real.frame(s, "render");
    EXPECT_GT(real.vertices(), chromeOnly) << "and the module itself must draw its rows";
}

/**
 * @brief A row a game declared draws and writes exactly as an engine row does (D17).
 *
 * The claim that makes a game's setting a setting rather than a second config system: the
 * panel is generated from the table, and `declare` puts a row in the table, so a game's
 * module is drawn by the same call with a different name. Nothing in `drawSettings` learns
 * that a game exists -- the module argument is the JSON section, and `demo` is a section
 * like `render` is.
 */
TEST(SettingsUi, AModuleAGameDeclaredDrawsAndWritesLikeAnyOther) {
    settings::Settings s;
    const settings::Setting<bool> dust = s.declare("demo.impactDust", true, "Impact dust");
    ASSERT_NE(dust.id, settings::Id::None);

    Panel absent;
    absent.frame(s, "nothingDeclaresThis");
    const uint32_t chromeOnly = absent.vertices();

    Panel panel;
    panel.in.tab = true;
    panel.frame(s, "demo");
    EXPECT_GT(panel.vertices(), chromeOnly) << "the declared row is drawn at all";

    panel.in.enter = true;
    EXPECT_TRUE(panel.frame(s, "demo"));

    EXPECT_FALSE(s.get(dust)) << "and the checkbox wrote the table";
    EXPECT_EQ(s.source(dust.id), settings::Source::Game);
    EXPECT_EQ(s.origin(dust.id), "panel") << "through `set`, with the same provenance an engine row gets";
}

TEST(SettingsUi, ASliderRowIsBoundedByTheRowRatherThanByThePanel) {
    // Not a UI assertion: the range a widget is given comes from the same `minimum` and
    // `maximum` the JSON parser clamps to, so a panel cannot offer a value the config file
    // would refuse. Checked through the table because that is where the guarantee lives.
    settings::Settings s;
    const settings::Row& r = s.row(settings::Id::render_bloomThreshold);
    EXPECT_LT(r.minimum, r.maximum);

    EXPECT_TRUE(s.set(options::render::bloomThreshold, static_cast<float>(r.maximum) + 100.0f));
    EXPECT_FLOAT_EQ(s.get(options::render::bloomThreshold), static_cast<float>(r.maximum));
}
