#include "ui/BindingMenu.h"

#include "core/Input.h"
#include "core/Logger.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace core;

namespace fs = std::filesystem;
using namespace input;

/**
 * @file tests/BindingMenuTests.cpp
 * @brief The rebind menu, which is where S1.1, S1.2 and S1.5 meet.
 *
 * The menu produces lines of text rather than draw calls, which is what makes this
 * file possible at all: every decision it makes -- what is selected, what the filter
 * matches, whose turn it is to own the keyboard -- is checkable without a device.
 *
 * The interesting cases are the handovers. While the menu is open the text field owns
 * the keyboard; while a capture is armed *nothing* does, including the field, or the
 * key being bound gets typed into the filter on its way past.
 */

namespace {

/// Runs one frame the way main.cpp does: resolve, then let the menu look.
void frame(InputMap& map, TextInput& text, ui::BindingMenu& menu, float dt = 0.016f) {
    map.beginFrame();
    text.update(dt);
    menu.update(map, text);
}

/// A map holding a handful of actions with recognisable names.
InputMap populated() {
    InputMap map;
    map.declare("Camera.Forward", "W");
    map.declare("Camera.Back", "S");
    map.declare("Toggle.Ssao", "F8");
    map.declare("Toggle.Bloom", "F10");
    return map;
}

bool anyLineContains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

/// The row the cursor is on, or empty if there is none.
std::string selectedRow(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        if (line.rfind("> ", 0) == 0) return line;
    }
    return {};
}

} // namespace

TEST(BindingMenuTest, ClosedItDrawsNothingAndLeavesTheKeyboardAlone) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);

    frame(map, text, menu);
    EXPECT_FALSE(menu.open());
    EXPECT_TRUE(menu.lines().empty()) << "an empty list is what tells the renderer there is nothing to draw";
    EXPECT_FALSE(map.textMode());
    EXPECT_FALSE(text.active());
}

TEST(BindingMenuTest, TabOpensItAndTheFieldTakesTheKeyboard) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);

    EXPECT_TRUE(menu.open());
    EXPECT_TRUE(map.textMode());
    EXPECT_TRUE(text.active());
    EXPECT_TRUE(anyLineContains(menu.lines(), "Camera.Forward"));
    EXPECT_TRUE(anyLineContains(menu.lines(), "F8"));
}

TEST(BindingMenuTest, TabClosesItAgainDespiteTextModeSuppressingKeys) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);
    map.onKey(Key::Tab, false);
    frame(map, text, menu);
    map.onKey(Key::Tab, true);
    frame(map, text, menu);

    EXPECT_FALSE(menu.open()) << "the toggle is exempt precisely so this works";
    EXPECT_FALSE(map.textMode());
    EXPECT_TRUE(menu.lines().empty());
}

TEST(BindingMenuTest, EscapeClosesItThroughTheField) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);

    text.onKey(Key::Escape, true);
    frame(map, text, menu);
    EXPECT_FALSE(menu.open());
}

TEST(BindingMenuTest, TypingFiltersTheListAndSaysHowMuchIsShown) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);

    for (const char c : std::string("ssao")) text.onChar(static_cast<uint32_t>(c));
    frame(map, text, menu);

    EXPECT_TRUE(anyLineContains(menu.lines(), "Toggle.Ssao"));
    EXPECT_FALSE(anyLineContains(menu.lines(), "Camera.Forward"));
    EXPECT_TRUE(anyLineContains(menu.lines(), "1-1 of 1")) << "the window is stated, not implied";
}

TEST(BindingMenuTest, ARetiredActionLeavesTheListing) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);
    map.retire(map.find("Toggle.Ssao"));

    map.onKey(Key::Tab, true);
    frame(map, text, menu);

    // The menu walks `actionCount()`, which is the table size, so the row has to be dropped
    // here: offering to rebind an action nothing resolves offers a control that does nothing.
    EXPECT_FALSE(anyLineContains(menu.lines(), "Toggle.Ssao"));
    EXPECT_TRUE(anyLineContains(menu.lines(), "Camera.Forward"));
    EXPECT_TRUE(anyLineContains(menu.lines(), "1-4 of 4")) << "four left of the five declared";
}

TEST(BindingMenuTest, TheFilterIsCaseInsensitiveAndReportsNoMatch) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);

    for (const char c : std::string("CAMERA")) text.onChar(static_cast<uint32_t>(c));
    frame(map, text, menu);
    EXPECT_TRUE(anyLineContains(menu.lines(), "1-2 of 2"));

    for (const char c : std::string("zzz")) text.onChar(static_cast<uint32_t>(c));
    frame(map, text, menu);
    EXPECT_TRUE(anyLineContains(menu.lines(), "no action matches"));
}

TEST(BindingMenuTest, SelectionMovesAndStopsAtTheEnds) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);
    EXPECT_NE(selectedRow(menu.lines()).find("Camera.Forward"), std::string::npos);

    map.onKey(Key::Up, true);
    frame(map, text, menu);
    EXPECT_NE(selectedRow(menu.lines()).find("Camera.Forward"), std::string::npos) << "the top is a wall";

    map.onKey(Key::Up, false);
    map.onKey(Key::Down, true);
    frame(map, text, menu);
    EXPECT_NE(selectedRow(menu.lines()).find("Camera.Back"), std::string::npos);

    for (int i = 0; i < 20; ++i) {
        map.onKey(Key::Down, false);
        map.onKey(Key::Down, true);
        frame(map, text, menu);
    }
    EXPECT_NE(selectedRow(menu.lines()).find("Menu.Bindings"), std::string::npos) << "the last row, not past it";
}

TEST(BindingMenuTest, ALongListPagesRatherThanTruncating) {
    InputMap map;
    for (int i = 0; i < 30; ++i) map.declare("Action." + std::to_string(i), "W");

    TextInput text;
    ui::BindingMenu menu;
    menu.visibleRows = 5;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);
    EXPECT_TRUE(anyLineContains(menu.lines(), "1-5 of 31"));

    map.onKey(Key::Tab, false);
    map.onKey(Key::PageDown, true);
    frame(map, text, menu);
    EXPECT_TRUE(anyLineContains(menu.lines(), "6-10 of 31")) << "a paged list states its window; a truncated one lies";
}

TEST(BindingMenuTest, TheWindowFollowsTheSelectionOffTheBottom) {
    InputMap map;
    for (int i = 0; i < 10; ++i) map.declare("Action." + std::to_string(i), "W");

    TextInput text;
    ui::BindingMenu menu;
    menu.visibleRows = 3;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);
    ASSERT_TRUE(anyLineContains(menu.lines(), "1-3 of 11"));

    map.onKey(Key::Tab, false);
    for (int i = 0; i < 3; ++i) {
        map.onKey(Key::Down, false);
        map.onKey(Key::Down, true);
        frame(map, text, menu);
    }

    // Without this the cursor walks past the bottom of the window and the selected row
    // stops being drawn at all -- a menu you can move around in but cannot see.
    EXPECT_TRUE(anyLineContains(menu.lines(), "2-4 of 11"));
    EXPECT_NE(selectedRow(menu.lines()).find("Action.3"), std::string::npos);
}

TEST(BindingMenuTest, EnterArmsACaptureAndTheFieldLetsGoOfTheKeyboard) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);

    text.onKey(Key::Enter, true);
    frame(map, text, menu);

    EXPECT_TRUE(map.capturing());
    EXPECT_FALSE(text.active()) << "otherwise the key being bound is typed into the filter on its way past";
    EXPECT_FALSE(map.textMode());
    EXPECT_TRUE(anyLineContains(menu.lines(), "press a control for Camera.Forward"));
}

TEST(BindingMenuTest, TheCapturedKeyBecomesTheBindingAndTheMenuSaysSo) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);
    text.onKey(Key::Enter, true);
    frame(map, text, menu);

    map.onKey(Key::K, true);
    frame(map, text, menu);

    EXPECT_FALSE(map.capturing());
    EXPECT_EQ(map.bindingList(map.find("Camera.Forward")), "K");
    EXPECT_TRUE(anyLineContains(menu.lines(), "Camera.Forward -> K"));
    // The asterisk is how a glance tells a rebind from a default.
    EXPECT_NE(selectedRow(menu.lines()).find('*'), std::string::npos);
    EXPECT_TRUE(text.active()) << "the field takes the keyboard back once the capture is done";
}

TEST(BindingMenuTest, EscapeDuringACaptureCancelsTheCaptureRatherThanClosingTheMenu) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);

    map.onKey(Key::Tab, true);
    frame(map, text, menu);
    text.onKey(Key::Enter, true);
    frame(map, text, menu);

    map.onKey(Key::Escape, true);
    frame(map, text, menu);

    EXPECT_FALSE(map.capturing());
    EXPECT_TRUE(menu.open()) << "one Escape, one meaning: the innermost thing that is listening";
    EXPECT_EQ(map.bindingList(map.find("Camera.Forward")), "W");
}

TEST(BindingMenuTest, F5RestoresTheDefault) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);
    map.setBindings(map.find("Camera.Forward"), "K");

    map.onKey(Key::Tab, true);
    frame(map, text, menu);
    map.onKey(Key::Tab, false);
    map.onKey(Key::F5, true);
    frame(map, text, menu);

    EXPECT_EQ(map.bindingList(map.find("Camera.Forward")), "W");
    EXPECT_TRUE(anyLineContains(menu.lines(), "reset to W"));
}

TEST(BindingMenuTest, F2WritesTheReboundActionsToTheConfigFile) {
    const fs::path dir = fs::temp_directory_path() / "substrate_menu_tests";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "substrate.json";
    {
        std::ofstream out(file);
        out << R"({"window": {"width": 4242}})";
    }

    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);
    menu.configPath = file.string();
    map.setBindings(map.find("Toggle.Ssao"), "K");

    map.onKey(Key::Tab, true);
    frame(map, text, menu);
    map.onKey(Key::Tab, false);
    map.onKey(Key::F2, true);
    frame(map, text, menu);

    std::ifstream in(file);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string written = buffer.str();

    EXPECT_NE(written.find("Toggle.Ssao"), std::string::npos);
    EXPECT_NE(written.find("4242"), std::string::npos) << "the rest of the config survives the save";
    EXPECT_TRUE(anyLineContains(menu.lines(), "saved to"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(BindingMenuTest, AnUnboundActionSaysSoRatherThanShowingAnEmptyColumn) {
    InputMap map = populated();
    TextInput text;
    ui::BindingMenu menu;
    menu.declareActions(map);
    map.clearBindings(map.find("Camera.Forward"));

    map.onKey(Key::Tab, true);
    frame(map, text, menu);
    EXPECT_TRUE(anyLineContains(menu.lines(), "<unbound>"));
}
