#pragma once

#include <optional>
#include <string>

/**
 * @file tools/cli/HarnessLog.h
 * @brief Reading numbers back out of what a run said.
 *
 * The scripted harnesses assert against log lines the engine writes, which the shell did with
 * one capturing `sed` expression per field. These are the same patterns with the failure mode
 * removed: a `sed` capture that stops matching yields an empty string, and an assertion
 * against an empty string silently compares nothing. `std::nullopt` has to be handled.
 */
namespace tool {

/// The text between `prefix` and the end of its line, or empty.
std::string after(const std::string& log, const std::string& prefix);

/// The whole line holding `needle`, or empty.
///
/// Fields belonging to one summary line are read out of that line specifically. Searching a
/// whole log for `Arena: ` finds the first *transition* line instead, which is a different
/// sentence that starts with the same words.
std::string lineWith(const std::string& log, const std::string& needle);

/// The number following `prefix`, or nullopt. Signed, because every ratio these lines report
/// admits a minus.
std::optional<double> number(const std::string& log, const std::string& prefix);

/// The second number of an `N..M` range, or nullopt.
std::optional<double> numberAfterDots(const std::string& log);

/// The step a named transition landed on. The log line is
/// `<label>: <from> -> <to> at step N (...)`.
std::optional<int> transitionStep(const std::string& log, const std::string& label,
                                  const std::string& from, const std::string& to);

std::string format(double value, int places = 3);

/// Counts assertions rather than exiting on the first, so one run reports everything wrong
/// with it instead of the first thing.
struct Report {
    int failures = 0;

    void fail(const std::string& message);
    void within(double value, double low, double high, const std::string& message);
};

} // namespace tool
