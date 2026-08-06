#pragma once

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @file tools/cli/Repo.h
 * @brief What every subcommand needs to know about the tree it is standing in.
 *
 * The shell scripts carried five copies of the CMakeCache parser and spelled the
 * configuration set out in seven places. This is the one of each.
 */
namespace tool {

enum class Config { Debug, Release, Asan, Tsan };

const char* name(Config config);

/// Nothing fuzzy: `scripts/test.sh releas` used to build debug and hand `releas` to gtest, and a
/// run that silently used the wrong configuration is indistinguishable from a right one.
std::optional<Config> parseConfig(std::string_view token);

/// "debug|release|asan|tsan", for the message a rejected token gets.
std::string configList();

/// Why this configuration cannot run here, or empty. Sanitizers need a runtime MinGW does
/// not ship, so on Windows the answer is a sentence rather than a broken build.
std::string configUnsupported(Config config);

/// The repository root, found by walking up from the working directory and falling back to
/// the tree this binary was configured from. Every subcommand runs from here, because the
/// scripts did and every relative path in the build assumes it.
const std::filesystem::path& repoRoot();

/// The Python interpreter, or empty when there is none.
///
/// `python3` on POSIX and usually `python` on Windows, where the versioned name is often
/// absent and `python3.exe` may be the App Execution Alias that opens the Microsoft Store
/// rather than an interpreter. Both are tried, in that order.
std::filesystem::path pythonExe();

/// This executable's own path. A harness re-enters it to reach `run` rather than shelling
/// out, so a measured launch and a hand-typed one take the same path into the engine.
const std::filesystem::path& selfPath();

std::filesystem::path buildDir(Config config);

/// `name` on POSIX, `name.exe` on Windows. Every path the build produces goes through this,
/// because a check for a file that the linker spelled differently is a check that fails on
/// one platform for a reason that has nothing to do with the build.
std::string executableName(std::string_view stem);

/// A game is a directory under game/ with a CMakeLists.txt in it. That file, and not the
/// directory, is what makes one.
bool isGame(std::string_view candidate);
std::vector<std::string> games();
void printGames(std::FILE* stream);

/// A variable out of a configured build directory's CMakeCache.txt, or empty.
std::string cacheValue(const std::filesystem::path& cache, std::string_view key);

/// Which game a build directory is configured for. This is how `build_game` tells every
/// other subcommand what it did, and why none of their signatures name a game.
std::string cachedGame(Config config);

/// The wrapper a configuration's binary has to be started through, and the environment it
/// needs. TSan aborts with "unexpected memory mapping" before main() without `setarch -R`,
/// and that failure looks exactly like a clean run to anyone grepping for race warnings.
struct Launch {
    std::vector<std::string> prefix;
    std::vector<std::pair<std::string, std::string>> env;
};
Launch sanitizerLaunch(Config config);

/// The `[game] [config] -- ...` parse, classified by predicate rather than by position, so
/// the two may be given in either order.
struct Invocation {
    Config config = Config::Debug;
    std::string game;

    /// Whether a game was asked for *by name*, recorded before any fallback fills `game` in.
    /// A suite that renders whichever game was built last is a suite whose baselines depend
    /// on the last thing somebody did.
    bool namedGame = false;

    /// Everything after `--`.
    std::vector<std::string> rest;
};

/// Returns false having already printed why, or having printed the help that was asked for.
/// `exitCode` is what to return in that case.
bool parseInvocation(const std::vector<std::string>& args, Invocation& out, int& exitCode,
                     std::string_view usage);

} // namespace tool
