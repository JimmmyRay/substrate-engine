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
 * @brief Rects, layout, hit testing, focus and widgets (S6).
 *
 * ## Immediate mode, and why that is the shape this engine wants
 *
 * There is no widget tree, no `Widget` base class and no `virtual void draw()`. A widget
 * is a **function call that both draws and answers**: `if (ui.button("Reload")) ...`.
 * The whole of what persists between frames is three integers -- which item the pointer
 * is over, which one it is holding, and which one has the keyboard -- plus one scroll
 * offset per panel.
 *
 * That is not a stylistic preference borrowed from elsewhere. Simplicity rule 3 rules out
 * interfaces and virtual dispatch, and a retained widget tree is the single most reliable
 * way a codebase acquires both: a base class with `draw`, `layout` and `onClick`, a
 * parent pointer, a child vector, and an event that has to walk all of it. Immediate mode
 * deletes the tree rather than abstracting over it -- the call stack *is* the hierarchy,
 * for exactly as long as the frame takes.
 *
 * The cost is stated rather than hidden: the layout of frame N+1 can depend on the
 * content of frame N. Two places pay it -- a panel's scrollbar and a list's extent both
 * size from what the previous frame measured -- and both are noted where they happen.
 *
 * ## What draws it
 *
 * Nothing here knows what Vulkan is. `Context` fills a `DrawList` of pixel-space vertices
 * and clip rectangles, and `gfx::Renderer` uploads them into the buffer the debug overlay
 * already had, through the pipeline the overlay already had. That is the same division
 * `renderer.debugLines` (S4.5) and `renderer.overlayLines` (S1.4) draw, and it is why
 * this whole file is in the hosted set with the unit suite able to reach it.
 *
 * A rectangle costs no new pipeline: `overlay.frag` multiplies a vertex colour by the R8
 * atlas, so a quad pointed at the solid block `FontMetrics::whiteU/whiteV` reserves *is*
 * a filled rect. See the note in FontMetrics.h.
 */
namespace ui {

// ------------------------------------------------------------------ draw list (S6.1)

/**
 * @brief Append one line of text as two triangles per glyph.
 *
 * A free function over a plain vector rather than a method, because it has two callers
 * that want different things around it: `DrawList::text`, which then has to account the
 * vertices to a clip command, and the renderer's debug HUD, which has no clipping at all.
 * Written twice it would be two places for the pen convention -- baseline origin, y down
 * -- to drift from `overlay.vert`.
 *
 * The pen starts on the baseline at (x, baselineY) and moves right. Glyphs outside ASCII
 * 32..126 are skipped rather than substituted.
 */
void appendText(std::vector<DrawVertex>& out, const FontMetrics& font, float x, float baselineY,
                const std::string& value, uint32_t rgba);

/// A run of vertices sharing one scissor rectangle. One per clip change, which in
/// practice is one per panel plus one per scrolling list.
struct DrawCommand {
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    /// x0, y0, x1, y1 in pixels, origin top-left. Already intersected with every clip
    /// above it, so the renderer sets the scissor to this and asks no questions.
    glm::vec4 clip{0.0f, 0.0f, 0.0f, 0.0f};
};

/**
 * @brief Vertices and their clip ranges, rebuilt from scratch every frame.
 *
 * Rebuilt rather than diffed, and that is the immediate-mode bargain: a few thousand
 * vertices a frame is a memcpy into a mapped buffer, and it removes every question about
 * what is stale.
 */
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
    /// Draw the image in descriptor slot `texture` over `r`, tinted by `rgba` (C5).
    /// White leaves the image alone; anything else multiplies, which is what makes one
    /// greyscale icon serve as an enabled and a disabled one.
    ///
    /// A slot rather than a `gfx::ImageId`, and the split is deliberate: this is the
    /// vertex builder, and the slot is what a vertex holds. Resolving a handle is
    /// `Context::image`'s job, because that is where a stale one has somewhere to be
    /// refused.
    void image(const glm::vec4& r, uint32_t texture, uint32_t rgba = 0xFFFFFFFFu);
    /// A one-pixel-thick frame drawn as four rects. Four rather than a line list because
    /// the line pipeline is a different pipeline, and this one is already bound.
    void rectOutline(const glm::vec4& r, uint32_t rgba, float thickness = 1.0f);
    /// The pen starts on the baseline at (x, baselineY) and moves right.
    void text(const FontMetrics& font, float x, float baselineY, const std::string& value, uint32_t rgba);

    [[nodiscard]] const std::vector<DrawVertex>& vertices() const { return verts; }
    [[nodiscard]] const std::vector<DrawCommand>& commands() const { return cmds; }
    [[nodiscard]] bool empty() const { return verts.empty(); }

    /// Texcoord of the atlas's solid block, copied in by `Context::begin`. Public data
    /// rather than a setter because it is one coordinate that has to be right and there
    /// is nothing to validate: a DrawList with both at zero draws rectangles the shape of
    /// whatever glyph happens to live at the atlas origin, which is visible immediately.
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

// ------------------------------------------------------------------- theme (S6.1)

/// Every colour and every distance the widgets use. Distances are in **unscaled**
/// pixels: `Context::begin` multiplies them by the DPI scale once (S6.5), so a theme is
/// written at one size and correct at all of them.
struct Theme {
    /// **Authored in sRGB**, which is what a person picking a colour types;
    /// `overlay.frag` converts. See the note there for why the conversion is in the
    /// shader rather than in `packColor`.
    ///
    /// The alpha is 98% rather than the 91% it started at, and that is worth a line
    /// because the number is misleading on its own: the swapchain is an `_SRGB` format,
    /// so the hardware blends in **linear** space. Nine percent of a bright background in
    /// linear terms is far more than nine percent of it perceptually -- a panel measured
    /// at "91% opaque" let Sponza's marble through at what looked like half strength.
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
    /// Slider fill, checkbox tick, selected row. One accent rather than three, because
    /// three would be three constants nobody could keep in agreement.
    uint32_t accent = 0xFF3FA9F5u;
    uint32_t scrollThumb = 0xFF6A625Au;

    float padding = 6.0f;      ///< inside a panel's edge, and inside a widget's
    float spacing = 4.0f;      ///< between two widgets
    float rowHeight = 20.0f;   ///< a plain widget's height, before the font is consulted
    float border = 1.0f;
    float scrollWidth = 8.0f;
    float titleHeight = 20.0f;
};

// ------------------------------------------------------------------ input (S6.4)

/**
 * @brief The pointer and the few keys a UI needs, as plain data.
 *
 * **Filled by the application, not read from `core::input::InputMap` here.** The UI has no
 * business naming actions -- which key opens a panel is a binding a player can change,
 * and a UI that read `Key::Tab` directly would be the hardcoded-key mistake S1.1 was
 * written to end. It also makes every widget testable against synthesised input, which
 * is what the unit suite does.
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
 * Held by the application across frames, because three of its members have to survive:
 * `active` (the widget the pointer is holding, which must stay held while the pointer
 * leaves it), `focus` (the widget with the keyboard) and the per-panel scroll offsets.
 * Everything else is rebuilt in `begin`.
 */
class Context {
  public:
    /**
     * @brief Start a frame.
     *
     * @param scale DPI scale (S6.5). Multiplies every distance in the theme and is the
     *        *only* place resolution independence is handled -- a widget that wrote a
     *        pixel count of its own would be the thing that breaks at 200%.
     */
    void begin(const InputState& in, float width, float height, const FontMetrics& font, float scale = 1.0f);
    void end();

    /// The one field a text widget needs and cannot own: S1.5's editable buffer. Set once
    /// at startup. Without it `textField` still draws and still takes focus; it just
    /// cannot be typed into, and says so once.
    void setTextInput(core::input::TextInput* edit) { text = edit; }

    /// Where `image` turns a `gfx::ImageId` into the slot a vertex carries (P1). Set once
    /// at startup, by whoever owns the table. Without it every image draws the font
    /// atlas, which is the same degradation a stale handle gets.
    void setImages(const gfx::ImageTable* table) { imageTable = table; }

    // ------------------------------------------------------------ panels (S6.2)
    /**
     * @brief Open a panel at `pos` with `size`, in pixels.
     *
     * @return false when the panel is collapsed or off screen, in which case **no widget
     *         call between here and `endPanel` draws anything** -- but they must still be
     *         made, and `endPanel` must still be called. That is the immediate-mode
     *         convention and it is the one thing about this API that surprises people:
     *         the `if` guards the work, not the structure.
     */
    bool beginPanel(const std::string& title, const glm::vec2& pos, const glm::vec2& size);
    void endPanel();

    // ------------------------------------------------------------ layout (S6.2)
    /**
     * @brief Split the next `columns` widgets across the content width.
     *
     * **Flow, not constraints**, and the roadmap called this the decision that shapes
     * everything after it. A constraint solver buys alignment across containers that do
     * not know about each other, and costs a solver plus a dependency graph plus a
     * relayout pass. Nothing in a settings panel or an inspector needs that: those are
     * columns of rows, and rows of equal columns. The rule is one paragraph long --
     * widgets stack downward, a row places them left to right, and every container knows
     * its own width from its parent -- which means a widget can be written without
     * knowing what contains it.
     */
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
     *
     * The single place layout happens, which is what keeps "how does a widget know where
     * it is" from being answered nine different ways.
     */
    glm::vec4 allocate(float height = -1.0f);

    // ----------------------------------------------------------- widgets (S6.3)
    void label(const std::string& value);
    void labelDim(const std::string& value);
    /// Right-aligned within the allocated rectangle. What a value column is.
    void labelRight(const std::string& value);

    /**
     * @brief An image from `Engine::images()`, laid out like any other widget (C5, P1).
     *
     * @param id      from `gfx::ImageTable::load`. A handle that was never issued, or was
     *                destroyed, draws the font atlas rather than crashing or aliasing onto
     *                whatever took the slot -- that refusal is the whole reason this takes
     *                a handle instead of the slot it used to.
     * @param height  in **unscaled** pixels, like every theme distance. Negative means
     *                one theme row, so `image(logo)` is a row-high icon.
     * @param aspect  width / height. The rectangle is `height * aspect` wide, clamped to
     *                the container, because a UI is laid out in rows and the row is what
     *                is fixed. Non-positive fills the container's width instead.
     * @param rgba    multiplied over the image; white leaves it alone.
     *
     * No hit test and no return value: this is the `label` of images. A clickable one is
     * `button` beside it, and giving every image a hover state would make a decorative
     * one swallow the pointer.
     */
    void image(gfx::ImageId id, float height = -1.0f, float aspect = -1.0f, uint32_t rgba = 0xFFFFFFFFu);

    /// True on the frame it is released with the pointer still inside it -- not on press.
    /// Pressing and dragging away is how a user changes their mind, and a button that
    /// fired on press cannot be changed about.
    bool button(const std::string& caption);
    bool checkbox(const std::string& caption, bool& value);
    /// Returns true on any frame the value changed.
    bool slider(const std::string& caption, float& value, float min, float max);
    /// Integer variant, snapped as it drags. A wrapper rather than a second widget: the
    /// visual, the hit test and the drag are the same thing at a different rounding.
    bool sliderInt(const std::string& caption, int& value, int min, int max);

    /**
     * @brief An editable string (S6.3, over S1.5's model).
     *
     * Focus is the whole of what makes this more than a label: on gaining it, the field
     * loads `value` into the shared `TextInput` and takes the keyboard; on Enter or on
     * losing it, it writes back; on Escape it does not.
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

    // ----------------------------------------------------------- routing (S6.4)
    /// True when the pointer is over any panel, or a widget is being dragged. The
    /// application tests this before orbiting the camera: a slider drag that also spun
    /// the view would be the first thing anybody noticed.
    [[nodiscard]] bool wantsPointer() const { return pointerCaptured || active != kNoId; }
    /// True while a text field has focus. The application feeds this to
    /// `InputMap::setTextMode`, which is the generalisation S6.4 owed S1.5 -- that call
    /// had exactly one consumer and now has a routing layer above it.
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

    /// What a panel remembers between frames. Two floats and an id; the whole of
    /// immediate mode's retained state, alongside `hot`, `active` and `focus`.
    struct PanelState {
        Id id = kNoId;
        float scroll = 0.0f;
        /// Measured last frame. A scrollbar cannot know its extent until the content has
        /// been laid out, and the content is laid out after the bar is drawn -- so it is
        /// one frame behind, which is invisible at 60 Hz and stated here rather than
        /// discovered.
        float contentHeight = 0.0f;
    };

    [[nodiscard]] Frame& frame() { return frames.back(); }
    [[nodiscard]] const Frame& frame() const { return frames.back(); }
    /// Hash of `label` mixed with the enclosing frame's id, so two widgets with the same
    /// caption in two panels are two widgets -- and the same caption in the same place is
    /// the same widget between frames, which is what makes focus survive at all.
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
    /// P1's lifetime check, borrowed rather than owned: `Engine` holds the table.
    const gfx::ImageTable* imageTable = nullptr;

    std::vector<Frame> frames;
    std::vector<PanelState> panels;

    glm::vec2 screen{0.0f};
    float dpiScale = 1.0f;

    // --------------------------------------------------------- retained (S6.4)
    Id hot = kNoId;    ///< under the pointer this frame
    Id active = kNoId; ///< held by the pointer, across frames until release
    Id focus = kNoId;  ///< has the keyboard
    bool focusIsText = false;
    bool pointerCaptured = false;

    /// Focus traversal, resolved at `end`. Every focusable widget in the order it was
    /// drawn, which is the only definition of "next" a flow layout has, paired with
    /// whether it is a text field -- because `focusIsText` has to move with the focus and
    /// the widget that will receive it has already returned by then.
    struct Focusable {
        Id id = kNoId;
        bool isText = false;
    };
    std::vector<Focusable> focusOrder;
    int focusStep = 0; ///< -1, 0 or +1, from Tab and Shift-Tab

    /// The field the shared `TextInput` is currently holding, or `kNoId`. Distinct from
    /// `focus` because the editor has to be *opened* exactly once on the frame focus
    /// arrives and *committed* exactly once on the frame it leaves -- and both of those
    /// are transitions, not states.
    Id textEditing = kNoId;
    /// The value the field held when editing started, so Escape has something to restore.
    std::string textSeed;
    float caretBlink = 0.0f;
};

} // namespace ui
