#pragma once

namespace core {

/**
 * @file engine/core/InputGlfw.h
 * @brief The one place input meets the window system.
 *
 * Keyboard and mouse arrive as callbacks `Engine` forwards straight into `InputMap` -- the
 * codes are identical, so there is nothing to translate. Gamepads have no callback and must
 * be polled, which is what this is for.
 *
 * The static_asserts holding `input::Key` and friends to GLFW's numbering live in the
 * matching .cpp, and are what lets the rest of the engine cast rather than convert.
 */
namespace input {

class InputMap;

/// @brief Poll every connected gamepad and hand each one's state to `map` separately.
///
/// Triggers are rescaled from GLFW's [-1,1] resting-at--1 convention to [0,1], so "not
/// pressed" is zero on every axis and the deadzone means the same thing everywhere.
void pollGamepads(InputMap& map);

} // namespace input

} // namespace core
