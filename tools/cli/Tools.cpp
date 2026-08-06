#include "Tools.h"

#include "Build.h"
#include "Process.h"
#include "Repo.h"

#include <cstdio>

namespace tool {
namespace {

namespace fs = std::filesystem;

/// Build the named tool target and hand it the rest of the arguments.
///
/// Built every time, for the reason `run` rebuilds every time: what you bake with is what
/// the tree currently says. A sidecar written by a stale binary is the exact failure the
/// scene format's version and layout digest exist to catch, and catching it at the next
/// launch is later than catching it here.
int buildThenExec(const char* target, const std::vector<std::string>& args, const char* usage) {
    Config config = Config::Release;
    size_t first = 0;

    if (!args.empty()) {
        if (args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
            std::fputs(usage, stderr);
            return 0;
        }
        if (const std::optional<Config> parsed = parseConfig(args[0])) {
            config = *parsed;
            first = 1;
        }
    }
    if (first >= args.size()) {
        std::fputs(usage, stderr);
        return 1;
    }

    // Passed back in rather than left to default, because a bare build clears SUBSTRATE_GAME
    // from the cache -- that is what makes it mean "engine and tests only". Baking a scene is
    // not a reason to make the next `run` reconfigure and relink.
    const std::string game = cachedGame(config);

    std::vector<std::string> buildArgs{name(config), "--target", target};
    if (const int code = cmdBuild(buildArgs, game); code != 0) return code;

    const fs::path binary = buildDir(config) / executableName(target);
    std::error_code ec;
    if (!fs::exists(binary, ec)) {
        std::fprintf(stderr, "error: %s does not exist after building.\n",
                     binary.generic_string().c_str());
        return 1;
    }

    std::vector<std::string> command{binary.string()};
    command.insert(command.end(), args.begin() + static_cast<long>(first), args.end());

    RunOptions options;
    options.cwd = repoRoot();
    options.inherit = true;
    execOrExit(command, options);
}

} // namespace

int cmdBake(const std::vector<std::string>& args) {
    return buildThenExec("substrate-bake", args,
                         "usage: substrate bake [config] <scene.gltf> ...\n"
                         "\n"
                         "Writes the .scene sidecar beside each document. No device, no window.\n");
}

int cmdSim(const std::vector<std::string>& args) {
    return buildThenExec("substrate-sim", args,
                         "usage: substrate sim [config] <scene.gltf> [--steps N] [--gravity G]\n"
                         "\n"
                         "Steps the simulation with no Vulkan device and no window.\n");
}

} // namespace tool
