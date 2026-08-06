#include "Build.h"

#include "BuildLock.h"
#include "Process.h"
#include "Repo.h"

#include <cstdio>
#include <fstream>

namespace tool {
namespace {

namespace fs = std::filesystem;

/// Submodules must exist before CMake runs. The guard in CMakeLists.txt fires otherwise,
/// but catching it here gives a shorter path to the fix.
void ensureSubmodules() {
    static const char* const kProbes[] = {"external/glfw/CMakeLists.txt",
                                          "external/fastgltf/CMakeLists.txt",
                                          "external/meshoptimizer/CMakeLists.txt"};
    for (const char* probe : kProbes) {
        std::error_code ec;
        if (fs::is_regular_file(repoRoot() / probe, ec)) continue;

        std::fputs("==> initialising submodules\n", stderr);
        RunOptions options;
        options.cwd = repoRoot();
        options.inherit = true;
        run({"git", "submodule", "update", "--init", "--recursive"}, options);
        return;
    }
}

std::vector<std::string> configureArgs(Config config) {
    switch (config) {
    case Config::Debug:
        return {"-DCMAKE_BUILD_TYPE=Debug"};
    case Config::Release:
        return {"-DCMAKE_BUILD_TYPE=Release"};
    case Config::Asan:
        return {"-DCMAKE_BUILD_TYPE=Debug",
                "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g",
                "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"};
    case Config::Tsan:
        return {"-DCMAKE_BUILD_TYPE=Debug",
                "-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -g",
                "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"};
    }
    return {};
}

/// clangd and other tooling look for this at the repository root.
///
/// A copy rather than a link on Windows, where a symlink needs either developer mode or an
/// elevated process. It goes stale the moment a target is added, which is why the link is
/// preferred wherever one can be made.
void publishCompileCommands(const fs::path& dir) {
    std::error_code ec;
    const fs::path source = dir / "compile_commands.json";
    if (!fs::is_regular_file(source, ec)) return;

    const fs::path target = repoRoot() / "compile_commands.json";
    fs::remove(target, ec);
#ifdef _WIN32
    fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
#else
    fs::create_symlink(fs::relative(source, repoRoot(), ec), target, ec);
#endif
}

} // namespace

int cmdBuild(const std::vector<std::string>& args, const std::string& game) {
    Config config = Config::Debug;
    size_t first = 0;

    if (!args.empty()) {
        if (args[0] == "clean") {
            std::error_code ec;
            fs::remove_all(repoRoot() / "build", ec);
            fs::remove(repoRoot() / "compile_commands.json", ec);
            std::puts("removed build/ and the compile_commands.json link");
            return 0;
        }
        if (const std::optional<Config> parsed = parseConfig(args[0])) {
            config = *parsed;
            first = 1;
        } else if (args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
            std::fputs("usage: substrate build [debug|release|asan|tsan|clean] [cmake args]\n",
                       stderr);
            return 0;
        } else {
            std::fprintf(stderr, "error: unknown config '%s' (want: %s|clean)\n",
                         args[0].c_str(), configList().c_str());
            return 1;
        }
    }

    if (const std::string why = configUnsupported(config); !why.empty()) {
        std::fprintf(stderr, "error: %s\n", why.c_str());
        return 1;
    }

    const std::vector<std::string> extra(args.begin() + static_cast<long>(first), args.end());
    const fs::path dir = buildDir(config);

    ensureSubmodules();

    BuildLock lock;
    if (!lock.acquire(dir)) return 1;

    std::vector<std::string> configure{"cmake", "-B", dir.string(), "-G", "Ninja"};
    for (const std::string& arg : configureArgs(config)) configure.push_back(arg);
    // Passed on every configure, including when it is empty: that is what clears a game a
    // previous `build-game` recorded, so `substrate build` always means engine and tests only.
    configure.push_back("-DSUBSTRATE_GAME=" + game);

    RunOptions options;
    options.cwd = repoRoot();
    options.inherit = true;

    std::fprintf(stderr, "==> configuring %s in %s/\n", name(config),
                 dir.lexically_relative(repoRoot()).generic_string().c_str());
    if (const RunResult r = run(configure, options); !r.ok()) return r.exitCode ? r.exitCode : 1;

    std::vector<std::string> build{"cmake", "--build", dir.string()};
    for (const std::string& arg : extra) build.push_back(arg);

    std::fputs("==> building\n", stderr);
    if (const RunResult r = run(build, options); !r.ok()) return r.exitCode ? r.exitCode : 1;

    if (config == Config::Debug) publishCompileCommands(dir);

    // Asserted, not announced. This line used to print unconditionally, so it claimed a
    // file existed without anyone having looked -- and a golden case then failed on the
    // very next statement because it did not.
    if (!extra.empty()) {
        std::string joined;
        for (const std::string& arg : extra) joined += (joined.empty() ? "" : " ") + arg;
        std::fprintf(stderr, "==> done: %s (%s)\n",
                     dir.lexically_relative(repoRoot()).generic_string().c_str(),
                     joined.c_str());
        return 0;
    }

    const fs::path artifact = game.empty() ? dir / "libsubstrate.a"
                                           : dir / executableName(game);
    std::error_code ec;
    if (!fs::exists(artifact, ec)) {
        std::fprintf(stderr, "error: cmake --build reported success but %s does not exist.\n",
                     artifact.generic_string().c_str());
        return 1;
    }
    std::fprintf(stderr, "==> done: %s%s\n",
                 artifact.lexically_relative(repoRoot()).generic_string().c_str(),
                 game.empty() ? " (no game configured; substrate build-game <name>)" : "");
    return 0;
}

int cmdBuildGame(const std::vector<std::string>& args) {
    const bool asked = !args.empty() &&
                       (args[0] == "-h" || args[0] == "--help" || args[0] == "help");
    if (args.empty() || asked) {
        if (!asked) std::fputs("error: no game named.\n", stderr);
        std::fputs("usage: substrate build-game <name> [debug|release|asan|tsan]\n", stderr);
        std::fputs("games:\n", stderr);
        printGames(stderr);
        return asked ? 0 : 1;
    }
    if (args[0] == "--list" || args[0] == "list") {
        printGames(stdout);
        return 0;
    }
    if (!isGame(args[0])) {
        std::fprintf(stderr, "error: game/%s/CMakeLists.txt does not exist\n", args[0].c_str());
        std::fputs("games:\n", stderr);
        printGames(stderr);
        return 1;
    }

    const std::string game = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    return cmdBuild(rest, game);
}

} // namespace tool
