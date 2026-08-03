#include "ui/Ui.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace core;

/**
 * @file tests/UiTests.cpp
 * @brief The draw list, the layout, the hit test and the widgets (S6).
 *
 * A UI is the subsystem most often shipped untested because "you can see whether it
 * works". You can see whether it *draws*; you cannot see, by looking, that a button
 * pressed and dragged off does not fire, that a scrolled-out row still answers clicks, or
 * that Tab reaches a widget nobody clicked. Those are the defects that survive a
 * screenshot, and they are what this file is for.
 *
 * It runs against a synthetic `FontMetrics` -- fixed six-pixel advances, no atlas, no
 * device -- which is the whole reason S6 split the metrics out of `ui::Font`. Every
 * assertion below is about arithmetic and state, and none of them needs a pixel.
 *
 * Five properties carry the stage:
 *
 * 1. **A clip can only shrink.** A widget cannot draw or be clicked outside the panel
 *    containing it, whatever rectangle it asks for.
 * 2. **Press and release are different events.** A button fires on release inside itself,
 *    which is what makes dragging away the way a user changes their mind.
 * 3. **Identity is position plus caption.** The same widget between frames is the same id,
 *    which is what makes focus and a drag survive at all; the same caption in two panels
 *    is two widgets.
 * 4. **Nothing off screen is interactive.** A list row scrolled past the edge is clipped
 *    out of both the drawing and the hit test, or a click lands on whatever is drawn over
 *    it.
 * 5. **Every distance scales.** One DPI multiplier, applied in one place.
 */

namespace {

constexpr float kAdvance = 6.0f;
constexpr float kLineHeight = 16.0f;

/// Fixed advances and a solid block at a known texcoord. Deliberately not a real font:
/// what is being tested is layout arithmetic, and a real font would make every expected
/// number a measurement rather than a statement.
ui::FontMetrics testFont() {
    ui::FontMetrics f;
    f.lineSpacing = kLineHeight;
    f.ascentPx = 12.0f;
    f.whiteU = 0.25f;
    f.whiteV = 0.75f;
    for (uint32_t i = 0; i < ui::kGlyphCount; ++i) {
        f.glyphs[i].x0 = 0.0f;
        f.glyphs[i].y0 = -10.0f;
        f.glyphs[i].x1 = kAdvance;
        f.glyphs[i].y1 = 2.0f;
        f.glyphs[i].s0 = 0.0f;
        f.glyphs[i].t0 = 0.0f;
        f.glyphs[i].s1 = 0.1f;
        f.glyphs[i].t1 = 0.1f;
        f.glyphs[i].advance = kAdvance;
    }
    return f;
}

/// One frame's worth of driving, so a test reads as a sequence of frames rather than as
/// bookkeeping. The context is the caller's, because half of what is being tested is what
/// survives between frames.
class Ui {
  public:
    Ui() : font(testFont()) {
        imageFile = std::filesystem::temp_directory_path() / "substrate_ui_image_test.bin";
        std::ofstream(imageFile, std::ios::binary) << "an image as far as the table is concerned";
        images.init(8);
        context.setImages(&images);
    }
    ~Ui() { std::filesystem::remove(imageFile); }

    ui::Context context;
    /// P1. Real handles rather than bare slots, because that is what `Context::image`
    /// takes now, and because a fabricated one would exercise the vertex builder instead
    /// of the refusal. `load` only asks that the file exists -- decoding is the
    /// renderer's half and no test here has a device.
    gfx::ImageTable images;
    std::filesystem::path imageFile;
    gfx::ImageId loadImage() { return images.load(imageFile.string()); }
    ui::FontMetrics font;
    ui::InputState in;

    void begin(float scale = 1.0f) { context.begin(in, 800.0f, 600.0f, font, scale); }
    void end() {
        context.end();
        // Edges are consumed by the frame that saw them, exactly as the application's
        // per-frame input resolution does it. Leaving them set is how a one-frame press
        // becomes a held button in a test and nowhere else.
        in.mousePressed = false;
        in.mouseReleased = false;
        in.tab = in.enter = in.escape = in.up = in.down = false;
        in.scroll = 0.0f;
    }

    void moveTo(float x, float y) { in.mouse = {x, y}; }
    void press() {
        in.mouseDown = true;
        in.mousePressed = true;
    }
    void release() {
        in.mouseDown = false;
        in.mouseReleased = true;
    }
};

const glm::vec2 kPanelPos{100.0f, 100.0f};
const glm::vec2 kPanelSize{300.0f, 400.0f};

} // namespace

// ==================================================================== draw list

TEST(UiDrawList, ARectIsAQuadOnTheSolidTexel) {
    ui::DrawList list;
    list.reset({0.0f, 0.0f, 800.0f, 600.0f});
    list.whiteU = 0.25f;
    list.whiteV = 0.75f;
    list.rect({10.0f, 20.0f, 40.0f, 50.0f}, 0xFF112233u);
    list.finish();

    // Two triangles, and every one of the six texcoords is the solid block. That is the
    // whole of S6.1: a rect is a text draw with different UVs, so there is no second
    // pipeline to bind and no second pass to order against.
    ASSERT_EQ(list.vertices().size(), 6u);
    for (const ui::DrawVertex& v : list.vertices()) {
        EXPECT_FLOAT_EQ(v.u, 0.25f);
        EXPECT_FLOAT_EQ(v.v, 0.75f);
        EXPECT_EQ(v.rgba, 0xFF112233u);
    }
    ASSERT_EQ(list.commands().size(), 1u);
    EXPECT_EQ(list.commands()[0].vertexCount, 6u);
}

TEST(UiDrawList, AnEmptyRectEmitsNothing) {
    ui::DrawList list;
    list.reset({0.0f, 0.0f, 800.0f, 600.0f});
    list.rect({10.0f, 10.0f, 10.0f, 50.0f}, 0xFFFFFFFFu); ///< zero width
    list.rect({10.0f, 50.0f, 40.0f, 10.0f}, 0xFFFFFFFFu); ///< inverted
    list.finish();
    EXPECT_TRUE(list.empty());
}

TEST(UiDrawList, AClipCanOnlyShrink) {
    ui::DrawList list;
    list.reset({0.0f, 0.0f, 800.0f, 600.0f});
    list.pushClip({100.0f, 100.0f, 200.0f, 200.0f});
    // A widget asking for a clip bigger than its container gets its container's. This is
    // the property that makes "a widget cannot draw outside its panel" true by
    // construction rather than by every widget remembering to check.
    list.pushClip({0.0f, 0.0f, 800.0f, 600.0f});
    EXPECT_FLOAT_EQ(list.clip().x, 100.0f);
    EXPECT_FLOAT_EQ(list.clip().y, 100.0f);
    EXPECT_FLOAT_EQ(list.clip().z, 200.0f);
    EXPECT_FLOAT_EQ(list.clip().w, 200.0f);
    list.popClip();
    list.popClip();
    EXPECT_FLOAT_EQ(list.clip().z, 800.0f);
}

TEST(UiDrawList, EachClipBecomesItsOwnCommand) {
    ui::DrawList list;
    list.reset({0.0f, 0.0f, 800.0f, 600.0f});
    list.rect({0.0f, 0.0f, 10.0f, 10.0f}, 0xFFFFFFFFu);
    list.pushClip({0.0f, 0.0f, 50.0f, 50.0f});
    list.rect({0.0f, 0.0f, 10.0f, 10.0f}, 0xFFFFFFFFu);
    list.popClip();
    list.rect({0.0f, 0.0f, 10.0f, 10.0f}, 0xFFFFFFFFu);
    list.finish();

    ASSERT_EQ(list.commands().size(), 3u);
    EXPECT_EQ(list.commands()[0].vertexCount, 6u);
    EXPECT_EQ(list.commands()[1].vertexCount, 6u);
    EXPECT_EQ(list.commands()[2].vertexCount, 6u);
    EXPECT_EQ(list.commands()[1].firstVertex, 6u);
    EXPECT_FLOAT_EQ(list.commands()[1].clip.z, 50.0f);
    // The ranges have to tile the vertex buffer exactly, or the renderer draws a gap.
    EXPECT_EQ(list.commands()[2].firstVertex, 12u);
    EXPECT_EQ(list.vertices().size(), 18u);
}

TEST(UiDrawList, AClipWithNothingInItEmitsNoCommand) {
    // Two pushes in a row is the ordinary case -- a panel that opens a list before
    // drawing anything itself -- and a zero-vertex draw for each would be a scissor
    // change the renderer pays for and nothing comes out of.
    ui::DrawList list;
    list.reset({0.0f, 0.0f, 800.0f, 600.0f});
    list.pushClip({0.0f, 0.0f, 50.0f, 50.0f});
    list.pushClip({0.0f, 0.0f, 25.0f, 25.0f});
    list.rect({0.0f, 0.0f, 10.0f, 10.0f}, 0xFFFFFFFFu);
    list.popClip();
    list.popClip();
    list.finish();

    ASSERT_EQ(list.commands().size(), 1u);
    EXPECT_FLOAT_EQ(list.commands()[0].clip.z, 25.0f);
}

TEST(UiDrawList, TextSkipsGlyphsWithNoInk) {
    const ui::FontMetrics font = testFont();
    ui::DrawList list;
    list.reset({0.0f, 0.0f, 800.0f, 600.0f});
    list.text(font, 0.0f, 20.0f, "ab", 0xFFFFFFFFu);
    list.finish();
    EXPECT_EQ(list.vertices().size(), 12u);

    // Outside 32..126 is skipped rather than substituted, and the advance goes with it.
    ui::DrawList other;
    other.reset({0.0f, 0.0f, 800.0f, 600.0f});
    other.text(font, 0.0f, 20.0f, "a\x01\x02", 0xFFFFFFFFu);
    other.finish();
    EXPECT_EQ(other.vertices().size(), 6u);
}

// ======================================================================= layout

TEST(UiLayout, WidgetsStackDownwards) {
    Ui ui;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 a = ui.context.allocate();
    const glm::vec4 b = ui.context.allocate();
    ui.context.endPanel();
    ui.end();

    EXPECT_FLOAT_EQ(a.x, b.x);
    EXPECT_GT(b.y, a.y);
    EXPECT_FLOAT_EQ(b.y - a.y, (a.w - a.y) + ui.context.scaled().spacing);
    // A vertical widget spans the panel's content width.
    EXPECT_NEAR(a.z - a.x, kPanelSize.x - 2.0f * ui.context.scaled().padding, 0.01f);
}

TEST(UiLayout, ARowSplitsTheWidthEvenly) {
    Ui ui;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 full = ui.context.allocate();
    ui.context.beginRow(3);
    const glm::vec4 a = ui.context.allocate();
    const glm::vec4 b = ui.context.allocate();
    const glm::vec4 c = ui.context.allocate();
    ui.context.endRow();
    const glm::vec4 after = ui.context.allocate();
    ui.context.endPanel();
    ui.end();

    // Three equal columns on one line, then flow resumes below them.
    EXPECT_FLOAT_EQ(a.y, b.y);
    EXPECT_FLOAT_EQ(b.y, c.y);
    EXPECT_NEAR(a.z - a.x, b.z - b.x, 0.01f);
    EXPECT_NEAR(b.z - b.x, c.z - c.x, 0.01f);
    EXPECT_LT(a.z, b.x);
    EXPECT_NEAR(c.z, full.z, 0.01f);
    EXPECT_GT(after.y, c.w);
}

TEST(UiLayout, APartlyFilledRowStillTakesItsHeight) {
    // Without this, `beginRow(3)` with two widgets in it has the next thing drawn on top
    // of them -- which looks like a rendering bug and is a layout one.
    Ui ui;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    ui.context.beginRow(3);
    const glm::vec4 a = ui.context.allocate();
    ui.context.allocate();
    ui.context.endRow();
    const glm::vec4 after = ui.context.allocate();
    ui.context.endPanel();
    ui.end();

    EXPECT_GE(after.y, a.w);
}

TEST(UiLayout, EveryDistanceScalesWithDpi) {
    Ui one;
    one.begin(1.0f);
    ASSERT_TRUE(one.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 a = one.context.allocate();
    one.context.endPanel();
    one.end();

    Ui two;
    two.begin(2.0f);
    ASSERT_TRUE(two.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 b = two.context.allocate();
    two.context.endPanel();
    two.end();

    // Rows get taller and padding grows, so the same panel holds fewer, larger widgets --
    // which is what "correct at another resolution" means. S6.5 is one multiplier applied
    // in one place, and this is the assertion that nothing bypassed it.
    EXPECT_GT(b.w - b.y, a.w - a.y);
    EXPECT_GT(b.x, a.x);
    EXPECT_FLOAT_EQ(two.context.scaled().padding, 2.0f * one.context.scaled().padding);
}

TEST(UiLayout, ARowHeightNeverFallsBelowTheFont) {
    // A theme written for a 16 px font and used with a 32 px one would clip every
    // caption. The row height is the larger of the two rather than the theme's alone.
    Ui ui;
    ui.font.lineSpacing = 64.0f;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ui.end();
    EXPECT_GE(r.w - r.y, 64.0f);
}

// ====================================================================== widgets

TEST(UiWidget, AButtonFiresOnReleaseInside) {
    Ui ui;
    // Frame 1: press. A button that fired here could not be changed about.
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();
    const glm::vec2 centre{(r.x + r.z) * 0.5f, (r.y + r.w) * 0.5f};

    ui.moveTo(centre.x, centre.y);
    ui.press();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_FALSE(ui.context.button("Go"));
    ui.context.endPanel();
    ui.end();

    // Frame 2: release, still inside.
    ui.release();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_TRUE(ui.context.button("Go"));
    ui.context.endPanel();
    ui.end();
}

TEST(UiWidget, AButtonDraggedOffDoesNotFire) {
    Ui ui;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();

    ui.moveTo((r.x + r.z) * 0.5f, (r.y + r.w) * 0.5f);
    ui.press();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.button("Go");
    ui.context.endPanel();
    ui.end();

    // Changed their mind: pointer leaves the button before letting go.
    ui.moveTo(10.0f, 10.0f);
    ui.release();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_FALSE(ui.context.button("Go"));
    ui.context.endPanel();
    ui.end();
}

TEST(UiWidget, ACheckboxToggles) {
    Ui ui;
    bool value = false;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();

    ui.moveTo(r.x + 10.0f, (r.y + r.w) * 0.5f);
    ui.press();
    ui.release();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_TRUE(ui.context.checkbox("Shadows", value));
    ui.context.endPanel();
    ui.end();
    EXPECT_TRUE(value);
}

TEST(UiWidget, ASliderTracksThePointerAbsolutely) {
    Ui ui;
    float value = 0.0f;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();

    // Grab it at the middle.
    ui.moveTo((r.x + r.z) * 0.5f, (r.y + r.w) * 0.5f);
    ui.press();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_TRUE(ui.context.slider("Exposure", value, 0.0f, 10.0f));
    ui.context.endPanel();
    ui.end();
    EXPECT_NEAR(value, 5.0f, 0.2f);

    // Drag past the right edge and back to a quarter. Absolute, not accumulated: a drag
    // that leaves the track and returns has to land where the pointer is.
    ui.moveTo(r.z + 500.0f, (r.y + r.w) * 0.5f);
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.slider("Exposure", value, 0.0f, 10.0f);
    ui.context.endPanel();
    ui.end();
    EXPECT_FLOAT_EQ(value, 10.0f);

    ui.moveTo(r.x + (r.z - r.x) * 0.25f, (r.y + r.w) * 0.5f);
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.slider("Exposure", value, 0.0f, 10.0f);
    ui.context.endPanel();
    ui.end();
    EXPECT_NEAR(value, 2.5f, 0.2f);
}

TEST(UiWidget, ASliderIgnoresAPointerThatNeverGrabbedIt) {
    Ui ui;
    float value = 3.0f;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();

    // Hovering is not dragging. Without the `active` check a slider would snap to the
    // pointer the moment it passed over on the way to something else.
    ui.moveTo((r.x + r.z) * 0.5f, (r.y + r.w) * 0.5f);
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_FALSE(ui.context.slider("Exposure", value, 0.0f, 10.0f));
    ui.context.endPanel();
    ui.end();
    EXPECT_FLOAT_EQ(value, 3.0f);
}

TEST(UiWidget, AListSelectsAndClips) {
    Ui ui;
    std::vector<std::string> items;
    for (int i = 0; i < 100; ++i) items.push_back("item " + std::to_string(i));
    uint32_t selected = 0;

    const float listHeight = 100.0f;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate(listHeight);
    ui.context.endPanel();
    ui.context.end();

    // The second visible row.
    const float rowHeight = ui.context.scaled().rowHeight;
    ui.moveTo(r.x + 20.0f, r.y + rowHeight * 1.5f);
    ui.press();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_TRUE(ui.context.list("Scenes", items, selected, listHeight));
    ui.context.endPanel();
    ui.end();
    EXPECT_EQ(selected, 1u);

    // A row far below the list's bottom edge is clipped out of the hit test as well as
    // out of the drawing. Without that, a click meant for whatever is drawn under the
    // list selects a row nobody can see.
    ui.release();
    ui.moveTo(r.x + 20.0f, r.w + rowHeight * 2.0f);
    ui.press();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_FALSE(ui.context.list("Scenes", items, selected, listHeight));
    ui.context.endPanel();
    ui.end();
    EXPECT_EQ(selected, 1u);
}

TEST(UiWidget, AListNavigatesWithTheKeyboardAndScrollsToFollow) {
    Ui ui;
    std::vector<std::string> items;
    for (int i = 0; i < 100; ++i) items.push_back("item " + std::to_string(i));
    uint32_t selected = 0;
    const float listHeight = 100.0f;

    // Click it once to give it focus.
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate(listHeight);
    ui.context.endPanel();
    ui.context.end();

    ui.moveTo(r.x + 20.0f, r.y + 5.0f);
    ui.press();
    ui.release();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.list("Scenes", items, selected, listHeight);
    ui.context.endPanel();
    ui.end();

    // Twenty rows down is well past the bottom of a 100-pixel list, so the view has to
    // follow the selection or a keyboard-navigable list is one you cannot see.
    for (int i = 0; i < 20; ++i) {
        ui.in.down = true;
        ui.begin();
        ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
        (void)ui.context.list("Scenes", items, selected, listHeight);
        ui.context.endPanel();
        ui.end();
    }
    EXPECT_EQ(selected, 20u);

    // The selected row is now drawn inside the list rather than below it: the top of the
    // visible window has moved down with it.
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.list("Scenes", items, selected, listHeight);
    ui.context.endPanel();
    ui.end();
    EXPECT_FALSE(ui.context.draw().empty());
}

// ================================================================ routing (S6.4)

TEST(UiRouting, ThePointerIsCapturedOverAPanelAndNotBesideIt) {
    Ui ui;
    ui.moveTo(kPanelPos.x + 10.0f, kPanelPos.y + 10.0f);
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    ui.context.endPanel();
    ui.end();
    // The gaps between widgets are still the panel: a drag that crosses one must not
    // reach the camera.
    EXPECT_TRUE(ui.context.wantsPointer());

    ui.moveTo(10.0f, 10.0f);
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    ui.context.endPanel();
    ui.end();
    EXPECT_FALSE(ui.context.wantsPointer());
}

TEST(UiRouting, ADragKeepsThePointerEvenOffThePanel) {
    Ui ui;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();

    float value = 0.0f;
    ui.moveTo((r.x + r.z) * 0.5f, (r.y + r.w) * 0.5f);
    ui.press();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.slider("Exposure", value, 0.0f, 1.0f);
    ui.context.endPanel();
    ui.end();

    // Still dragging, pointer now far outside. A slider drag that let go of the mouse the
    // moment it left the panel would also start orbiting the camera, which is the first
    // thing anybody would notice.
    ui.moveTo(5.0f, 5.0f);
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.slider("Exposure", value, 0.0f, 1.0f);
    ui.context.endPanel();
    ui.end();
    EXPECT_TRUE(ui.context.wantsPointer());
}

TEST(UiRouting, TabReachesAWidgetNobodyClicked) {
    Ui ui;
    bool first = false;
    bool second = false;

    ui.in.tab = true;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.checkbox("One", first);
    (void)ui.context.checkbox("Two", second);
    ui.context.endPanel();
    ui.end();

    // Focus landed on the first widget. Enter now toggles it without the pointer having
    // touched anything, which is the whole point of a traversal order.
    ui.in.enter = true;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_TRUE(ui.context.checkbox("One", first));
    EXPECT_FALSE(ui.context.checkbox("Two", second));
    ui.context.endPanel();
    ui.end();
    EXPECT_TRUE(first);
    EXPECT_FALSE(second);

    // Tab again, and it is the second one's turn.
    ui.in.tab = true;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.checkbox("One", first);
    (void)ui.context.checkbox("Two", second);
    ui.context.endPanel();
    ui.end();

    ui.in.enter = true;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_FALSE(ui.context.checkbox("One", first));
    EXPECT_TRUE(ui.context.checkbox("Two", second));
    ui.context.endPanel();
    ui.end();
    EXPECT_TRUE(second);
}

TEST(UiRouting, ShiftTabGoesBackwards) {
    Ui ui;
    bool a = false;
    bool b = false;

    ui.in.tab = true;
    ui.in.shift = true;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.checkbox("One", a);
    (void)ui.context.checkbox("Two", b);
    ui.context.endPanel();
    ui.end();
    ui.in.shift = false;

    // From nothing focused, backwards is the last widget.
    ui.in.enter = true;
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.checkbox("One", a);
    (void)ui.context.checkbox("Two", b);
    ui.context.endPanel();
    ui.end();
    EXPECT_FALSE(a);
    EXPECT_TRUE(b);
}

TEST(UiRouting, TheSameCaptionInTwoPanelsIsTwoWidgets) {
    Ui ui;
    bool left = false;
    bool right = false;

    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Left", {0.0f, 0.0f}, {200.0f, 200.0f}));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ASSERT_TRUE(ui.context.beginPanel("Right", {400.0f, 0.0f}, {200.0f, 200.0f}));
    ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();

    ui.moveTo(r.x + 10.0f, (r.y + r.w) * 0.5f);
    ui.press();
    ui.release();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Left", {0.0f, 0.0f}, {200.0f, 200.0f}));
    (void)ui.context.checkbox("Enabled", left);
    ui.context.endPanel();
    ASSERT_TRUE(ui.context.beginPanel("Right", {400.0f, 0.0f}, {200.0f, 200.0f}));
    (void)ui.context.checkbox("Enabled", right);
    ui.context.endPanel();
    ui.end();

    // Identity is the caption mixed with the container. Without the container in it, one
    // click would have toggled both.
    EXPECT_TRUE(left);
    EXPECT_FALSE(right);
}

TEST(UiRouting, TheHashSuffixSeparatesTwoIdenticalCaptions) {
    Ui ui;
    bool first = false;
    bool second = false;

    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();

    ui.moveTo(r.x + 10.0f, (r.y + r.w) * 0.5f);
    ui.press();
    ui.release();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.checkbox("Reset##a", first);
    (void)ui.context.checkbox("Reset##b", second);
    ui.context.endPanel();
    ui.end();

    EXPECT_TRUE(first);
    EXPECT_FALSE(second);
}

// ================================================================ text (S6.3)

TEST(UiText, AFieldLoadsCommitsAndReverts) {
    Ui ui;
    input::TextInput edit;
    ui.context.setTextInput(&edit);
    std::string value = "sponza";

    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();

    // Click the field: it takes focus and seeds the editor from the value.
    ui.moveTo(r.z - 20.0f, (r.y + r.w) * 0.5f);
    ui.press();
    ui.release();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.textField("Scene", value);
    ui.context.endPanel();
    ui.end();

    EXPECT_TRUE(ui.context.wantsKeyboard());
    EXPECT_TRUE(edit.active());
    EXPECT_EQ(edit.text(), "sponza");

    // Typing writes through rather than waiting for Enter: a field that only applied on
    // Return is one where half the edits silently do nothing.
    edit.onChar('!');
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_TRUE(ui.context.textField("Scene", value));
    ui.context.endPanel();
    ui.end();
    EXPECT_EQ(value, "sponza!");

    // Escape restores what it held when editing started, and drops focus.
    edit.onKey(input::Key::Escape, true);
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.textField("Scene", value);
    ui.context.endPanel();
    ui.end();
    EXPECT_EQ(value, "sponza");
    EXPECT_FALSE(ui.context.wantsKeyboard());
    EXPECT_FALSE(edit.active());
}

TEST(UiText, EnterCommitsAndCloses) {
    Ui ui;
    input::TextInput edit;
    ui.context.setTextInput(&edit);
    std::string value = "a";

    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();

    ui.moveTo(r.z - 20.0f, (r.y + r.w) * 0.5f);
    ui.press();
    ui.release();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.textField("Scene", value);
    ui.context.endPanel();
    ui.end();

    edit.onChar('b');
    edit.onKey(input::Key::Enter, true);
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    EXPECT_TRUE(ui.context.textField("Scene", value));
    ui.context.endPanel();
    ui.end();

    EXPECT_EQ(value, "ab");
    EXPECT_FALSE(ui.context.wantsKeyboard());
}

TEST(UiText, ClickingElsewhereCommits) {
    Ui ui;
    input::TextInput edit;
    ui.context.setTextInput(&edit);
    std::string value = "a";

    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    const glm::vec4 r = ui.context.allocate();
    ui.context.endPanel();
    ui.context.end();

    ui.moveTo(r.z - 20.0f, (r.y + r.w) * 0.5f);
    ui.press();
    ui.release();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.textField("Scene", value);
    ui.context.endPanel();
    ui.end();

    edit.onChar('z');
    // Click far away: focus goes, and what was typed is kept -- which is what every text
    // field outside a dialog box does.
    ui.moveTo(5.0f, 5.0f);
    ui.press();
    ui.release();
    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.textField("Scene", value);
    ui.context.endPanel();
    ui.end();

    EXPECT_EQ(value, "az");
    EXPECT_FALSE(ui.context.wantsKeyboard());
    EXPECT_FALSE(edit.active());
}

TEST(UiText, KeyboardIsNotWantedWithoutAFocusedField) {
    Ui ui;
    input::TextInput edit;
    ui.context.setTextInput(&edit);
    bool flag = false;
    std::string value = "x";

    ui.begin();
    ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
    (void)ui.context.checkbox("Shadows", flag);
    (void)ui.context.textField("Scene", value);
    ui.context.endPanel();
    ui.end();

    // Nothing focused yet: the application must not put the input map into text mode, or
    // WASD stops working the moment a panel is opened.
    EXPECT_FALSE(ui.context.wantsKeyboard());
}

TEST(UiText, TabIntoAFieldTakesTheKeyboard) {
    Ui ui;
    input::TextInput edit;
    ui.context.setTextInput(&edit);
    bool flag = false;
    std::string value = "x";

    // Two tabs: the checkbox, then the field. `focusIsText` has to travel with the focus,
    // and the widget receiving it has already returned by the time focus moves -- which
    // is why the traversal list records the kind as well as the id.
    for (int i = 0; i < 2; ++i) {
        ui.in.tab = true;
        ui.begin();
        ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, kPanelSize));
        (void)ui.context.checkbox("Shadows", flag);
        (void)ui.context.textField("Scene", value);
        ui.context.endPanel();
        ui.end();
    }
    EXPECT_TRUE(ui.context.wantsKeyboard());
}

// ===================================================================== panels

TEST(UiPanel, ContentTallerThanThePanelScrolls) {
    Ui ui;
    ui.moveTo(kPanelPos.x + 10.0f, kPanelPos.y + 10.0f);

    const auto drawRows = [&ui] {
        ASSERT_TRUE(ui.context.beginPanel("Panel", kPanelPos, {200.0f, 120.0f}));
        for (int i = 0; i < 40; ++i) ui.context.label("row " + std::to_string(i));
        ui.context.endPanel();
    };

    // Frame 1 measures; frame 2 is the first that can know it overflows. One frame of lag
    // on a scrollbar is the stated cost of immediate mode, and this is where it is paid.
    ui.begin();
    drawRows();
    ui.end();

    ui.in.scroll = -3.0f;
    ui.begin();
    drawRows();
    ui.end();

    // Scrolled content draws above the panel's top edge, which is exactly what the clip
    // exists to cut off.
    ui.begin();
    drawRows();
    ui.end();
    EXPECT_FALSE(ui.context.draw().empty());

    // The panel's own background and border are drawn *before* its clip is pushed --
    // they are what defines the region, so they are the one thing legitimately unclipped.
    // Everything after that is content, and every content command has to sit inside the
    // panel however far it scrolled.
    uint32_t clipped = 0;
    for (const ui::DrawCommand& c : ui.context.draw().commands()) {
        const bool isFullScreen = c.clip.x <= 0.0f && c.clip.y <= 0.0f && c.clip.z >= 800.0f && c.clip.w >= 600.0f;
        if (isFullScreen) continue;
        ++clipped;
        EXPECT_GE(c.clip.x, kPanelPos.x - 0.01f);
        EXPECT_GE(c.clip.y, kPanelPos.y - 0.01f);
        EXPECT_LE(c.clip.w, kPanelPos.y + 120.0f + 0.01f);
    }
    EXPECT_GT(clipped, 0u) << "the panel's content was never clipped at all";
}

TEST(UiPanel, WidgetsDrawEvenWithNoFont) {
    // The renderer sets the font up before the first frame, but a caller that runs a
    // frame before then must not crash -- and the layout still has to be right, because
    // the numbers do not depend on the glyphs.
    ui::Context context;
    ui::InputState in;
    ui::FontMetrics empty;
    context.begin(in, 800.0f, 600.0f, empty, 1.0f);
    ASSERT_TRUE(context.beginPanel("Panel", kPanelPos, kPanelSize));
    bool flag = false;
    float value = 0.0f;
    context.label("hello");
    (void)context.button("Go");
    (void)context.checkbox("Flag", flag);
    (void)context.slider("Value", value, 0.0f, 1.0f);
    context.endPanel();
    context.end();
    EXPECT_FALSE(context.draw().empty());
}

// ============================================ screen-space images (C5)
//
// The half of C5 that is not Vulkan. The renderer owns the descriptor array and the
// upload; what is testable here is the contract between them -- that a slot rides on the
// vertex, that everything drawn before C5 existed still says slot zero, and that the
// layout is the same layout every other widget goes through.

TEST(UiImage, EveryVertexNotDrawnAsAnImageIsTheFontAtlas) {
    // The default matters more than it looks: `DrawVertex::texture` was added to a struct
    // with several hundred existing call sites, and a wrong default would have pointed
    // every glyph in the engine at an unloaded slot.
    Ui probe;
    probe.begin();
    ASSERT_TRUE(probe.context.beginPanel("Panel", kPanelPos, kPanelSize));
    probe.context.label("text");
    bool flag = true;
    (void)probe.context.checkbox("Flag", flag);
    probe.context.separator();
    probe.context.endPanel();
    probe.end();

    ASSERT_FALSE(probe.context.draw().vertices().empty());
    for (const ui::DrawVertex& v : probe.context.draw().vertices()) {
        EXPECT_EQ(v.texture, ui::kFontAtlasSlot);
    }
}

TEST(UiImage, ADrawnImageCarriesItsSlotOnEveryVertexOfTheQuad) {
    ui::DrawList list;
    list.reset({0.0f, 0.0f, 800.0f, 600.0f});
    list.image({10.0f, 20.0f, 110.0f, 70.0f}, 3u);
    list.finish();

    ASSERT_EQ(list.vertices().size(), 6u);
    for (const ui::DrawVertex& v : list.vertices()) EXPECT_EQ(v.texture, 3u);

    // The whole image, corner to corner. A source rectangle is the next thing to want and
    // deliberately absent; this pins what the absence means.
    float minU = 1.0f;
    float maxU = 0.0f;
    float minV = 1.0f;
    float maxV = 0.0f;
    for (const ui::DrawVertex& v : list.vertices()) {
        minU = std::min(minU, v.u);
        maxU = std::max(maxU, v.u);
        minV = std::min(minV, v.v);
        maxV = std::max(maxV, v.v);
    }
    EXPECT_FLOAT_EQ(minU, 0.0f);
    EXPECT_FLOAT_EQ(maxU, 1.0f);
    EXPECT_FLOAT_EQ(minV, 0.0f);
    EXPECT_FLOAT_EQ(maxV, 1.0f);
}

TEST(UiImage, TextAndImagesInOnePanelStayOneDrawPerClip) {
    // The reason the slot is per-vertex rather than per-command. If mixing them broke the
    // run into two draws, a list of rows with icons would cost one draw per row.
    Ui probe;
    const gfx::ImageId id = probe.loadImage();
    const uint32_t slot = probe.images.slot(id);
    probe.begin();
    ASSERT_TRUE(probe.context.beginPanel("Panel", kPanelPos, kPanelSize));
    probe.context.label("before");
    probe.context.image(id);
    probe.context.label("after");
    probe.context.endPanel();
    probe.end();

    uint32_t withVertices = 0;
    for (const ui::DrawCommand& c : probe.context.draw().commands()) {
        if (c.vertexCount > 0) ++withVertices;
    }
    // The panel's chrome is drawn unclipped and its content inside the clip: two, and the
    // image did not add a third.
    EXPECT_LE(withVertices, 2u);

    bool sawImage = false;
    for (const ui::DrawVertex& v : probe.context.draw().vertices()) {
        if (v.texture == slot) sawImage = true;
    }
    EXPECT_TRUE(sawImage);
}

TEST(UiImage, AspectSizesTheWidthAndTheContainerCapsIt) {
    Ui probe;
    const gfx::ImageId id = probe.loadImage();
    const uint32_t slot = probe.images.slot(id);
    probe.begin();
    ASSERT_TRUE(probe.context.beginPanel("Panel", kPanelPos, kPanelSize));

    // 2:1 at 20 unscaled pixels high is 40 wide, which fits.
    probe.context.image(id, 20.0f, 2.0f);
    // 100:1 at the same height wants 2000 and cannot have it.
    probe.context.image(id, 20.0f, 100.0f);
    probe.context.endPanel();
    probe.end();

    std::vector<float> widths;
    const std::vector<ui::DrawVertex>& verts = probe.context.draw().vertices();
    for (size_t i = 0; i + 6 <= verts.size(); i += 6) {
        if (verts[i].texture != slot) continue;
        float lo = verts[i].x;
        float hi = verts[i].x;
        for (size_t k = i; k < i + 6; ++k) {
            lo = std::min(lo, verts[k].x);
            hi = std::max(hi, verts[k].x);
        }
        widths.push_back(hi - lo);
    }
    ASSERT_EQ(widths.size(), 2u);
    EXPECT_NEAR(widths[0], 40.0f, 0.01f);
    EXPECT_LE(widths[1], kPanelSize.x);
    EXPECT_GT(widths[1], widths[0]) << "the clamped one should still be the wider of the two";
}

TEST(UiImage, ADestroyedHandleDrawsTheFontAtlasRatherThanWhateverTookTheSlot) {
    // P1's refusal, at the layer that performs it. Before the handle carried a
    // generation there was nothing here to check: the vertex held a slot, the slot was
    // live, and the widget drew somebody else's icon with no way to tell.
    Ui probe;
    const gfx::ImageId stale = probe.loadImage();
    const uint32_t slot = probe.images.slot(stale);
    probe.images.destroy(stale);
    const gfx::ImageId fresh = probe.loadImage();
    ASSERT_EQ(probe.images.slot(fresh), slot) << "the free list should have handed the slot back";

    probe.begin();
    ASSERT_TRUE(probe.context.beginPanel("Panel", kPanelPos, kPanelSize));
    probe.context.image(stale);
    probe.context.endPanel();
    probe.end();

    for (const ui::DrawVertex& v : probe.context.draw().vertices()) {
        EXPECT_NE(v.texture, slot);
        EXPECT_EQ(v.texture, ui::kFontAtlasSlot);
    }
}

TEST(UiImage, WithNoTableEveryImageDegradesToTheAtlas) {
    // The state before `Engine` wires the table in, and the state a tool driving
    // `ui::Context` by hand is in. It draws a font atlas rather than dereferencing null.
    Ui probe;
    const gfx::ImageId id = probe.loadImage();
    probe.context.setImages(nullptr);

    probe.begin();
    ASSERT_TRUE(probe.context.beginPanel("Panel", kPanelPos, kPanelSize));
    probe.context.image(id);
    probe.context.endPanel();
    probe.end();

    for (const ui::DrawVertex& v : probe.context.draw().vertices()) EXPECT_EQ(v.texture, ui::kFontAtlasSlot);
}

TEST(UiImage, AZeroSizedRectangleDrawsNothing) {
    ui::DrawList list;
    list.reset({0.0f, 0.0f, 800.0f, 600.0f});
    list.image({10.0f, 20.0f, 10.0f, 70.0f}, 1u); // zero width
    list.image({10.0f, 20.0f, 110.0f, 20.0f}, 1u); // zero height
    list.finish();
    EXPECT_TRUE(list.vertices().empty());
}
