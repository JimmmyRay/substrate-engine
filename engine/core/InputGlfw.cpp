#include "core/InputGlfw.h"

#include "core/Input.h"
#include "core/Logger.h"

#include <GLFW/glfw3.h>

#include <cmath>

namespace core {

namespace input {
namespace {

/// Every code in Input.h, checked against the macro it claims to equal. A mismatch is
/// a build failure here rather than a key that quietly stops working at runtime --
/// which is the entire reason the header is allowed to hard-code GLFW's numbers.
#define SUBSTRATE_ASSERT_CODE(name, value, glfwMacro)                                                                  \
    static_assert(static_cast<int>(Key::name) == (glfwMacro), "input::Key::" #name " no longer matches " #glfwMacro);
SUBSTRATE_KEY_LIST(SUBSTRATE_ASSERT_CODE)
#undef SUBSTRATE_ASSERT_CODE

#define SUBSTRATE_ASSERT_CODE(name, value, glfwMacro)                                                                  \
    static_assert(static_cast<int>(MouseButton::name) == (glfwMacro),                                                  \
                  "input::MouseButton::" #name " no longer matches " #glfwMacro);
SUBSTRATE_MOUSE_LIST(SUBSTRATE_ASSERT_CODE)
#undef SUBSTRATE_ASSERT_CODE

#define SUBSTRATE_ASSERT_CODE(name, value, glfwMacro)                                                                  \
    static_assert(static_cast<int>(PadButton::name) == (glfwMacro),                                                    \
                  "input::PadButton::" #name " no longer matches " #glfwMacro);
SUBSTRATE_PAD_BUTTON_LIST(SUBSTRATE_ASSERT_CODE)
#undef SUBSTRATE_ASSERT_CODE

#define SUBSTRATE_ASSERT_CODE(name, value, glfwMacro)                                                                  \
    static_assert(static_cast<int>(PadAxis::name) == (glfwMacro),                                                      \
                  "input::PadAxis::" #name " no longer matches " #glfwMacro);
SUBSTRATE_PAD_AXIS_LIST(SUBSTRATE_ASSERT_CODE)
#undef SUBSTRATE_ASSERT_CODE

// The state struct is indexed by our own counts, so those have to match too.
static_assert(kPadButtonCount == GLFW_GAMEPAD_BUTTON_LAST + 1, "gamepad button count drifted from GLFW");
static_assert(kPadAxisCount == GLFW_GAMEPAD_AXIS_LAST + 1, "gamepad axis count drifted from GLFW");
static_assert(kMouseButtonCount == GLFW_MOUSE_BUTTON_LAST + 1, "mouse button count drifted from GLFW");
static_assert(kKeyCodeCount == GLFW_KEY_LAST + 1, "key code space drifted from GLFW");

bool isTrigger(int axis) {
    return axis == GLFW_GAMEPAD_AXIS_LEFT_TRIGGER || axis == GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER;
}

} // namespace

void pollGamepads(InputMap& map) {
    // **One call per pad, and nothing merges them** (C26). This loop used to fold every
    // connected pad into a single state -- any button on any pad read as pressed, and each
    // axis took the largest magnitude across all of them -- so it enumerated the devices
    // and then deliberately threw away which one had acted. Local co-op was not hard, it
    // was inexpressible.
    //
    // The joystick id is the pad index the map holds, so a player assigned to pad 1 is
    // assigned to the same physical pad across an unplug and a reconnect. A slot with no
    // gamepad in it is reported as disconnected rather than skipped, or a pad unplugged
    // mid-run would keep answering with the state it had when it left.
    uint32_t index = 0;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST && index < kMaxPads; ++jid, ++index) {
        GamepadState state;
        GLFWgamepadstate raw;
        if (glfwJoystickIsGamepad(jid) == GLFW_TRUE && glfwGetGamepadState(jid, &raw) == GLFW_TRUE) {
            state.connected = true;
            for (int b = 0; b < kPadButtonCount; ++b) state.buttons[b] = raw.buttons[b] == GLFW_PRESS;
            for (int a = 0; a < kPadAxisCount; ++a) {
                state.axes[a] = isTrigger(a) ? (raw.axes[a] + 1.0f) * 0.5f : raw.axes[a];
            }
            if (!map.gamepadConnected(index)) Logger::debug(LogCategory::Input, "Gamepad %d: %s", jid, glfwGetGamepadName(jid));
        }
        // Only as far as the map has already seen: growing to sixteen slots on a machine
        // with one pad would make every resolve walk fifteen empty entries for ever.
        if (state.connected || index < map.padCount()) map.setGamepad(state, index);
    }
}

} // namespace input

} // namespace core
