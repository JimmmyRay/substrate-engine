#include "Engine.h"
#include "Entry.h"
#include "Game.h"

#include "core/Paths.h"

#include <memory>

/**
 * @file engine/Entry.cpp
 * @brief `main()`, compiled only when SUBSTRATE_ENTRY_POINT is ON.
 *
 * A false from `init` has already torn down everything it built; pairing it with a
 * `shutdown()` tears the same things down twice.
 */
int main(int argc, char** argv) {
    // Before anything can call `executableDir()`, which caches its answer on first use --
    // seeding it later leaves the cached fallback wrong for the rest of the process.
    core::seedExecutablePath(argc > 0 ? argv[0] : nullptr);

    // Constructed before `Engine::init`, which calls `Game::configure` for the scene,
    // gravity and the mix graph before it builds the subsystems they configure.
    std::unique_ptr<Game> game = substrateCreateGame();

    Engine engine;
    if (!engine.init(argc, argv, *game)) return engine.exitCode();

    const int code = engine.run(*game);

    engine.shutdown();
    return code;
}
