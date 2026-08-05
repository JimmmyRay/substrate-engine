#pragma once

#include "core/Format.h"
#include "core/Names.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <string>

namespace core {

/// Subsystem tags. Filtered at runtime via Logger::setCategories().
enum class LogCategory : uint32_t {
    Core = 1u << 0,
    Vulkan = 1u << 1,
    GLTF = 1u << 2,
    Scene = 1u << 3,
    Render = 1u << 4,
    Asset = 1u << 5,
    Input = 1u << 6,
    Profile = 1u << 7,
    Audio = 1u << 8,
};

/// Every bit above. A category added without widening this is invisible to `--log-categories
/// all`, and the unit suite's totality check over it fails.
inline constexpr uint32_t AllLogCategories = 0x1FFu;

/// Ordered by severity; a level is emitted when it is <= the active level.
enum class LogLevel : int {
    Critical = 0,
    Error = 1,
    Warn = 2,
    Status = 3,
    Debug = 4,
    /// Not a level. `core::namesEveryValue` walks up to it, so removing it turns "every
    /// level has a name" from a `static_assert` back into a promise.
    Count = 5,
};

enum class LogOutput : uint8_t {
    Terminal = 1u << 0,
    File = 1u << 1,
    Both = (1u << 0) | (1u << 1),
};

/// @brief The three name lists, one per enum, canonical spelling first.
///
/// `logCategoryNames` is also what prints the tag on every log line, so a second list for
/// either purpose lets a category become printable and unparseable. It does *not* hold
/// `all`, a wildcard rather than a category; `Config::logCategoryMask` owns that word.
[[nodiscard]] Names<LogLevel> logLevelNames();
[[nodiscard]] Names<LogOutput> logOutputNames();
[[nodiscard]] Names<LogCategory> logCategoryNames();

/// @brief Categorised logger with colourised terminal output and async file writes.
///
/// Callable from any thread. Terminal writes happen on the calling thread under
/// `g_terminalMutex`; file writes are queued to a background writer, so disk latency never
/// lands in a frame.
class Logger {
  public:
    /// Open the log file and start the writer thread. Safe to call again; re-inits.
    static void init(const std::string& filePath, LogOutput output = LogOutput::Both);

    /// Drain the queue, join the writer, close the file. Idempotent.
    static void shutdown();

    /// Change where log lines go, without re-opening the file. Separate from `init` because
    /// the lines a `--dump-settings=json` run has to keep off stdout are logged *before*
    /// `init` runs, while the config file is being read.
    static void setOutput(LogOutput output);

    static void setLevel(LogLevel l) { s_level.store(static_cast<int>(l), std::memory_order_relaxed); }
    static LogLevel level() { return static_cast<LogLevel>(s_level.load(std::memory_order_relaxed)); }

    static void setCategories(uint32_t mask) { s_categories.store(mask, std::memory_order_relaxed); }
    static uint32_t categories() { return s_categories.load(std::memory_order_relaxed); }
    static void enableCategory(LogCategory c, bool on);

    /// True when a message at this level and category would be emitted.
    static bool wants(LogLevel l, LogCategory c) {
        return static_cast<int>(l) <= s_level.load(std::memory_order_relaxed) &&
               (s_categories.load(std::memory_order_relaxed) & static_cast<uint32_t>(c)) != 0;
    }

    [[noreturn]] static void critical(LogCategory c, const std::string& msg);

    static void error(LogCategory c, const std::string& msg) { emitIf(LogLevel::Error, c, msg); }
    static void warn(LogCategory c, const std::string& msg) { emitIf(LogLevel::Warn, c, msg); }
    static void status(LogCategory c, const std::string& msg) { emitIf(LogLevel::Status, c, msg); }
    static void debug(LogCategory c, const std::string& msg) { emitIf(LogLevel::Debug, c, msg); }

    [[noreturn]] __attribute__((format(SUBSTRATE_PRINTF_FORMAT, 2, 3))) static void critical(LogCategory c, const char* fmt, ...);

    __attribute__((format(SUBSTRATE_PRINTF_FORMAT, 2, 3))) static void error(LogCategory c, const char* fmt, ...);
    __attribute__((format(SUBSTRATE_PRINTF_FORMAT, 2, 3))) static void warn(LogCategory c, const char* fmt, ...);
    __attribute__((format(SUBSTRATE_PRINTF_FORMAT, 2, 3))) static void status(LogCategory c, const char* fmt, ...);
    __attribute__((format(SUBSTRATE_PRINTF_FORMAT, 2, 3))) static void debug(LogCategory c, const char* fmt, ...);

    static std::string vformat(const char* fmt, va_list args);

  private:
    static void emitIf(LogLevel l, LogCategory c, const std::string& msg) {
        if (wants(l, c)) emit(l, c, msg);
    }
    static void emit(LogLevel l, LogCategory c, const std::string& msg);

    static std::atomic<int> s_level;
    static std::atomic<uint32_t> s_categories;
};

} // namespace core
