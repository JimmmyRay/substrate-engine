#include "Run.h"

#include "Build.h"
#include "BuildLock.h"
#include "Process.h"
#include "Repo.h"

#include <cstdio>

namespace tool {
namespace {

namespace fs = std::filesystem;

constexpr const char* kUsage =
    "usage: substrate run [<game>] [debug|release|asan|tsan] -- [scene.gltf] [options]\n"
    "\n"
    "The two leading arguments are a game name and a configuration, in either order, and\n"
    "both are optional. Everything after '--' is passed straight to the executable.\n";

/// Naming no game opens the engine's own test scene rather than a game's, which is what
/// makes a bare `run` on a fresh clone do something rather than explain itself.
///
/// It used to be whichever game the build directory happened to hold, and that made `run`
/// and the golden suite mean different things on different days: a game builds its own
/// world in `init`, so a suite that named no game got that world in its baselines.
constexpr const char* kViewerGame = "viewer";

/// Sponza rather than one of the small generated scenes, because it is what the golden
/// suite pins and what the renderer is tuned against.
constexpr const char* kEngineScene = "engine/assets/Sponza/glTF/Sponza.gltf";

bool namesAScene(const std::string& arg) {
    const auto endsWith = [&arg](std::string_view suffix) {
        return arg.size() >= suffix.size() &&
               arg.compare(arg.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return endsWith(".gltf") || endsWith(".glb") || arg.rfind("res:/", 0) == 0;
}

} // namespace

int cmdRun(const std::vector<std::string>& args) {
    Invocation invocation;
    int exitCode = 0;
    if (!parseInvocation(args, invocation, exitCode, kUsage)) return exitCode;

    if (const std::string why = configUnsupported(invocation.config); !why.empty()) {
        std::fprintf(stderr, "error: %s\n", why.c_str());
        return 1;
    }

    // Before the build, not after it. The engine refuses `--record` without ffmpeg too, but
    // it does so once the window is open and several minutes of compiling have gone by.
    for (const std::string& arg : invocation.rest) {
        if (arg == "--record" && which("ffmpeg").empty()) {
            std::fputs("error: --record needs ffmpeg on PATH.\n", stderr);
            std::fputs("       Debian/Ubuntu: sudo apt install ffmpeg\n", stderr);
            std::fputs("       Fedora:        sudo dnf install ffmpeg\n", stderr);
            std::fputs("       Windows:       winget install Gyan.FFmpeg\n", stderr);
            return 1;
        }
    }

    std::string game = invocation.game;
    if (game.empty()) {
        if (!isGame(kViewerGame)) {
            std::fprintf(stderr,
                         "error: game/%s is what `substrate run` opens when no game is named,\n"
                         "       and it is not in the tree. Name one:\n", kViewerGame);
            printGames(stderr);
            return 1;
        }
        game = kViewerGame;
    }

    const fs::path dir = buildDir(invocation.config);
    const fs::path binary = dir / executableName(game);

    if (cachedGame(invocation.config) != game) {
        std::fprintf(stderr, "==> %s does not hold %s; switching it\n",
                     dir.lexically_relative(repoRoot()).generic_string().c_str(), game.c_str());
    }

    // Taken here rather than inside the build, and released only after the check below,
    // because the check is inside the window it protects: a concurrent link unlinks the
    // binary before rewriting it.
    BuildLock lock;
    if (!lock.acquire(dir)) return 1;

    std::vector<std::string> buildArgs{game, name(invocation.config)};
    if (const int code = cmdBuildGame(buildArgs); code != 0) return code;

    std::error_code ec;
    if (!fs::exists(binary, ec)) {
        std::fprintf(stderr, "error: %s still does not exist after building.\n",
                     binary.generic_string().c_str());
        return 1;
    }

    // A scene named after `--` wins over the default. Recognised by extension rather than
    // by position, because the flags after `--` are order-free and the golden suite puts
    // its scene last -- without this, a case that names its own scene would get two.
    bool hasScene = false;
    for (const std::string& arg : invocation.rest) hasScene = hasScene || namesAScene(arg);

    std::vector<std::string> sceneArg;
    if (!invocation.namedGame && !hasScene) {
        if (!fs::is_regular_file(repoRoot() / kEngineScene, ec)) {
            std::fputs("error: Sponza is missing, and it is what `substrate run` opens when no\n"
                       "       game is named. Run: substrate fetch-assets\n"
                       "       Or name a game (substrate run --list) or a scene.\n", stderr);
            return 1;
        }
        sceneArg.push_back(kEngineScene);
    }

    // VK_LAYER_PATH is deliberately left alone. VulkanContext::init detects the case where
    // it hides the system layers and appends the standard directories itself, so validation
    // works no matter how the binary is launched -- not only through here.
    const Launch launch = sanitizerLaunch(invocation.config);

    fs::create_directories(repoRoot() / "debug_frames", ec);

    std::vector<std::string> command = launch.prefix;
    command.push_back(binary.string());
    for (const std::string& arg : sceneArg) command.push_back(arg);
    for (const std::string& arg : invocation.rest) command.push_back(arg);

    std::string echo;
    for (const std::string& arg : command) echo += (echo.empty() ? "" : " ") + arg;
    std::fprintf(stderr, "==> %s\n", echo.c_str());

    // Dropped before the run: a lock held across a 120-second capture blocks every other
    // build in the tree for its duration.
    lock.release();

    RunOptions options;
    options.cwd = repoRoot();
    options.inherit = true;
    options.env = launch.env;
    execOrExit(command, options);
}

} // namespace tool
