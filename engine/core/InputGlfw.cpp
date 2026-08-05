#include "core/InputGlfw.h"

#include "core/Input.h"
#include "core/Logger.h"

#include <GLFW/glfw3.h>

#include <cmath>

namespace core {

namespace input {
namespace {

/// Every code in Input.h, checked against the macro it claims to equal. Removing one makes
/// a mismatch a key that quietly stops working instead of a build failure -- and these are
/// the entire reason the header may hard-code GLFW's numbers.
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
    // One call per pad, and nothing merges them: folding the pads into a single state
    // throws away which one acted, which makes local co-op inexpressible.
    //
    // The joystick id is the pad index the map holds, so a player assigned to pad 1 keeps
    // the same physical pad across an unplug and a reconnect. An empty slot is reported as
    // disconnected rather than skipped, or a pad unplugged mid-run keeps answering with the
    // state it had when it left.
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
        // Only as far as the map has already seen. Growing to sixteen slots on a machine
        // with one pad makes every resolve walk fifteen empty entries for ever.
        if (state.connected || index < map.padCount()) map.setGamepad(state, index);
    }
}

} // namespace input

} // namespace core
