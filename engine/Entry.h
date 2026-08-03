#pragma once

#include "Game.h"

#include <memory>

/**
 * @file engine/Entry.h
 * @brief The whole of a game's entry point (G1).
 *
 * ```cpp
 * // game/demo/DemoGame.cpp
 * SUBSTRATE_GAME(DemoGame)
 * ```
 *
 * `SUBSTRATE_ENTRY_POINT` is a CMake option, ON by default. With it on the engine library
 * compiles `main()` (see `engine/Entry.cpp`) and the macro below is the only thing
 * connecting a game class to it. With it off, the game writes its own `main()` and drives
 * `Engine` by hand -- which is the shape `scripts/baseline.py` and any tool wanting to
 * interleave work between the frame's phases would take.
 *
 * The indirection is a free function rather than a registry, a factory interface or a
 * static initialiser that appends to a list. One game per executable is the whole
 * requirement, and a link error naming `substrateCreateGame` is a better diagnostic for
 * "you forgot the macro" than an empty list discovered at runtime.
 */
std::unique_ptr<Game> substrateCreateGame();

#define SUBSTRATE_GAME(T)                                       \
    std::unique_ptr<Game> substrateCreateGame() {               \
        return std::make_unique<T>();                           \
    }
