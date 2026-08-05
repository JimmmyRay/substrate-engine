#pragma once

#include "core/Input.h"
#include "gfx/ImageTable.h"
#include "ui/FontMetrics.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file Ui.h
 * @brief Rects, layout, hit testing, focus and widgets.
 *
 * Immediate mode: the only state crossing a frame boundary is `hot`, `active`, `focus` and one
 * scroll offset per panel. The cost is that frame N+1's layout can depend on frame N's content
 * -- a panel's scrollbar and a list's extent both do, and both say so where they happen.
 *
 * Nothing here names Vulkan; a `DrawList` of pixel-space vertices comes out and `gfx::Renderer`
 * uploads it. Include a Vulkan header and this file leaves the hosted set with the unit suite.
 */
namespace ui {

/**
 * @brief Append one line of text as two triangles per glyph.
 *
 * The pen starts on the baseline at (x, baselineY) and moves right, y down -- the convention
 * `overlay.vert` is written against. Glyphs outside ASCII 32..126 are skipped, not substituted.
 */
void appendText(std::vector<DrawVertex>& out, const FontMetrics& font, float x, float baselineY,
                const std::string& value, uint32_t rgba);

/// A run of vertices sharing one scissor rectangle.
struct DrawCommand {
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    /// x0, y0, x1, y1 in pixels, origin top-left. Already intersected with every clip
    /// above it, so the renderer sets the scissor to this and asks no questions.
    glm::vec4 clip{0.0f, 0.0f, 0.0f, 0.0f};
};

/// @brief Vertices and their clip ranges, rebuilt from scratch every frame.
class DrawList {
  public:
    /// Start a frame. `fullClip` is the screen, and it is the bottom of the clip stack.
    void reset(const glm::vec4& fullClip);
    /// Close the trailing command. Called by `Context::end`; a caller filling a DrawList
    /// by hand has to call it before reading `commands()`.
    void finish();

    /// Intersected with whatever is already on the stack, so a clip can only ever shrink
    /// -- a widget cannot draw outside the panel that contains it by pushing a bigger
    /// rectangle.
    void pushClip(const glm::vec4& rect);
    void popClip();
    [[nodiscard]] const glm::vec4& clip() const { return clipStack.back(); }

    void rect(const glm::vec4& r, uint32_t rgba);
    /// Draw the image in descriptor slot `texture` over `r`, tinted by `rgba`. White leaves the
    /// image alone; anything else multiplies. Takes a slot, not a handle -- `Context::image`
    /// resolves, which is where a stale handle has somewhere to be refused.
    void image(const glm::vec4& r, uint32_t texture, uint32_t rgba = 0xFFFFFFFFu);
    /// A one-pixel-thick frame drawn as four rects. A line list would cost a pipeline switch;
    /// this one is already bound.
    void rectOutline(const glm::vec4& r, uint32_t rgba, float thickness = 1.0f);
    /// The pen starts on the baseline at (x, baselineY) and moves right.
    void text(const FontMetrics& font, float x, float baselineY, const std::string& value, uint32_t rgba);

    [[nodiscard]] const std::vector<DrawVertex>& vertices() const { return verts; }
    [[nodiscard]] const std::vector<DrawCommand>& commands() const { return cmds; }
    [[nodiscard]] bool empty() const { return verts.empty(); }

    /// Texcoord of the atlas's solid block, copied in by `Context::begin`. Left at zero, every
    /// rectangle draws the shape of whatever glyph happens to sit at the atlas origin.
    float whiteU = 0.0f;
    float whiteV = 0.0f;

  private:
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t rgba,
              uint32_t texture = kFontAtlasSlot);
    /// Close the current command and open one at the current clip. A no-op when the
    /// current command is empty, so pushing two clips in a row does not emit a zero-draw.
    void breakCommand();

    std::vector<DrawVertex> verts;
    std::vector<DrawCommand> cmds;
    std::vector<glm::vec4> clipStack;
};

/// Every colour and every distance the widgets use. Distances are in **unscaled** pixels;
/// `Context::begin` multiplies them by the DPI scale once.
struct Theme {
    /// Authored in sRGB, which is what a person picking a colour types; `overlay.frag`
    /// converts. Alpha reads far more opaque than it is: the swapchain is `_SRGB`, so the
    /// hardware blends in linear space and 91% opacity let Sponza's marble through at what
    /// looked like half strength.
    uint32_t panelBackground = 0xFA1C1917u;
    uint32_t panelBorder = 0xFF4A423Cu;
    uint32_t titleBackground = 0xFF2E2822u;
    uint32_t text = 0xFFEAE4DCu;
    uint32_t textDim = 0xFF9A928Au;
    uint32_t textDisabled = 0xFF6A625Au;
    uint32_t widgetBackground = 0xFF322C26u;
    uint32_t widgetHover = 0xFF453D35u;
    uint32_t widgetActive = 0xFF564C42u;
    uint32_t widgetBorder = 0xFF564C42u;
    /// Slider fill, checkbox tick, selected row.
    uint32_t accent = 0xFF3FA9F5u;
    uint32_t scrollThumb = 0xFF6A625Au;

    float padding = 6.0f;      ///< inside a panel's edge, and inside a widget's
    float spacing = 4.0f;      ///< between two widgets
    float rowHeight = 20.0f;   ///< a plain widget's height, before the font is consulted
    float border = 1.0f;
    float scrollWidth = 8.0f;
    float titleHeight = 20.0f;
};

/**
 * @brief The pointer and the few keys a UI needs, as plain data.
 *
 * Filled by the application; reading `core::input::InputMap` from here would hardcode keys a
 * player can rebind, and would take every widget out of reach of the unit suite's synthesised
 * input.
 */
struct InputState {
    glm::vec2 mouse{0.0f};
    bool mouseDown = false;     ///< held this frame
    bool mousePressed = false;  ///< the edge, this frame
    bool mouseReleased = false; ///< the edge, this frame
    float scroll = 0.0f;        ///< wheel notches, positive away from the user

    // Focus traversal and list navigation. Edges, not held states.
    bool tab = false;
    bool shift = false;
    bool enter = false;
    bool escape = false;
    bool up = false;
    bool down = false;

    float dt = 0.0f; ///< seconds, for the text cursor blink
};

/// Identity of one widget within one frame. Zero is "nothing".
using Id = uint32_t;
constexpr Id kNoId = 0;

/**
 * @brief One frame of UI: layout, hit testing, focus and the vertices that come out.
 *
 * Must be held by the application across frames. Rebuild it each frame and `active` (the widget
 * the pointer is holding while it leaves the rectangle), `focus` and the per-panel scroll
 * offsets are all lost; everything else `begin` rebuilds anyway.
 */
class Context {
  public:
    /**
     * @brief Start a frame.
     *
     * @param scale DPI scale, applied to every theme distance here and nowhere else. A widget
     *        writing a pixel count of its own is what breaks at 200%.
     */
    void begin(const InputState& in, float width, float height, const FontMetrics& font, float scale = 1.0f);
    void end();

    /// The shared editable buffer. Set once at startup; without it `textField` still draws and
    /// still takes focus, but cannot be typed into, and says so once.
    void setTextInput(core::input::TextInput* edit) { text = edit; }

    /// What `image` resolves a `gfx::ImageId` against. Set once at startup; without it every
    /// image draws the font atlas, the same degradation a stale handle gets.
    void setImages(const gfx::ImageTable* table) { imageTable = table; }

    /**
     * @brief Open a panel at `pos` with `size`, in pixels.
     *
     * @return false when the panel is collapsed or off screen. The widget calls between here
     *         and `endPanel` must still be made and `endPanel` must still be called -- the `if`
     *         guards the work, not the structure. Skipping them leaves the frame stack unbalanced.
     */
    bool beginPanel(const std::string& title, const glm::vec2& pos, const glm::vec2& size);
    void endPanel();

    /// @brief Split the next `columns` widgets across the content width.
    void beginRow(uint32_t columns);
    void endRow();

    /// Vertical gap. Negative uses the theme's spacing.
    void spacing(float pixels = -1.0f);
    /// A full-width rule, with the theme's spacing above and below it.
    void separator();

    /**
     * @brief Reserve the next widget's rectangle. Every widget goes through this.
     *
     * @param height rows high, in scaled pixels; negative means one theme row.
     */
    glm::vec4 allocate(float height = -1.0f);

    void label(const std::string& value);
    void labelDim(const std::string& value);
    /// Right-aligned within the allocated rectangle.
    void labelRight(const std::string& value);

    /**
     * @brief An image from `Engine::images()`, laid out like any other widget.
     *
     * @param id      from `gfx::ImageTable::load`. A handle never issued or since destroyed
     *                draws the font atlas rather than aliasing onto whatever took the slot.
     * @param height  in **unscaled** pixels, like every theme distance. Negative means one
     *                theme row.
     * @param aspect  width / height. The rectangle is `height * aspect` wide, clamped to the
     *                container. Non-positive fills the container's width instead.
     * @param rgba    multiplied over the image; white leaves it alone.
     */
    void image(gfx::ImageId id, float height = -1.0f, float aspect = -1.0f, uint32_t rgba = 0xFFFFFFFFu);

    /// True on the frame it is released with the pointer still inside it, never on press --
    /// pressing and dragging away is how a user changes their mind.
    bool button(const std::string& caption);
    bool checkbox(const std::string& caption, bool& value);
    /// Returns true on any frame the value changed.
    bool slider(const std::string& caption, float& value, float min, float max);
    /// Integer variant, snapped as it drags.
    bool sliderInt(const std::string& caption, int& value, int min, int max);

    /**
     * @brief An editable string.
     *
     * On gaining focus the field loads `value` into the shared `TextInput` and takes the
     * keyboard; on Enter or on losing focus it writes back; on Escape it does not.
     *
     * @return true on the frame `value` changed.
     */
    bool textField(const std::string& caption, std::string& value);

    /**
     * @brief A scrolling list of selectable rows.
     *
     * @param height in scaled pixels; the list scrolls when its rows do not fit.
     * @return true on the frame the selection changed.
     */
    bool list(const std::string& caption, const std::vector<std::string>& items, uint32_t& selected, float height);

    /// True when the pointer is over any panel, or a widget is being dragged. The application
    /// must test this before orbiting the camera, or a slider drag also spins the view.
    [[nodiscard]] bool wantsPointer() const { return pointerCaptured || active != kNoId; }
    /// True while a text field has focus. The application feeds this to `InputMap::setTextMode`.
    [[nodiscard]] bool wantsKeyboard() const { return focus != kNoId && focusIsText; }

    [[nodiscard]] const DrawList& draw() const { return drawList; }
    [[nodiscard]] float scale() const { return dpiScale; }
    /// Theme distances after the DPI scale, which is what a caller sizing a panel wants.
    [[nodiscard]] const Theme& scaled() const { return metrics; }

    /// Written at any time; takes effect at the next `begin`.
    Theme theme;

  private:
    struct Frame {
        glm::vec4 bounds{0.0f}; ///< the region this level lays out inside
        glm::vec2 cursor{0.0f}; ///< next widget's top-left
        uint32_t columns = 0;   ///< 0 is vertical flow; N is a row of N equal columns
        uint32_t column = 0;
        float rowHeight = 0.0f; ///< tallest widget placed in the row being filled
        Id id = kNoId;
    };

    /// What a panel remembers between frames.
    struct PanelState {
        Id id = kNoId;
        float scroll = 0.0f;
        /// Measured last frame: the bar is drawn before the content it measures, so this is
        /// one frame behind and a panel whose content changes size scrolls a frame late.
        float contentHeight = 0.0f;
    };

    [[nodiscard]] Frame& frame() { return frames.back(); }
    [[nodiscard]] const Frame& frame() const { return frames.back(); }
    /// Hash of `label` mixed with the enclosing frame's id. The same caption in the same place
    /// must hash the same every frame or focus and drags cannot survive one; mixing in anything
    /// that varies frame to frame -- an index, a value -- breaks both silently.
    [[nodiscard]] Id makeId(const std::string& label) const;
    [[nodiscard]] bool hitTest(const glm::vec4& r) const;
    /// The hot/active protocol, in one place: returns whether the pointer is over `r`,
    /// and updates `hot` and `active` accordingly.
    bool behave(Id id, const glm::vec4& r, bool& pressed, bool& released);
    /// Background colour for a widget in its current state.
    [[nodiscard]] uint32_t stateColor(Id id, bool hovered) const;
    void drawCaption(const glm::vec4& r, const std::string& value, uint32_t rgba);
    [[nodiscard]] PanelState& panelState(Id id);
    /// Move focus to the next or previous focusable widget. Recorded during the frame and
    /// applied at `end`, because "the next one" is not known until the frame is over.
    void applyFocusChange();

    DrawList drawList;
    Theme metrics; ///< `theme` with every distance multiplied by `dpiScale`
    InputState input;
    const FontMetrics* font = nullptr;
    core::input::TextInput* text = nullptr;
    /// Borrowed; `Engine` owns the table.
    const gfx::ImageTable* imageTable = nullptr;

    std::vector<Frame> frames;
    std::vector<PanelState> panels;

    glm::vec2 screen{0.0f};
    float dpiScale = 1.0f;

    Id hot = kNoId;    ///< under the pointer this frame
    Id active = kNoId; ///< held by the pointer, across frames until release
    Id focus = kNoId;  ///< has the keyboard
    bool focusIsText = false;
    bool pointerCaptured = false;

    /// Every focusable widget in draw order, which is the only definition of "next" a flow
    /// layout has. `isText` rides along because `focusIsText` moves with the focus and the
    /// widget about to receive it has already returned by the time `end` runs.
    struct Focusable {
        Id id = kNoId;
        bool isText = false;
    };
    std::vector<Focusable> focusOrder;
    int focusStep = 0; ///< -1, 0 or +1, from Tab and Shift-Tab

    /// The field the shared `TextInput` is currently holding, or `kNoId`. Collapsing this into
    /// `focus` loses the two transitions it exists for: open once on the frame focus arrives,
    /// commit once on the frame it leaves.
    Id textEditing = kNoId;
    /// The value the field held when editing started, so Escape has something to restore.
    std::string textSeed;
    float caretBlink = 0.0f;
};

} // namespace ui
