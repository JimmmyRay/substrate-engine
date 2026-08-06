#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @file tools/cli/Process.h
 * @brief Spawn a child, bound it in time, and hand back what it said.
 *
 * One function replacing `timeout -s TERM N cmd >"$log" 2>&1` and the grep over the log that
 * followed every one of them. Output arrives as a string rather than a file, which is why no
 * caller needs the ANSI strip the shell harnesses ran before they could match a line.
 */
namespace tool {

struct RunResult {
    int exitCode = 0;
    bool timedOut = false;
    std::string out;
    std::string err;

    bool ok() const { return exitCode == 0 && !timedOut; }

    /// True when `needle` appears anywhere the child wrote. Harnesses key on log lines the
    /// engine prints to either stream, and which one is not part of that contract.
    bool saidAny(std::string_view needle) const;
};

struct RunOptions {
    /// Seconds before the child is asked to stop. Zero waits forever.
    int timeoutSeconds = 0;

    /// Empty inherits the caller's.
    std::filesystem::path cwd;

    /// Added to the child's environment, not replacing it.
    std::vector<std::pair<std::string, std::string>> env;

    /// Let the child write straight to this process's stdout and stderr. `out` and `err`
    /// come back empty; use it when a human is watching, since capturing would hold every
    /// line until exit.
    bool inherit = false;
};

/// Run `argv` to completion, or until the timeout.
///
/// A timeout is a request first: the child gets SIGTERM (CTRL_BREAK on Windows) and two
/// seconds to act on it, because that signal is what makes the profiler flush its trace --
/// killing outright turns a slow run into a run with no measurement in it.
RunResult run(const std::vector<std::string>& argv, const RunOptions& options = {});

/// Replace this process with `argv` where the platform can, spawn and forward the exit code
/// where it cannot. Nothing after this returns on POSIX.
[[noreturn]] void execOrExit(const std::vector<std::string>& argv, const RunOptions& options = {});

/// The same text with SGR escapes removed.
///
/// The engine colours its log, and every harness matches on those lines. The shell versions
/// ran `sed -i` over the log file to strip them, which is a step that only exists because
/// the output had to go through a file first.
std::string stripAnsi(std::string text);

/// Absolute path of the first `name` on PATH, or empty. PATHEXT is applied on Windows, so
/// asking for "cmake" finds cmake.exe.
std::filesystem::path which(const std::string& name);

} // namespace tool
