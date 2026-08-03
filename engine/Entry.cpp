#include "Engine.h"
#include "Entry.h"
#include "Game.h"

#include "core/Paths.h"

#include <memory>

/**
 * @file engine/Entry.cpp
 * @brief `main()`, compiled only when SUBSTRATE_ENTRY_POINT is ON.
 *
 * Eight lines, and that is the point: everything it used to hold is now either in
 * `Engine` or in a game. `init` tears down whatever it built before returning false, so
 * there is no shutdown to pair with a failed start.
 */
int main(int argc, char** argv) {
    // First, and before Engine::init: executableDir() caches its answer on first use, so
    // the fallback has to be in place before anything can ask. Only used where the
    // platform lookup fails, which on Linux means /proc is not mounted.
    core::seedExecutablePath(argc > 0 ? argv[0] : nullptr);

    // The game is created before the engine is initialised, not after: `Engine::init`
    // calls `Game::configure` to learn the scene, gravity and the mix graph, and it needs
    // all three before it builds the things they configure (S1).
    std::unique_ptr<Game> game = substrateCreateGame();

    Engine engine;
    if (!engine.init(argc, argv, *game)) return engine.exitCode();

    const int code = engine.run(*game);

    engine.shutdown();
    return code;
}
