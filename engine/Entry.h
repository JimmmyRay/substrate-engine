#pragma once

#include "Game.h"

#include <memory>

/**
 * @file engine/Entry.h
 * @brief The whole of a game's entry point.
 *
 * ```cpp
 * // game/demo/DemoGame.cpp
 * SUBSTRATE_GAME(DemoGame)
 * ```
 *
 * The macro is the only thing connecting a game class to the engine's `main()`. Turning the
 * `SUBSTRATE_ENTRY_POINT` CMake option off leaves the game to write its own `main()` and
 * drive `Engine` by hand.
 */
std::unique_ptr<Game> substrateCreateGame();

#define SUBSTRATE_GAME(T)                                       \
    std::unique_ptr<Game> substrateCreateGame() {               \
        return std::make_unique<T>();                           \
    }
