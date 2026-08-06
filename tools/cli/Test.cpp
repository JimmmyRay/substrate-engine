#include "Run.h"

#include "Build.h"
#include "BuildLock.h"
#include "Process.h"
#include "Repo.h"

#include <algorithm>
#include <cstdio>

namespace tool {
namespace {

namespace fs = std::filesystem;

} // namespace

int cmdTest(const std::vector<std::string>& args) {
    Config config = Config::Debug;
    size_t first = 0;

    if (!args.empty() && args[0] != "--") {
        if (args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
            std::fputs("usage: substrate test [debug|release|asan|tsan] -- [--gtest_filter=...]\n",
                       stderr);
            return 0;
        }
        if (const std::optional<Config> parsed = parseConfig(args[0])) {
            config = *parsed;
            first = 1;
        } else {
            // Without this branch `test releas` built debug, passed `releas` to the test
            // binary as a gtest argument, and reported a green suite for a configuration
            // nobody ran.
            std::fprintf(stderr, "error: unknown argument '%s' (want: %s)\n", args[0].c_str(),
                         configList().c_str());
            std::fputs("       Everything meant for the binary goes after '--'.\n", stderr);
            return 1;
        }
    }
    if (first < args.size() && args[first] == "--") ++first;
    const std::vector<std::string> rest(args.begin() + static_cast<long>(first), args.end());

    if (const std::string why = configUnsupported(config); !why.empty()) {
        std::fprintf(stderr, "error: %s\n", why.c_str());
        return 1;
    }

    const fs::path dir = buildDir(config);

    // Carried through rather than cleared, because a build with no game named clears the
    // cache variable -- and running the tests must not silently un-configure the game a
    // build directory holds. This builds one target and changes nothing else about it.
    const std::string game = cachedGame(config);

    BuildLock lock;
    if (!lock.acquire(dir)) return 1;

    std::vector<std::string> buildArgs{name(config), "--target", "substrate_tests"};
    if (const int code = cmdBuild(buildArgs, game); code != 0) return code;

    const fs::path binary = dir / executableName("substrate_tests");
    std::error_code ec;
    if (!fs::exists(binary, ec)) {
        std::fprintf(stderr, "error: %s does not exist after building.\n",
                     binary.generic_string().c_str());
        return 1;
    }

    // Any Python suite still in tests/, before the C++ one and only when no gtest argument
    // was given -- a `--gtest_filter` says the caller is after one C++ case and does not
    // want a second suite's output in front of it.
    //
    // Skipped rather than failed when python3 is absent: python is not a build dependency
    // of the engine, and refusing to run the C++ tests without it would make it one.
    if (rest.empty()) {
        std::vector<fs::path> suites;
        for (const auto& entry : fs::directory_iterator(repoRoot() / "tests", ec)) {
            const std::string file = entry.path().filename().string();
            if (file.size() > 8 && file.compare(file.size() - 8, 8, "_test.py") == 0) {
                suites.push_back(entry.path());
            }
        }
        std::sort(suites.begin(), suites.end());

        const fs::path python = pythonExe();
        if (!suites.empty() && python.empty()) {
            std::fputs("warning: no python interpreter found; skipping the python suites\n", stderr);
        } else {
            for (const fs::path& suite : suites) {
                std::fprintf(stderr, "==> %s\n",
                             suite.lexically_relative(repoRoot()).generic_string().c_str());
                RunOptions pyOptions;
                pyOptions.cwd = repoRoot();
                pyOptions.inherit = true;
                if (const RunResult r = run({python.string(), suite.string()}, pyOptions); !r.ok()) {
                    return r.exitCode ? r.exitCode : 1;
                }
            }
        }
    }

    const Launch launch = sanitizerLaunch(config);

    std::vector<std::string> command = launch.prefix;
    command.push_back(binary.string());
    for (const std::string& arg : rest) command.push_back(arg);

    lock.release();

    RunOptions options;
    options.cwd = repoRoot();
    options.inherit = true;
    options.env = launch.env;
    execOrExit(command, options);
}

} // namespace tool
