#pragma once

namespace core {

/**
 * @file engine/core/InputGlfw.h
 * @brief The one place input meets the window system (S1.3).
 *
 * Keyboard and mouse arrive as callbacks, which `Engine` already owns and forwards
 * straight into `InputMap` -- the codes are identical, so there is nothing to
 * translate. Gamepads have no callback and must be polled, which is what this is for.
 *
 * The static_asserts that hold `input::Key` and friends to GLFW's numbering live in
 * the matching .cpp, and are the reason the rest of the engine can cast rather than
 * convert.
 */
namespace input {

class InputMap;

/**
 * @brief Poll every connected gamepad and hand the result to `map`.
 *
 * **Scale: generalized.** All sixteen of GLFW's joystick slots are read and merged
 * into one state -- buttons OR together, axes take whichever pad has pushed furthest
 * from centre. So one pad, four pads and none are the same code path, and a second
 * player is a scene-level question about which actions to route rather than a second
 * input path here.
 *
 * Triggers are rescaled from GLFW's [-1,1] resting-at--1 convention to [0,1], so that
 * "not pressed" is zero on every axis and a deadzone means the same thing everywhere.
 */
void pollGamepads(InputMap& map);

} // namespace input

} // namespace core
