#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core {

/**
 * @file engine/core/Input.h
 * @brief Named actions bound to physical controls, a deterministic press feed, and a
 * text field. `InputMap` resolves actions; `TextInput` accumulates characters, which is a
 * different mechanism because typing is layout- and IME-dependent.
 *
 * Nothing here includes GLFW, and the key, mouse and gamepad codes below *are* GLFW's
 * numeric values -- the window layer casts rather than translates. Every one is
 * static_asserted against the real macro in `engine/core/InputGlfw.cpp`, so changing a
 * value here without changing GLFW's is a compile error rather than a key that silently
 * stops working.
 */
namespace input {

// clang-format off
/**
 * @brief Every key this engine can name, as (enumerator, value, GLFW macro).
 *
 * The third column is expanded only by `engine/core/InputGlfw.cpp`, the one translation
 * unit that includes GLFW; expanding it anywhere else pulls GLFW into this header.
 */
#define SUBSTRATE_KEY_LIST(SUB_ENTRY)                             \
    SUB_ENTRY(Space,        32,  GLFW_KEY_SPACE)                  \
    SUB_ENTRY(Apostrophe,   39,  GLFW_KEY_APOSTROPHE)             \
    SUB_ENTRY(Comma,        44,  GLFW_KEY_COMMA)                  \
    SUB_ENTRY(Minus,        45,  GLFW_KEY_MINUS)                  \
    SUB_ENTRY(Period,       46,  GLFW_KEY_PERIOD)                 \
    SUB_ENTRY(Slash,        47,  GLFW_KEY_SLASH)                  \
    SUB_ENTRY(Num0,         48,  GLFW_KEY_0)                      \
    SUB_ENTRY(Num1,         49,  GLFW_KEY_1)                      \
    SUB_ENTRY(Num2,         50,  GLFW_KEY_2)                      \
    SUB_ENTRY(Num3,         51,  GLFW_KEY_3)                      \
    SUB_ENTRY(Num4,         52,  GLFW_KEY_4)                      \
    SUB_ENTRY(Num5,         53,  GLFW_KEY_5)                      \
    SUB_ENTRY(Num6,         54,  GLFW_KEY_6)                      \
    SUB_ENTRY(Num7,         55,  GLFW_KEY_7)                      \
    SUB_ENTRY(Num8,         56,  GLFW_KEY_8)                      \
    SUB_ENTRY(Num9,         57,  GLFW_KEY_9)                      \
    SUB_ENTRY(Semicolon,    59,  GLFW_KEY_SEMICOLON)              \
    SUB_ENTRY(Equal,        61,  GLFW_KEY_EQUAL)                  \
    SUB_ENTRY(A,            65,  GLFW_KEY_A)                      \
    SUB_ENTRY(B,            66,  GLFW_KEY_B)                      \
    SUB_ENTRY(C,            67,  GLFW_KEY_C)                      \
    SUB_ENTRY(D,            68,  GLFW_KEY_D)                      \
    SUB_ENTRY(E,            69,  GLFW_KEY_E)                      \
    SUB_ENTRY(F,            70,  GLFW_KEY_F)                      \
    SUB_ENTRY(G,            71,  GLFW_KEY_G)                      \
    SUB_ENTRY(H,            72,  GLFW_KEY_H)                      \
    SUB_ENTRY(I,            73,  GLFW_KEY_I)                      \
    SUB_ENTRY(J,            74,  GLFW_KEY_J)                      \
    SUB_ENTRY(K,            75,  GLFW_KEY_K)                      \
    SUB_ENTRY(L,            76,  GLFW_KEY_L)                      \
    SUB_ENTRY(M,            77,  GLFW_KEY_M)                      \
    SUB_ENTRY(N,            78,  GLFW_KEY_N)                      \
    SUB_ENTRY(O,            79,  GLFW_KEY_O)                      \
    SUB_ENTRY(P,            80,  GLFW_KEY_P)                      \
    SUB_ENTRY(Q,            81,  GLFW_KEY_Q)                      \
    SUB_ENTRY(R,            82,  GLFW_KEY_R)                      \
    SUB_ENTRY(S,            83,  GLFW_KEY_S)                      \
    SUB_ENTRY(T,            84,  GLFW_KEY_T)                      \
    SUB_ENTRY(U,            85,  GLFW_KEY_U)                      \
    SUB_ENTRY(V,            86,  GLFW_KEY_V)                      \
    SUB_ENTRY(W,            87,  GLFW_KEY_W)                      \
    SUB_ENTRY(X,            88,  GLFW_KEY_X)                      \
    SUB_ENTRY(Y,            89,  GLFW_KEY_Y)                      \
    SUB_ENTRY(Z,            90,  GLFW_KEY_Z)                      \
    SUB_ENTRY(LeftBracket,  91,  GLFW_KEY_LEFT_BRACKET)           \
    SUB_ENTRY(Backslash,    92,  GLFW_KEY_BACKSLASH)              \
    SUB_ENTRY(RightBracket, 93,  GLFW_KEY_RIGHT_BRACKET)          \
    SUB_ENTRY(GraveAccent,  96,  GLFW_KEY_GRAVE_ACCENT)           \
    SUB_ENTRY(Escape,       256, GLFW_KEY_ESCAPE)                 \
    SUB_ENTRY(Enter,        257, GLFW_KEY_ENTER)                  \
    SUB_ENTRY(Tab,          258, GLFW_KEY_TAB)                    \
    SUB_ENTRY(Backspace,    259, GLFW_KEY_BACKSPACE)              \
    SUB_ENTRY(Insert,       260, GLFW_KEY_INSERT)                 \
    SUB_ENTRY(Delete,       261, GLFW_KEY_DELETE)                 \
    SUB_ENTRY(Right,        262, GLFW_KEY_RIGHT)                  \
    SUB_ENTRY(Left,         263, GLFW_KEY_LEFT)                   \
    SUB_ENTRY(Down,         264, GLFW_KEY_DOWN)                   \
    SUB_ENTRY(Up,           265, GLFW_KEY_UP)                     \
    SUB_ENTRY(PageUp,       266, GLFW_KEY_PAGE_UP)                \
    SUB_ENTRY(PageDown,     267, GLFW_KEY_PAGE_DOWN)              \
    SUB_ENTRY(Home,         268, GLFW_KEY_HOME)                   \
    SUB_ENTRY(End,          269, GLFW_KEY_END)                    \
    SUB_ENTRY(CapsLock,     280, GLFW_KEY_CAPS_LOCK)              \
    SUB_ENTRY(ScrollLock,   281, GLFW_KEY_SCROLL_LOCK)            \
    SUB_ENTRY(NumLock,      282, GLFW_KEY_NUM_LOCK)               \
    SUB_ENTRY(PrintScreen,  283, GLFW_KEY_PRINT_SCREEN)           \
    SUB_ENTRY(Pause,        284, GLFW_KEY_PAUSE)                  \
    SUB_ENTRY(F1,           290, GLFW_KEY_F1)                     \
    SUB_ENTRY(F2,           291, GLFW_KEY_F2)                     \
    SUB_ENTRY(F3,           292, GLFW_KEY_F3)                     \
    SUB_ENTRY(F4,           293, GLFW_KEY_F4)                     \
    SUB_ENTRY(F5,           294, GLFW_KEY_F5)                     \
    SUB_ENTRY(F6,           295, GLFW_KEY_F6)                     \
    SUB_ENTRY(F7,           296, GLFW_KEY_F7)                     \
    SUB_ENTRY(F8,           297, GLFW_KEY_F8)                     \
    SUB_ENTRY(F9,           298, GLFW_KEY_F9)                     \
    SUB_ENTRY(F10,          299, GLFW_KEY_F10)                    \
    SUB_ENTRY(F11,          300, GLFW_KEY_F11)                    \
    SUB_ENTRY(F12,          301, GLFW_KEY_F12)                    \
    SUB_ENTRY(Kp0,          320, GLFW_KEY_KP_0)                   \
    SUB_ENTRY(Kp1,          321, GLFW_KEY_KP_1)                   \
    SUB_ENTRY(Kp2,          322, GLFW_KEY_KP_2)                   \
    SUB_ENTRY(Kp3,          323, GLFW_KEY_KP_3)                   \
    SUB_ENTRY(Kp4,          324, GLFW_KEY_KP_4)                   \
    SUB_ENTRY(Kp5,          325, GLFW_KEY_KP_5)                   \
    SUB_ENTRY(Kp6,          326, GLFW_KEY_KP_6)                   \
    SUB_ENTRY(Kp7,          327, GLFW_KEY_KP_7)                   \
    SUB_ENTRY(Kp8,          328, GLFW_KEY_KP_8)                   \
    SUB_ENTRY(Kp9,          329, GLFW_KEY_KP_9)                   \
    SUB_ENTRY(KpDecimal,    330, GLFW_KEY_KP_DECIMAL)             \
    SUB_ENTRY(KpDivide,     331, GLFW_KEY_KP_DIVIDE)              \
    SUB_ENTRY(KpMultiply,   332, GLFW_KEY_KP_MULTIPLY)            \
    SUB_ENTRY(KpSubtract,   333, GLFW_KEY_KP_SUBTRACT)            \
    SUB_ENTRY(KpAdd,        334, GLFW_KEY_KP_ADD)                 \
    SUB_ENTRY(KpEnter,      335, GLFW_KEY_KP_ENTER)               \
    SUB_ENTRY(KpEqual,      336, GLFW_KEY_KP_EQUAL)               \
    SUB_ENTRY(LeftShift,    340, GLFW_KEY_LEFT_SHIFT)             \
    SUB_ENTRY(LeftControl,  341, GLFW_KEY_LEFT_CONTROL)           \
    SUB_ENTRY(LeftAlt,      342, GLFW_KEY_LEFT_ALT)               \
    SUB_ENTRY(LeftSuper,    343, GLFW_KEY_LEFT_SUPER)             \
    SUB_ENTRY(RightShift,   344, GLFW_KEY_RIGHT_SHIFT)            \
    SUB_ENTRY(RightControl, 345, GLFW_KEY_RIGHT_CONTROL)          \
    SUB_ENTRY(RightAlt,     346, GLFW_KEY_RIGHT_ALT)              \
    SUB_ENTRY(RightSuper,   347, GLFW_KEY_RIGHT_SUPER)            \
    SUB_ENTRY(Menu,         348, GLFW_KEY_MENU)

#define SUBSTRATE_MOUSE_LIST(SUB_ENTRY)                           \
    SUB_ENTRY(Left,   0, GLFW_MOUSE_BUTTON_LEFT)                  \
    SUB_ENTRY(Right,  1, GLFW_MOUSE_BUTTON_RIGHT)                 \
    SUB_ENTRY(Middle, 2, GLFW_MOUSE_BUTTON_MIDDLE)                \
    SUB_ENTRY(Extra1, 3, GLFW_MOUSE_BUTTON_4)                     \
    SUB_ENTRY(Extra2, 4, GLFW_MOUSE_BUTTON_5)                     \
    SUB_ENTRY(Extra3, 5, GLFW_MOUSE_BUTTON_6)                     \
    SUB_ENTRY(Extra4, 6, GLFW_MOUSE_BUTTON_7)                     \
    SUB_ENTRY(Extra5, 7, GLFW_MOUSE_BUTTON_8)

#define SUBSTRATE_PAD_BUTTON_LIST(SUB_ENTRY)                      \
    SUB_ENTRY(A,            0,  GLFW_GAMEPAD_BUTTON_A)            \
    SUB_ENTRY(B,            1,  GLFW_GAMEPAD_BUTTON_B)            \
    SUB_ENTRY(X,            2,  GLFW_GAMEPAD_BUTTON_X)            \
    SUB_ENTRY(Y,            3,  GLFW_GAMEPAD_BUTTON_Y)            \
    SUB_ENTRY(LeftBumper,   4,  GLFW_GAMEPAD_BUTTON_LEFT_BUMPER)  \
    SUB_ENTRY(RightBumper,  5,  GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER) \
    SUB_ENTRY(Back,         6,  GLFW_GAMEPAD_BUTTON_BACK)         \
    SUB_ENTRY(Start,        7,  GLFW_GAMEPAD_BUTTON_START)        \
    SUB_ENTRY(Guide,        8,  GLFW_GAMEPAD_BUTTON_GUIDE)        \
    SUB_ENTRY(LeftThumb,    9,  GLFW_GAMEPAD_BUTTON_LEFT_THUMB)   \
    SUB_ENTRY(RightThumb,   10, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB)  \
    SUB_ENTRY(DpadUp,       11, GLFW_GAMEPAD_BUTTON_DPAD_UP)      \
    SUB_ENTRY(DpadRight,    12, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT)   \
    SUB_ENTRY(DpadDown,     13, GLFW_GAMEPAD_BUTTON_DPAD_DOWN)    \
    SUB_ENTRY(DpadLeft,     14, GLFW_GAMEPAD_BUTTON_DPAD_LEFT)

#define SUBSTRATE_PAD_AXIS_LIST(SUB_ENTRY)                        \
    SUB_ENTRY(LeftX,        0, GLFW_GAMEPAD_AXIS_LEFT_X)          \
    SUB_ENTRY(LeftY,        1, GLFW_GAMEPAD_AXIS_LEFT_Y)          \
    SUB_ENTRY(RightX,       2, GLFW_GAMEPAD_AXIS_RIGHT_X)         \
    SUB_ENTRY(RightY,       3, GLFW_GAMEPAD_AXIS_RIGHT_Y)         \
    SUB_ENTRY(LeftTrigger,  4, GLFW_GAMEPAD_AXIS_LEFT_TRIGGER)    \
    SUB_ENTRY(RightTrigger, 5, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER)
// clang-format on

#define SUBSTRATE_ENUMERATOR(name, value, glfwMacro) name = (value),

enum class Key : int { Unknown = -1, SUBSTRATE_KEY_LIST(SUBSTRATE_ENUMERATOR) };
enum class MouseButton : int { Unknown = -1, SUBSTRATE_MOUSE_LIST(SUBSTRATE_ENUMERATOR) };
enum class PadButton : int { Unknown = -1, SUBSTRATE_PAD_BUTTON_LIST(SUBSTRATE_ENUMERATOR) };
enum class PadAxis : int { Unknown = -1, SUBSTRATE_PAD_AXIS_LIST(SUBSTRATE_ENUMERATOR) };

#undef SUBSTRATE_ENUMERATOR

/// One past the largest key code. The key-state arrays are indexed by code, so a key added
/// above this without raising it writes past their end.
constexpr int kKeyCodeCount = 349;
constexpr int kMouseButtonCount = 8;
constexpr int kPadButtonCount = 15;
constexpr int kPadAxisCount = 6;

/// Value at or above which an analog action reads as held. Halfway, so a stick pushed to
/// its edge acts like a key and a stick resting off-centre does not.
constexpr float kDigitalThreshold = 0.5f;

/// @brief One physical control feeding one action.
///
/// `scale` is for axes: a stick's Y is one axis with two meanings, so `Camera.Forward`
/// takes it at -1 and `Camera.Back` at +1.
struct Binding {
    enum class Source : uint8_t { Unbound, Key, Mouse, PadButton, PadAxis };

    Source source = Source::Unbound;
    int code = 0;
    float scale = 1.0f;

    friend bool operator==(const Binding& a, const Binding& b) {
        return a.source == b.source && a.code == b.code && a.scale == b.scale;
    }
};

/// "W", "F8", "LeftShift", "Mouse.Left", "Pad.A", "Pad.LeftY-". These spellings are what a
/// config file holds, so changing one silently unbinds every existing file that used it.
/// An axis always carries a sign; without a direction it is half a binding.
std::string bindingName(const Binding& b);
/// Unbound for anything unrecognised. Case-insensitive on the key name, so a config file
/// may say "w".
Binding bindingFromName(std::string_view text);

/// Space-separated list, in the order the action holds them.
std::string bindingListName(const std::vector<Binding>& bindings);
std::vector<Binding> bindingListFromName(std::string_view text);

/// @brief A gamepad's raw state, as the window layer read it.
///
/// Axes arrive already remapped: sticks in [-1,1], and the triggers -- which GLFW reports
/// resting at -1 -- rescaled to [0,1], so "not pressed" is zero on every axis. The deadzone
/// is *not* applied here; the map applies it, because it is a preference.
struct GamepadState {
    bool connected = false;
    float axes[kPadAxisCount]{};
    bool buttons[kPadButtonCount]{};
};

/// Pads the map will hold. Capped at 16 because `PlayerDevices::pads` is a 32-bit mask and
/// GLFW enumerates sixteen joystick slots; raising it past 32 needs a wider mask.
constexpr uint32_t kMaxPads = 16;

/// @brief Which devices one player is holding.
///
/// `pads` is a bit per pad index, so `1u << 2` is the third pad. `keyboard` covers the
/// mouse: they are one pair of hands.
struct PlayerDevices {
    bool keyboard = false;
    uint32_t pads = 0;
};

/// Every pad, which is what a single-player game wants.
constexpr uint32_t kAllPads = 0xFFFFFFFFu;

using ActionId = uint32_t;
constexpr ActionId kInvalidAction = UINT32_MAX;

/**
 * @brief Named actions, their bindings, and the raw device state behind them.
 *
 * The window layer pushes events in; `beginFrame` resolves every action once; the frame
 * reads `held`/`pressed`/`value`. Resolving on demand instead would make "did this action
 * fire" answer differently at two points in one frame, and would lose a key tapped and
 * released between two polls.
 */
class InputMap {
  public:
    /// Idempotent: declaring an existing name returns its id and leaves its bindings alone,
    /// so a second caller cannot silently reset the first one's rebind. A retired action is
    /// found too, and declaring it again revives it.
    ActionId declare(std::string name, std::string_view defaultBindings = {});

    /**
     * @brief Take an action out of circulation without invalidating its `ActionId`.
     *
     * The row is tombstoned, never erased: an id is an index into `actions`, so erasing
     * shifts every id above it and silently repoints the ids a game already holds.
     *
     * Bindings and defaults are kept so a revived scheme comes back with the player's
     * rebind intact, and `saveBindings` still writes a retired row that differs from its
     * default -- switching scheme before quitting must not drop the edit.
     *
     * Out of range is a no-op, as everything else here is.
     */
    void retire(ActionId id);
    [[nodiscard]] bool actionLive(ActionId id) const;

    [[nodiscard]] ActionId find(std::string_view name) const;
    /// @brief `find`, but a retired row answers too.
    ///
    /// Only for a caller that *stores* a binding rather than acts on one: through `find`, a
    /// config row naming a retired action reads as unknown, falls back to the default, and
    /// the next `saveBindings` drops the player's edit for good. Anything reacting to input
    /// wants `find`, since a dead row must stay unfindable to a frame.
    [[nodiscard]] ActionId findDeclared(std::string_view name) const;
    /// The size of the action table, retired rows included, because it is a loop bound
    /// against an id-indexed table. Callers walking it ask `actionLive` per row.
    [[nodiscard]] uint32_t actionCount() const { return static_cast<uint32_t>(actions.size()); }
    [[nodiscard]] const std::string& actionName(ActionId id) const;

    void bind(ActionId id, const Binding& b);
    void setBindings(ActionId id, std::string_view list);
    /**
     * @brief Move an action's *declared default*, and the live list with it.
     *
     * What a game calls to ship a control scheme differing from whatever declared the
     * action. `setBindings` is not a substitute: it moves the live list and leaves
     * `defaults` behind, so `isDefault()` goes false, the binding menu offers to reset to a
     * key the game never uses, and `saveBindings` writes rows the player never touched.
     *
     * Call it from `Game::init` and nowhere later: `Engine::run` applies the config's
     * rebinds after `Game::init`, so a call after that overwrites a live rebind.
     *
     * Out of range is a no-op, as everything else here is.
     */
    void setDefaultBindings(ActionId id, std::string_view list);
    void clearBindings(ActionId id);
    [[nodiscard]] const std::vector<Binding>& bindings(ActionId id) const;
    [[nodiscard]] std::string bindingList(ActionId id) const;
    /// The list this action was declared with. What "reset" restores, and what keeps a
    /// saved file holding only what the user changed.
    [[nodiscard]] const std::string& defaultBindingList(ActionId id) const;
    [[nodiscard]] bool isDefault(ActionId id) const;
    void resetToDefault(ActionId id);

    /**
     * @brief Config rows waiting for the action they name to be declared.
     *
     * `applyBindings` runs once after `Game::init`, but a camera declares its rows when it
     * is *installed*, so rows for a camera not yet installed would be dropped by a lookup
     * with nothing to find. `declare` takes a parked row the first time that name appears.
     *
     * A row that is applied is erased. Replaying it instead would push the file back over a
     * scheme the player has since rebound and revived.
     *
     * These are not actions: nothing resolves them, `conflicts()` cannot see them, and
     * `saveBindings` must never write one -- inventing a row would freeze a typo into the
     * config for good. `Engine` reports what is left at exit.
     *
     * The setter replaces the whole set, because the argument is a whole config file.
     */
    void setParkedBindings(std::vector<std::pair<std::string, std::string>> rows);
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& parkedBindings() const {
        return parked;
    }

    /// Two actions that fire from the same binding. `a` is the earlier declaration.
    struct Conflict {
        ActionId a = kInvalidAction;
        ActionId b = kInvalidAction;
        Binding binding;
    };

    /**
     * @brief Every pair of actions sharing a binding.
     *
     * A query rather than a refusal, because a collision is not always a bug -- a menu's
     * "back" and a game's "crouch" live in different modes -- and only the caller knows.
     * `Engine` logs them after the config is applied and continues.
     *
     * Unbound actions never collide with each other.
     */
    [[nodiscard]] std::vector<Conflict> conflicts() const;

    void onKey(Key key, bool down);
    void onMouseButton(MouseButton button, bool down);
    void onCursorPos(double x, double y);
    void onScroll(double yoffset);
    /// One pad's state, by index. The window layer pushes each connected pad separately.
    void setGamepad(const GamepadState& pad, uint32_t index = 0);
    /// Release every key and button without an event. Call on focus loss, or alt-tabbing
    /// away mid-chord leaves the held keys held forever.
    void loseFocus();

    /// Resolve every action from the state accumulated since the last call.
    void beginFrame();

    /**
     * @brief How many players this map resolves every action for.
     *
     * One by default, holding the keyboard and every pad. Growing it gives the new players
     * *no* devices -- the caller assigns them, because only the game knows whether the
     * second person is on a pad, on the arrow keys, or not there yet.
     *
     * Shrinking is not an error: a query past the end answers as an unbound action does.
     */
    void setPlayerCount(uint32_t players);
    [[nodiscard]] uint32_t playerCount() const { return static_cast<uint32_t>(players.size()); }
    void setPlayerDevices(uint32_t player, PlayerDevices devices);
    [[nodiscard]] PlayerDevices playerDevices(uint32_t player) const;

    [[nodiscard]] float value(ActionId id, uint32_t player = 0) const;
    [[nodiscard]] bool held(ActionId id, uint32_t player = 0) const;
    [[nodiscard]] bool pressed(ActionId id, uint32_t player = 0) const;
    [[nodiscard]] bool released(ActionId id, uint32_t player = 0) const;

    // Raw queries, for code choosing a binding rather than acting on one.
    [[nodiscard]] bool keyDown(Key key) const;
    [[nodiscard]] bool keyPressed(Key key) const;
    [[nodiscard]] double cursorX() const { return cursor[0]; }
    [[nodiscard]] double cursorY() const { return cursor[1]; }
    [[nodiscard]] double cursorDeltaX() const { return cursorDelta[0]; }
    [[nodiscard]] double cursorDeltaY() const { return cursorDelta[1]; }
    [[nodiscard]] double scrollDelta() const { return scroll; }
    /// Any pad at all, or one named pad.
    [[nodiscard]] bool gamepadConnected() const;
    [[nodiscard]] bool gamepadConnected(uint32_t pad) const;
    [[nodiscard]] uint32_t padCount() const { return static_cast<uint32_t>(pads.size()); }

    /// Arm the capture: the next physical control pressed becomes this action's binding.
    /// `replace` clears the existing list first. Escape cancels and stays unbindable, or a
    /// rebind menu has no way out.
    void beginCapture(ActionId id, bool replace = true);
    void cancelCapture();
    [[nodiscard]] bool capturing() const { return captureTarget != kInvalidAction; }
    [[nodiscard]] ActionId captureAction() const { return captureTarget; }
    /// True on the frame a capture completed, so a caller can save or redraw.
    [[nodiscard]] bool captureCompleted() const { return captureDone; }

    /// Suppress every keyboard-sourced binding. Set while a text field has focus, or a
    /// player typing "was" walks. Mouse and pad keep resolving.
    void setTextMode(bool on) { textModeOn = on; }
    [[nodiscard]] bool textMode() const { return textModeOn; }
    /// Exempt one action from that suppression: the one that closes the field, which
    /// otherwise cannot fire while the field it dismisses is open.
    void setTextModeExempt(ActionId id, bool exempt);

    /// @brief Suppress every *mouse*-sourced binding, as text mode suppresses keys.
    ///
    /// Set while the UI has the pointer, or a drag on a slider also reaches whatever the
    /// game bound to `Mouse.Left` and spins the world behind the panel. Keyboard and pad
    /// keep resolving.
    void setPointerMode(bool on) { pointerMode = on; }
    [[nodiscard]] bool pointerModeActive() const { return pointerMode; }
    /// Exempt one action from that suppression -- the UI's own click.
    void setPointerModeExempt(ActionId id, bool exempt);

    /// Stick displacement below this reads as zero. Applied here rather than in the device
    /// layer because it is a preference, and lives in substrate.json.
    float gamepadDeadzone = 0.15f;

  private:
    /// One player's answer for one action, kept per action rather than as a second action
    /// table per player -- the name, bindings and exemptions are the action's, and the
    /// other layout copies them once per player.
    struct ActionState {
        float value = 0.0f;
        bool held = false;
        bool heldLast = false;
        bool pressed = false;
        bool released = false;
    };

    struct Action {
        std::string name;
        std::string defaults;
        std::vector<Binding> bindings;
        std::vector<ActionState> state{1};
        bool textExempt = false;
        bool pointerExempt = false;
        /// Tombstone. Nothing is ever erased from `actions`, because an id is its index.
        bool live = true;
    };

    [[nodiscard]] float resolve(const Binding& b, const PlayerDevices& devices, bool textExempt,
                                bool pointerExempt) const;
    [[nodiscard]] bool edgePressed(const Binding& b, const PlayerDevices& devices, bool textExempt,
                                   bool pointerExempt) const;
    /// The one place a pad index is checked against `kMaxPads` before it shifts the mask;
    /// `1u << pad` at 32 or above is undefined.
    [[nodiscard]] static bool holdsPad(const PlayerDevices& devices, uint32_t pad) {
        return pad < kMaxPads && (devices.pads & (1u << pad)) != 0u;
    }
    bool serviceCapture();

    std::vector<Action> actions;
    /// Config rows nothing has declared yet -- see `setParkedBindings`.
    std::vector<std::pair<std::string, std::string>> parked;
    /// Devices per player. Must never be empty: a map with no players resolves nothing and
    /// every query goes quiet, which is why `setPlayerCount(0)` still leaves one.
    std::vector<PlayerDevices> players{PlayerDevices{true, kAllPads}};

    // Two sets of press flags, not one. Events accumulate into `*Events`; `beginFrame`
    // moves them into `*Frame` and clears the accumulator. Collapsing them makes every edge
    // query after `beginFrame` -- which is all of them -- read flags just cleared.
    bool keys[kKeyCodeCount]{};
    bool keyPressedEvents[kKeyCodeCount]{};
    bool keyPressedFrame[kKeyCodeCount]{};
    bool mouse[kMouseButtonCount]{};
    bool mousePressedEvents[kMouseButtonCount]{};
    bool mousePressedFrame[kMouseButtonCount]{};

    /// One entry per pad the window layer has ever reported. Never shrunk: an unplugged pad
    /// keeps its index, so the player assigned to it gets it back when it returns.
    std::vector<GamepadState> pads;
    std::vector<GamepadState> padsLast;

    double cursor[2]{};
    double cursorPrevFrame[2]{};
    double cursorDelta[2]{};
    bool cursorSeen = false;
    double scrollAccum = 0.0;
    double scroll = 0.0;

    ActionId captureTarget = kInvalidAction;
    bool captureReplace = true;
    bool captureDone = false;
    bool textModeOn = false;
    bool pointerMode = false;
};

/**
 * @brief Ask for the pointer: hidden, and reporting unbounded deltas.
 *
 * Process state, not one map's -- there is one physical cursor, and two maps asking would
 * be talking about it.
 *
 * These record a *desire*; the frame loop applies the platform's cursor mode, and the
 * effective grab is `mouseGrabbed() && !uiOpen && the window has focus`. That is what keeps
 * a panel opening mid-drag from leaving the pointer captured, with nothing to re-assert
 * afterwards. No refcounting: one boolean, last writer wins.
 */
void mouseGrab();
void mouseRelease();
[[nodiscard]] bool mouseGrabbed();
/// Drop the desire and everything else the process holds about the pointer. The unit suite
/// links these sources, so without it a grab carries between tests.
void mouseGrabReset();

/**
 * @brief A deterministic feed of action presses, played into an `InputMap` by frame.
 *
 * `--input-script 60:Game.Save,90:Camera.Forward+,150:Camera.Forward-`. `apply` synthesises
 * the event the device behind that action's binding would have produced and hands it to the
 * map's ordinary event feed. Nothing here may reach past that feed -- setting `pressed`
 * directly turns every test using this into a test of the feed, with text mode, the
 * deadzone and rebinding all bypassed.
 *
 * Steps are addressed by frame index and never by elapsed time, and `apply` is const, so a
 * scripted frame is a function of its index and the step list alone: a frame applied twice
 * is a frame applied once, and a missed call cannot make the run drift.
 *
 * The *first* binding is the one driven, which is the one the binding menu lists first and
 * a rebind replaces. An action holding none is skipped.
 */
class Script {
  public:
    /// One event: on `frame`, `action` goes down or up.
    struct Step {
        uint64_t frame = 0;
        std::string action;
        bool down = false;
        /// Which pad a pad-bound step drives, from `@1`. Keys and mouse buttons ignore it:
        /// the keyboard is one device however many people share it.
        uint32_t pad = 0;
    };

    /**
     * @brief Parse `<frame>:<action>[+|-][@<pad>]` steps, separated by commas or
     *        whitespace.
     *
     * `+` presses, `-` releases, a bare action is both on the same frame. The suffix is
     * unambiguous only because this addresses *actions*: a binding name can end in one
     * (`Pad.LeftY-`).
     *
     * All or nothing, and replaces whatever was here. A script that half-ran would report a
     * result against whichever half it managed to press, so a malformed step logs and
     * leaves the list untouched.
     */
    bool parse(std::string_view text);

    void add(uint64_t frame, std::string action, bool down, uint32_t pad = 0);

    [[nodiscard]] bool empty() const { return list.empty(); }
    [[nodiscard]] const std::vector<Step>& steps() const { return list; }
    /// Highest frame any step names, or zero for an empty script. A run shorter than this
    /// never reaches the later steps.
    [[nodiscard]] uint64_t lastFrame() const;

    /// @brief Names the script uses that no declared action claims.
    ///
    /// Only meaningful once every action has been declared. `Engine` logs it there, because
    /// a typo whose first symptom is silence at frame 500 has already cost the run.
    [[nodiscard]] std::vector<std::string> unknownActions(const InputMap& map) const;

    /**
     * @brief Feed every step scheduled for `frame` into `map`. Call before `beginFrame`.
     *
     * An unclaimed name is skipped in silence; `unknownActions` reports it once instead.
     *
     * A script holding a pad-bound step takes that pad over for the whole run -- a pad is
     * pushed as a whole state, so the state is rebuilt from every pad step up to `frame`.
     * A script naming only pad 1 leaves a real pad on slot 0 working.
     */
    void apply(InputMap& map, uint64_t frame) const;

  private:
    std::vector<Step> list;
};

/**
 * @brief An editable UTF-8 string fed by character events.
 *
 * Characters arrive already resolved through the keyboard layout and any IME. Editing keys
 * arrive as *keys*, because there is no character for "delete the previous one".
 *
 * Repeat for those editing keys is driven here from the frame delta, not by the platform:
 * the character callback repeats letters at the system rate, nothing repeats a backspace,
 * and a repeat sourced from the window system is not testable.
 *
 * `cursorBytes` is a byte offset that must stay on a UTF-8 boundary -- every move and
 * delete steps a whole codepoint, or a backspace cuts a multi-byte character in half.
 */
class TextInput {
  public:
    void setActive(bool on);
    [[nodiscard]] bool active() const { return activeFlag; }

    void onChar(uint32_t codepoint);
    void onKey(Key key, bool down);
    /// Drives key repeat. Call once per frame with the frame delta, whether or not the
    /// field is active.
    void update(float dt);

    [[nodiscard]] const std::string& text() const { return buffer; }
    [[nodiscard]] size_t cursor() const { return cursorBytes; }
    void setText(std::string value);
    void clear();

    /// Consumed on read: Enter and Escape are events, and one left true fires its handler
    /// on every frame after the first.
    bool takeSubmitted();
    bool takeCancelled();

    /// 0 is unbounded. A field with a limit gets `overflowed()` when it is hit, rather than
    /// characters vanishing.
    size_t maxBytes = 0;
    [[nodiscard]] bool overflowed() const { return overflowFlag; }

    float repeatDelay = 0.4f; ///< seconds held before the first repeat
    float repeatRate = 0.03f; ///< seconds between repeats after that

  private:
    void apply(Key key);
    static bool repeatable(Key key);

    std::string buffer;
    size_t cursorBytes = 0;
    bool activeFlag = false;
    bool submitted = false;
    bool cancelled = false;
    bool overflowFlag = false;

    Key repeatKey = Key::Unknown;
    /// Time since the press, not a countdown: counting up keeps the Nth repeat at
    /// `delay + N * rate` exactly, where a countdown accumulates N roundings.
    float repeatHeld = 0.0f;
    uint32_t repeatCount = 0;
};

/// Number of bytes in the UTF-8 sequence starting at `s[at]`, or 1 for a malformed
/// lead byte so that a walk always terminates.
size_t utf8SequenceLength(const std::string& s, size_t at);
/// Append `codepoint` to `out` as UTF-8. Codepoints above 0x10FFFF and the surrogate
/// range are dropped -- they cannot be encoded, and inventing a replacement here would
/// hide a platform bug.
void utf8Encode(std::string& out, uint32_t codepoint);

/**
 * @brief Apply a name-to-binding-list table over the actions already declared.
 *
 * Rows resolve through `findDeclared`, not `find`, so a retired action is rebound rather
 * than treated as missing. A row that resolves to nothing is parked, not dropped -- see
 * `InputMap::setParkedBindings` -- and this call *replaces* the map's parked set, so a
 * second table is not shadowed by the first one's leftovers.
 *
 * @return rows applied to a declared action. A parked row is applied later, by the
 *         `declare` that first names it, and is counted by nothing.
 */
uint32_t applyBindings(InputMap& map, const std::vector<std::pair<std::string, std::string>>& table);

/**
 * @brief Rewrite `input.bindings` in an existing config file, in place.
 *
 * Only actions whose bindings differ from their declared defaults are written. Writing
 * every action instead freezes today's defaults into the file, so a default that moves in a
 * later build never reaches anyone who never rebound it.
 *
 * Re-parses the file and swaps one object rather than serialising the struct `Config` read,
 * because every key the reader never looked at has to survive the save. The cost is that
 * rapidjson pretty-prints the file back rather than preserving its hand-written layout.
 *
 * A file that does not exist yet is created holding only the input section.
 */
bool saveBindings(const InputMap& map, const std::string& configPath);

} // namespace input

} // namespace core
