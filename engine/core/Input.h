#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core {

/**
 * @file engine/core/Input.h
 * @brief Named actions bound to physical controls, and a text field (S1).
 *
 * Three things live here, and the first two are deliberately separate mechanisms rather
 * than one with a mode flag:
 *
 * - `InputMap` resolves *actions*. A frame asks "is Camera.Forward held", not "is W
 *   down". Keyboard, mouse buttons and gamepad all feed the same action, so adding a
 *   pad is a binding rather than a second input path with its own switch statement.
 * - `TextInput` accumulates *characters*. Typing is not a key press that happens to
 *   produce a letter -- it is layout- and IME-dependent, arrives on a different
 *   callback, and repeats. A text field built out of key actions gets a QWERTY-only
 *   editor that cannot type an accent.
 * - `Script` is a third *source* for the first one rather than a mechanism of its own:
 *   a list of presses addressed by frame index, fed in through the same event calls the
 *   window layer uses. It is here and not in the tests because a game binary is what a
 *   golden case or an end-to-end regression drives, and a feed the engine did not ship
 *   could not reach one.
 *
 * Nothing in this file includes GLFW. The key, mouse and gamepad codes below *are*
 * GLFW's numeric values, so the window layer casts rather than translates, and
 * `engine/core/InputGlfw.cpp` static_asserts every one of them against the real macro --
 * see SUBSTRATE_KEY_LIST. A drift between the two is a compile error, not a key that
 * silently stops working.
 *
 * ## Scale
 *
 * **Generalized.** There is no fixed action count, no fixed binding count per action,
 * no capped text length and no fixed player or pad count: all are vectors sized by what
 * the caller declared. Two quantities are bound, both by a code space rather than by
 * usage -- the key-state array, indexed by key code, and the pad vector, capped at
 * `kMaxPads` because `PlayerDevices::pads` is a 32-bit mask.
 */
namespace input {

// clang-format off
/**
 * @brief Every key this engine can name, as (enumerator, value, GLFW macro).
 *
 * The third column is only expanded by engine/core/InputGlfw.cpp, which is the one
 * translation unit that includes GLFW. Everywhere else the macro parameter goes
 * unused and unexpanded, which is what keeps this header GLFW-free while still being
 * checkable against it.
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

/// One past the largest key code, which is what the key-state arrays are sized to.
constexpr int kKeyCodeCount = 349;
constexpr int kMouseButtonCount = 8;
constexpr int kPadButtonCount = 15;
constexpr int kPadAxisCount = 6;

/// Value at or above which an analog action reads as held. Halfway, so a stick pushed
/// to its edge acts like a key and a stick resting off-centre does not.
constexpr float kDigitalThreshold = 0.5f;

/**
 * @brief One physical control feeding one action.
 *
 * `scale` exists for axes: a stick's Y is one axis with two meanings, so
 * `Camera.Forward` takes it at -1 and `Camera.Back` at +1. Buttons and keys always
 * contribute their full value; a scale on one of those would be a volume knob nobody
 * asked for.
 */
struct Binding {
    enum class Source : uint8_t { Unbound, Key, Mouse, PadButton, PadAxis };

    Source source = Source::Unbound;
    int code = 0;
    float scale = 1.0f;

    friend bool operator==(const Binding& a, const Binding& b) {
        return a.source == b.source && a.code == b.code && a.scale == b.scale;
    }
};

/// "W", "F8", "LeftShift", "Mouse.Left", "Pad.A", "Pad.LeftY-". Axes always carry a
/// sign, because an axis without a direction is half a binding.
std::string bindingName(const Binding& b);
/// Unbound for anything unrecognised. Case-insensitive on the key name, so a config
/// file may say "w".
Binding bindingFromName(std::string_view text);

/// Space-separated list, in the order the action holds them.
std::string bindingListName(const std::vector<Binding>& bindings);
std::vector<Binding> bindingListFromName(std::string_view text);

/**
 * @brief A gamepad's raw state, as the window layer read it.
 *
 * Axes are already remapped: sticks stay [-1,1], and the two triggers -- which GLFW
 * reports resting at -1 -- are rescaled to [0,1] so that "not pressed" is zero on
 * every axis. The deadzone is applied later, by the map, because it is a preference
 * rather than a property of the device.
 */
struct GamepadState {
    bool connected = false;
    float axes[kPadAxisCount]{};
    bool buttons[kPadButtonCount]{};
};

/// Pads the map will hold. GLFW enumerates sixteen joystick slots, and the assignment
/// below is a bitmask, so this is the smaller of the two limits and is stated here rather
/// than discovered at slot 32.
constexpr uint32_t kMaxPads = 16;

/**
 * @brief Which devices one player is holding (C26).
 *
 * **A player rather than a device is what an action resolves against**, and that is the
 * decision the row turned on. A device index on the binding reads as the obvious answer
 * and is the wrong unit twice over: two people can share a keyboard -- WASD and the arrow
 * keys is the oldest local-co-op scheme there is -- and one person can hold a pad and a
 * HOTAS at once. Neither is expressible if a binding names a device; both are a set here.
 *
 * `pads` is a bit per pad index, so `1u << 2` is the third pad and `kAllPads` is every one
 * of them. `keyboard` covers the mouse too: they are one pair of hands.
 */
struct PlayerDevices {
    bool keyboard = false;
    uint32_t pads = 0;
};

/// Every pad, which is what a single-player game wants and what the merged state used to
/// do by accident.
constexpr uint32_t kAllPads = 0xFFFFFFFFu;

using ActionId = uint32_t;
constexpr ActionId kInvalidAction = UINT32_MAX;

/**
 * @brief Named actions, their bindings, and the raw device state behind them.
 *
 * The window layer pushes events in; `beginFrame` resolves every action once; the
 * frame reads `held`/`pressed`/`value`. Resolving once per frame rather than on
 * demand is what makes "did this action fire" the same answer everywhere in the
 * frame, including for a key that was tapped and released between two polls.
 */
class InputMap {
  public:
    /// Idempotent: declaring an existing name returns its id and leaves its bindings
    /// alone, so a second caller cannot silently reset the first one's rebind. A retired
    /// action is found too, and declaring it again revives it -- see `retire`.
    ActionId declare(std::string name, std::string_view defaultBindings = {});

    /**
     * @brief Take an action out of circulation without invalidating its `ActionId`.
     *
     * An id is an index into the action table, so erasing a row would shift every id above
     * it and silently repoint the ids a game is holding -- a failure with no symptom until
     * a keypress does the wrong thing. The row is tombstoned instead: it resolves as
     * unheld, `find` no longer sees it, and its id stays valid and still names it.
     *
     * **Bindings and defaults are kept, and that is the point.** `declare` is idempotent by
     * name, so re-declaring a retired action revives it with the same id and whatever the
     * player had rebound it to; a control scheme that comes and goes does not cost them the
     * edit. For the same reason `saveBindings` still writes a retired row that differs from
     * its default -- switching scheme before quitting must not drop it.
     *
     * Out of range is a no-op, as everything else here is.
     */
    void retire(ActionId id);
    [[nodiscard]] bool actionLive(ActionId id) const;

    [[nodiscard]] ActionId find(std::string_view name) const;
    /**
     * @brief `find`, but a retired row answers too -- the *action* rather than the live map.
     *
     * For a caller that stores a binding rather than acts on one, which is `applyBindings`
     * and nothing else so far: a config row naming an action the game retired in
     * `Game::init` read as unknown through `find`, so the player's edit fell back to the
     * default and the next `saveBindings` dropped it for good. Anything *reacting* to input
     * wants `find` -- a dead row resolves as unheld and has to stay unfindable to a frame.
     */
    [[nodiscard]] ActionId findDeclared(std::string_view name) const;
    /// The size of the action table, retired rows included -- it is a loop bound, and a
    /// live count would make those loops skip real rows rather than dead ones. Callers
    /// walking it for display or for the config ask `actionLive` per row.
    [[nodiscard]] uint32_t actionCount() const { return static_cast<uint32_t>(actions.size()); }
    [[nodiscard]] const std::string& actionName(ActionId id) const;

    void bind(ActionId id, const Binding& b);
    void setBindings(ActionId id, std::string_view list);
    /**
     * @brief Move an action's *declared default*, and the live list with it.
     *
     * What a game reaches for when it ships a control scheme that differs from whatever
     * declared the action -- the engine's camera owning W, A, S and D being the case that
     * exists. `setBindings` alone looks like it does the same job and quietly does a
     * different one: it moves the live list and leaves `defaults` behind, so `isDefault()`
     * goes false, the binding menu offers to "reset" the camera back onto W, and
     * `saveBindings` writes rows the player never touched. That is a game pretending to be
     * a user who edited a binding.
     *
     * Rewriting both is safe at the moment a game has to call it: `Engine::run` applies the
     * config's rebinds *after* `Game::init`, so a player who really did move this action
     * still wins. Called later than that it overwrites a live rebind, which is why the one
     * documented call site is `Game::init`.
     *
     * Out of range is a no-op, as everything else here is.
     */
    void setDefaultBindings(ActionId id, std::string_view list);
    void clearBindings(ActionId id);
    [[nodiscard]] const std::vector<Binding>& bindings(ActionId id) const;
    [[nodiscard]] std::string bindingList(ActionId id) const;
    /// The list this action was declared with, which is what "reset" restores and what
    /// makes a saved file hold only what the user actually changed.
    [[nodiscard]] const std::string& defaultBindingList(ActionId id) const;
    [[nodiscard]] bool isDefault(ActionId id) const;
    void resetToDefault(ActionId id);

    /**
     * @brief Config rows waiting for the action they name to be declared.
     *
     * `applyBindings` runs once, right after `Game::init`, and a camera declares its rows
     * when it is *installed* -- so a game holding three cameras has two thirds of its
     * `Camera.*` rows undeclared at the moment the file is read, and every rebind of them
     * would be dropped by a lookup that has nothing to find. Rows `applyBindings` could not
     * resolve are parked here instead, and `declare` takes one the first time that name
     * appears, whenever that is.
     *
     * **Taken once, and gone.** A row that is applied is erased, so a scheme that is later
     * retired and revived comes back holding whatever the player has on it *now* rather
     * than having the file replayed over it.
     *
     * These are not actions: nothing resolves them, `conflicts()` cannot see them and
     * `saveBindings` never writes one -- there is no row to write, and inventing one would
     * freeze a typo into the config for good. A name no game ever declares simply stays
     * here, which is what `Engine` reports at exit as the unknown action it really is.
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
     * `declare` deduplicates by *name*, which says nothing about two actions reaching for
     * the same key -- so a game that binds a save to F2 while the debug views already own
     * it gets both behaviours on one press and no complaint from anywhere. That is a
     * silent failure with no symptom except a person eventually noticing, which is the
     * kind this project pays a query to turn into a startup line.
     *
     * A query rather than a refusal: a collision is not always a bug. Two actions live in
     * different modes often enough -- a menu's "back" and a game's "crouch" -- that the
     * engine has no business rejecting one, and the caller is the one who knows. `Engine`
     * logs them after the config is applied and continues.
     *
     * Unbound actions never collide with each other; an action with no binding is not
     * competing for anything.
     */
    [[nodiscard]] std::vector<Conflict> conflicts() const;

    // ------------------------------------------------------------- event feed
    void onKey(Key key, bool down);
    void onMouseButton(MouseButton button, bool down);
    void onCursorPos(double x, double y);
    void onScroll(double yoffset);
    /// One pad's state, by index (C26). The window layer pushes each connected pad
    /// separately; nothing merges them any more.
    void setGamepad(const GamepadState& pad, uint32_t index = 0);
    /// Every key and button released without an event, for a window that lost focus
    /// mid-chord. Without it, alt-tabbing away while holding W flies forever.
    void loseFocus();

    /// Resolve every action from the state accumulated since the last call.
    void beginFrame();

    // --------------------------------------------------------------- players (C26)
    /**
     * @brief How many players this map resolves every action for.
     *
     * One by default, holding the keyboard and every pad -- which is exactly what the
     * merged pad state did before this existed, so a game that never calls this sees no
     * change at all. Growing it leaves player 0's devices alone and gives the new players
     * none: an engine that guessed "pad *i* belongs to player *i*" would be inventing a
     * seating plan, and the game is what knows whether the second person is on a pad, on
     * the arrow keys, or not there yet.
     *
     * Shrinking below the count a caller then asks about is not an error -- every query
     * past the end answers as an unbound action does, because a player who has left is a
     * player pressing nothing.
     */
    void setPlayerCount(uint32_t players);
    [[nodiscard]] uint32_t playerCount() const { return static_cast<uint32_t>(players.size()); }
    void setPlayerDevices(uint32_t player, PlayerDevices devices);
    [[nodiscard]] PlayerDevices playerDevices(uint32_t player) const;

    // ------------------------------------------------------------- action state
    /// Player 0 unless another is named, which is what keeps every existing caller --
    /// the demo, the UI, the binding menu -- a one-player caller with no edit.
    [[nodiscard]] float value(ActionId id, uint32_t player = 0) const;
    [[nodiscard]] bool held(ActionId id, uint32_t player = 0) const;
    [[nodiscard]] bool pressed(ActionId id, uint32_t player = 0) const;
    [[nodiscard]] bool released(ActionId id, uint32_t player = 0) const;

    // ------------------------------------------------------------ raw queries
    // For code that is choosing a binding rather than acting on one -- the rebind
    // menu -- plus the pointer, which no action wants to be.
    [[nodiscard]] bool keyDown(Key key) const;
    [[nodiscard]] bool keyPressed(Key key) const;
    [[nodiscard]] double cursorX() const { return cursor[0]; }
    [[nodiscard]] double cursorY() const { return cursor[1]; }
    [[nodiscard]] double cursorDeltaX() const { return cursorDelta[0]; }
    [[nodiscard]] double cursorDeltaY() const { return cursorDelta[1]; }
    [[nodiscard]] double scrollDelta() const { return scroll; }
    /// Any pad at all, or one named pad. The bare form is what a "press any button" prompt
    /// wants and is what this always meant.
    [[nodiscard]] bool gamepadConnected() const;
    [[nodiscard]] bool gamepadConnected(uint32_t pad) const;
    [[nodiscard]] uint32_t padCount() const { return static_cast<uint32_t>(pads.size()); }

    // ------------------------------------------------------- rebinding (S1.4)
    /// Arm the capture: the next physical control pressed becomes this action's
    /// binding. `replace` clears the existing list first; otherwise the capture adds
    /// an alternative. Escape cancels and binds nothing -- there has to be a way out
    /// that is not itself bindable.
    void beginCapture(ActionId id, bool replace = true);
    void cancelCapture();
    [[nodiscard]] bool capturing() const { return captureTarget != kInvalidAction; }
    [[nodiscard]] ActionId captureAction() const { return captureTarget; }
    /// True on the frame a capture completed, so a caller can save or redraw.
    [[nodiscard]] bool captureCompleted() const { return captureDone; }

    /// Suppress every keyboard-sourced binding. Set while a text field has focus: a
    /// player typing "was" should not walk. Mouse and pad keep resolving, because
    /// neither of them types.
    void setTextMode(bool on) { textModeOn = on; }
    [[nodiscard]] bool textMode() const { return textModeOn; }
    /// Exempt one action from that suppression. Exactly one kind of action needs it:
    /// the one that closes the field. A key that dismisses a text box cannot be
    /// disabled by the text box being open.
    void setTextModeExempt(ActionId id, bool exempt);

    /**
     * @brief Suppress every *mouse*-sourced binding, the way text mode suppresses keys.
     *
     * Set while the UI has the pointer (S6.4). The problem it solves is the first thing
     * anybody notices about a panel over a 3D view: a world drag and the UI's own click on
     * one button, so dragging a slider spins the world behind it. `Camera.Orbit` was that
     * case until it moved to `Mouse.Middle`; a game binding anything of its own to
     * `Mouse.Left` is the same case again, which is why this is not the camera's to solve.
     *
     * It is here rather than in the camera because the camera is not the only consumer
     * and would not be the last -- and because this is the generalisation S6.4 owed
     * S1.5: `setTextMode` already answered exactly this question for the keyboard, with
     * one exemption for the action that closes the field. This is the same pair for the
     * other device, and `Ui.Click` is the exempt one.
     *
     * Keyboard and pad keep resolving. A panel taking the mouse should not stop WASD.
     */
    void setPointerMode(bool on) { pointerMode = on; }
    [[nodiscard]] bool pointerModeActive() const { return pointerMode; }
    /// Exempt one action from that suppression -- the UI's own click, and nothing else.
    void setPointerModeExempt(ActionId id, bool exempt);

    /// Stick displacement below this reads as zero. Applied by the map rather than the
    /// device layer because it is a preference, and lives in substrate.json.
    float gamepadDeadzone = 0.15f;

  private:
    /// One player's answer for one action. A vector per action rather than a second
    /// action table per player: the name, the bindings and the two exemptions are the
    /// action's and would be copied N times by the other layout.
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
        /// Tombstone. False rows keep their name, bindings and defaults and stop
        /// resolving; nothing is ever erased from `actions`, because an id is its index.
        bool live = true;
    };

    [[nodiscard]] float resolve(const Binding& b, const PlayerDevices& devices, bool textExempt,
                                bool pointerExempt) const;
    [[nodiscard]] bool edgePressed(const Binding& b, const PlayerDevices& devices, bool textExempt,
                                   bool pointerExempt) const;
    /// The one place a player's pad set is walked. `holds` is the membership test the
    /// bitmask exists for, and it is where a pad index past the mask is refused.
    [[nodiscard]] static bool holdsPad(const PlayerDevices& devices, uint32_t pad) {
        return pad < kMaxPads && (devices.pads & (1u << pad)) != 0u;
    }
    bool serviceCapture();

    std::vector<Action> actions;
    /// Config rows nothing has declared yet -- see `setParkedBindings`. Empty in the
    /// common case, and bounded by the config file: `applyBindings` replaces the set
    /// rather than adding to it.
    std::vector<std::pair<std::string, std::string>> parked;
    /// Devices per player. Never empty: `setPlayerCount(0)` still leaves one, because a
    /// map with no players resolves nothing and every existing query would go quiet.
    std::vector<PlayerDevices> players{PlayerDevices{true, kAllPads}};

    // Two sets of press flags, not one. Events accumulate into `*Events` whenever they
    // arrive; `beginFrame` moves them into `*Frame` and clears the accumulator. Without
    // the snapshot, every edge query after `beginFrame` -- which is all of them --
    // would read flags that had just been cleared.
    //
    // There is no matching *release* flag, and that is not an omission: a release can
    // always be seen in the level test, because a control that came up is a control
    // that is no longer down. Only a press can hide inside a single frame.
    bool keys[kKeyCodeCount]{};
    bool keyPressedEvents[kKeyCodeCount]{};
    bool keyPressedFrame[kKeyCodeCount]{};
    bool mouse[kMouseButtonCount]{};
    bool mousePressedEvents[kMouseButtonCount]{};
    bool mousePressedFrame[kMouseButtonCount]{};

    /// One entry per pad the window layer has ever reported, grown on demand and never
    /// shrunk -- a pad that is unplugged reports `connected = false` and keeps its index,
    /// so a player assigned to it is assigned to it again when it comes back.
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
 * **There is one cursor, so this is process state rather than one map's.** A static behind
 * free functions, for the reason `Logger` and `Profiler` are: a static member would say the
 * state belonged to a map, and a second map would be talking about the same physical
 * pointer.
 *
 * Nothing here includes a window, so these record a *desire* and the frame loop applies the
 * platform's cursor mode. The effective grab is `mouseGrabbed() && !uiOpen && the window
 * having focus`, which is what keeps a panel opening mid-drag from leaving the pointer
 * captured -- the desire survives it and nothing has to re-assert it afterwards.
 *
 * No refcounting: one boolean, last writer wins.
 */
void mouseGrab();
void mouseRelease();
[[nodiscard]] bool mouseGrabbed();
/// Drop the desire and anything else the process holds about the pointer. `mouseRelease` is
/// a frame saying it is done with it; this is a program starting over, and exists because
/// the unit suite links these sources and would otherwise carry a grab between tests.
void mouseGrabReset();

/**
 * @brief A deterministic feed of action presses, played into an `InputMap` by frame (C16).
 *
 * `--input-script 60:Game.Save,90:Camera.Forward+,150:Camera.Forward-`. A step names a
 * frame, an action, and whether that action goes down or up; `apply` turns it into the
 * event the device behind that action's binding would have produced and hands it to the
 * map's ordinary event feed. **Nothing here reaches past that feed.** Text mode still
 * suppresses a scripted key, the deadzone still applies to a scripted stick, a scripted
 * press and release inside one frame still reads as the tap `beginFrame` documents, and
 * a rebind still moves what a script drives. A feed that set `pressed` directly would be
 * a test of the feed.
 *
 * It exists because a test could not press a key. C6 could unit-test its own refusals 32
 * ways and could not drive a save through the key bound to it, and the binding table has
 * 49 actions with nothing walking one from an event to a game reading `pressed()`.
 *
 * The consumers are tests, so the design constraint is the one `--camera-spin` already
 * answers: **frame N has to be the same frame N on every run.** Steps are addressed by
 * frame index and never by elapsed time, and `apply` is const because a scripted frame is
 * a function of its index and the step list alone -- so a frame applied twice is a frame
 * applied once, and a run cannot drift from a missed call.
 *
 * **The first binding is the one driven.** An action may hold several and something has
 * to choose; the first is what the binding menu lists first and what a rebind replaces.
 * An action holding none is skipped, because there is no device to stand in for.
 */
class Script {
  public:
    /// One event: on `frame`, `action` goes down or up.
    struct Step {
        uint64_t frame = 0;
        std::string action;
        bool down = false;
        /// Which pad a pad-bound step drives (C26). A script says `@1` to script the
        /// second player's stick; keys and mouse buttons ignore it, because the keyboard
        /// is one device however many people are sharing it.
        uint32_t pad = 0;
    };

    /**
     * @brief Parse `<frame>:<action>[+|-][@<pad>]` steps, separated by commas or
     *        whitespace.
     *
     * `+` presses, `-` releases, and a bare action is both on the same frame -- the tap
     * most actions are actually used as, and the one case a level test cannot see. The
     * suffix is unambiguous because an action name never ends in one; a *binding* name
     * does (`Pad.LeftY-`), which is a second reason this addresses actions.
     *
     * `@<pad>` names the gamepad a pad-bound step drives, and defaults to 0 (C26). It
     * goes after the edge because the edge is part of the action's spelling and the pad is
     * not: `30:Player.Forward+@1` reads as "player two pushes forward" rather than as an
     * action nobody declared.
     *
     * Commas so that a script needs no shell quoting, whitespace so that one pasted out
     * of a log still parses.
     *
     * **All or nothing**, and replaces whatever was here. A malformed step logs and
     * leaves the script untouched: this is test infrastructure, and one that half-ran
     * would report a result against whichever half it managed to press.
     */
    bool parse(std::string_view text);

    void add(uint64_t frame, std::string action, bool down, uint32_t pad = 0);

    [[nodiscard]] bool empty() const { return list.empty(); }
    [[nodiscard]] const std::vector<Step>& steps() const { return list; }
    /// Highest frame any step names, or zero for an empty script -- what a run has to
    /// outlast for the script to have happened at all.
    [[nodiscard]] uint64_t lastFrame() const;

    /**
     * @brief Names the script uses that no declared action claims.
     *
     * A query rather than a refusal, for the reason `conflicts()` is one: the caller is
     * what knows. `Engine` logs it once every action has been declared, which is the only
     * moment the answer means anything -- and it is logged at all because a typo whose
     * first symptom is silence at frame 500 has already cost the run it was written to
     * make repeatable.
     */
    [[nodiscard]] std::vector<std::string> unknownActions(const InputMap& map) const;

    /**
     * @brief Feed every step scheduled for `frame` into `map`. Call before `beginFrame`.
     *
     * A name no action claims is skipped in silence; `unknownActions` is where that is
     * reported, once, rather than on the frame it would have fired.
     *
     * A script holding a pad-bound step takes *that pad* over for the whole run: a pad
     * is pushed as a whole state rather than as events, so the state is rebuilt from
     * every pad step up to `frame` and set each time. A script with no pad step never
     * touches one, which is what leaves a real pad working beside one -- and a script
     * that names only pad 1 leaves pad 0 alone.
     */
    void apply(InputMap& map, uint64_t frame) const;

  private:
    std::vector<Step> list;
};

/**
 * @brief An editable UTF-8 string fed by character events (S1.5).
 *
 * Characters arrive from the platform's character callback, already resolved through
 * the keyboard layout and any IME, and are inserted at the cursor. Editing keys --
 * backspace, delete, the arrows, home and end -- arrive as keys, because there is no
 * character for "delete the previous one".
 *
 * **Repeat is ours, not the platform's.** The character callback already repeats at
 * the system rate, so held letters repeat for free; nothing repeats a backspace, so
 * this class does, at a stated delay and rate driven by the frame delta. That also
 * makes it testable, which a repeat sourced from the window system is not.
 *
 * The cursor is a byte offset that always sits on a UTF-8 boundary: every move and
 * every delete steps over a whole codepoint, so a multi-byte character cannot be cut
 * in half by a backspace.
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

    /// Consumed on read: Enter and Escape are events, and an event that stays true
    /// fires its handler on every frame after the first.
    bool takeSubmitted();
    bool takeCancelled();

    /// 0 means unbounded, which is the default. A field that wants a limit sets one
    /// and gets `overflowed()` when it is hit, rather than characters vanishing.
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
    /// Time the key has been held, and how many repeats that has already produced.
    /// Counting from the press rather than subtracting a countdown keeps the Nth
    /// repeat at `delay + N * rate` exactly, instead of at the sum of N roundings.
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
 * Every action keeps its declared default until a row names it, so a config holding
 * one rebind rebinds one action. Rows resolve through `findDeclared`, so an action the
 * game retired before this ran is rebound rather than treated as missing.
 *
 * A row that resolves to nothing at all is **parked rather than dropped** -- see
 * `InputMap::setParkedBindings`, and note that the map's parked set is *replaced* by this
 * call, so a second table cannot be shadowed by the first one's leftovers. The count
 * below is rows applied to a declared action; a parked row is applied later, by the
 * `declare` that first names it, and is counted by nothing.
 *
 * @return how many rows were applied.
 */
uint32_t applyBindings(InputMap& map, const std::vector<std::pair<std::string, std::string>>& table);

/**
 * @brief Rewrite `input.bindings` in an existing config file, in place (S1.2).
 *
 * Only actions whose bindings differ from the ones they were declared with are
 * written, so the file records what the user changed rather than a snapshot of every
 * default -- and a default that moves in a later build still reaches anyone who never
 * rebound it.
 *
 * There is one reader (Config) and one writer (this), and they are in different files
 * for a reason: the reader parses a whole document into a struct, and the writer has
 * to preserve every key the reader never looked at. So this re-parses the file, swaps
 * one object, and writes it back. The cost is that the file comes back pretty-printed
 * by rapidjson rather than in its hand-written layout; the alternative is a save that
 * deletes settings it did not understand.
 *
 * A file that does not exist yet is created holding only the input section.
 */
bool saveBindings(const InputMap& map, const std::string& configPath);

} // namespace input

} // namespace core
