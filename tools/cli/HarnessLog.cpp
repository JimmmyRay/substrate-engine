#include "HarnessLog.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace tool {

std::string after(const std::string& log, const std::string& prefix) {
    const size_t at = log.find(prefix);
    if (at == std::string::npos) return {};
    const size_t from = at + prefix.size();
    const size_t end = log.find('\n', from);
    std::string value = log.substr(from, end == std::string::npos ? end : end - from);
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    return value;
}

std::string lineWith(const std::string& log, const std::string& needle) {
    const size_t at = log.find(needle);
    if (at == std::string::npos) return {};
    const size_t start = log.rfind('\n', at);
    const size_t end = log.find('\n', at);
    const size_t from = start == std::string::npos ? 0 : start + 1;
    return log.substr(from, end == std::string::npos ? end : end - from);
}

std::optional<double> number(const std::string& log, const std::string& prefix) {
    const size_t at = log.find(prefix);
    if (at == std::string::npos) return std::nullopt;

    size_t i = at + prefix.size();
    const size_t start = i;
    if (i < log.size() && (log[i] == '-' || log[i] == '+')) ++i;
    while (i < log.size() && (std::isdigit(static_cast<unsigned char>(log[i])) || log[i] == '.')) {
        ++i;
    }
    if (i == start) return std::nullopt;
    return std::atof(log.substr(start, i - start).c_str());
}

std::optional<double> numberAfterDots(const std::string& log) {
    const size_t at = log.find("..");
    if (at == std::string::npos) return std::nullopt;
    return number(log.substr(at), "..");
}

std::optional<int> transitionStep(const std::string& log, const std::string& label,
                                  const std::string& from, const std::string& to) {
    const std::optional<double> step =
        number(log, label + ": " + from + " -> " + to + " at step ");
    if (!step) return std::nullopt;
    return static_cast<int>(*step);
}

std::string format(double value, int places) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", places, value);
    return buffer;
}

void Report::fail(const std::string& message) {
    std::fprintf(stderr, "  FAIL: %s\n", message.c_str());
    ++failures;
}

void Report::within(double value, double low, double high, const std::string& message) {
    if (value < low || value > high) fail(message);
}

} // namespace tool
