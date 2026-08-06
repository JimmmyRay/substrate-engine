#include "Bench.h"
#include "Build.h"
#include "Harness.h"
#include "Manifest.h"
#include "Rdoc.h"
#include "Repo.h"
#include "Run.h"
#include "Setup.h"
#include "Tools.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/**
 * @file tools/cli/main.cpp
 * @brief `substrate` -- the build and harness tooling, as one program.
 *
 * A flat table of functions, not a Subcommand class with a virtual run(). The table is the
 * dispatch and the help text both, so a command that exists and a command that is documented
 * cannot drift apart.
 *
 * What is deliberately not here: configuring and building this program. Something has to run
 * cmake before any of this exists, which is what scripts/build.sh and scripts/build.cmd are
 * and the only reason either still exists.
 */
namespace {

using tool::cmdArena;
using tool::cmdBake;
using tool::cmdBench;
using tool::cmdBuild;
using tool::cmdBuildGame;
using tool::cmdFetchAssets;
using tool::cmdGolden;
using tool::cmdLocomotion;
using tool::cmdManifest;
using tool::cmdNewGame;
using tool::cmdPerfgate;
using tool::cmdInstallHooks;
using tool::cmdRdoc;
using tool::cmdReadback;
using tool::cmdRun;
using tool::cmdSim;
using tool::cmdTest;

int cmdBuildEngine(const std::vector<std::string>& args) { return cmdBuild(args, ""); }

struct Command {
    const char* name;
    int (*run)(const std::vector<std::string>&);
    const char* summary;
};

constexpr Command kCommands[] = {
    {"build", cmdBuildEngine, "the engine and the unit suite; produces no runnable binary"},
    {"build-game", cmdBuildGame, "the engine, plus one game under game/<name>/"},
    {"run", cmdRun, "build a game and launch it"},
    {"test", cmdTest, "build and run the unit suite"},
    {"bench", cmdBench, "per-pass GPU and CPU cost, read from the trace"},
    {"perfgate", cmdPerfgate, "fail if the frame got slower than perf-budget.json"},
    {"fetch-assets", cmdFetchAssets, "sparse-clone Sponza and generate the derived assets"},
    {"new-game", cmdNewGame, "scaffold game/<name>/ from the template"},
    {"golden", cmdGolden, "golden-image regression; snap or check"},
    {"readback", cmdReadback, "present a known PNG and compare the swapchain bit-exactly"},
    {"locomotion", cmdLocomotion, "drive game/demo through nine scripted arms"},
    {"arena", cmdArena, "drive game/battle_arena through eight scripted arms"},
    {"manifest", cmdManifest, "every file a packaged game needs, as src -> dest"},
    {"bake", cmdBake, "write a scene's .scene sidecar; no device, no window"},
    {"rdoc", cmdRdoc, "RenderDoc capture and analysis"},
    {"install-hooks", cmdInstallHooks, "write the pre-commit perf gate"},
    {"sim", cmdSim, "step a scene with no Vulkan device and no window"},
};

int usage(int exitCode) {
    std::FILE* stream = exitCode == 0 ? stdout : stderr;
    std::fputs("substrate -- Substrate's build and harness tooling\n"
               "\n"
               "usage: substrate <command> [args]\n"
               "\n",
               stream);
    for (const Command& command : kCommands) {
        std::fprintf(stream, "  %-12s %s\n", command.name, command.summary);
    }
    std::fputs("\n`substrate <command> --help` for one command's arguments.\n", stream);
    return exitCode;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage(2);
    if (args[0] == "-h" || args[0] == "--help" || args[0] == "help") return usage(0);

    if (tool::repoRoot().empty()) {
        std::fputs("error: no Substrate checkout here. Run this from inside one.\n", stderr);
        return 2;
    }

    for (const Command& command : kCommands) {
        if (args[0] != command.name) continue;
        return command.run({args.begin() + 1, args.end()});
    }

    std::fprintf(stderr, "error: unknown command '%s'\n\n", args[0].c_str());
    return usage(2);
}
