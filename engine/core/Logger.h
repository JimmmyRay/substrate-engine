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

inline constexpr uint32_t AllLogCategories = 0x1FFu;

/// Ordered by severity; a level is emitted when it is <= the active level.
enum class LogLevel : int {
    Critical = 0,
    Error = 1,
    Warn = 2,
    Status = 3,
    Debug = 4,
    /// Not a level. It is what makes "every level has a name" a `static_assert` beside the
    /// list rather than a promise -- see `core::namesEveryValue`.
    Count = 5,
};

enum class LogOutput : uint8_t {
    Terminal = 1u << 0,
    File = 1u << 1,
    Both = (1u << 0) | (1u << 1),
};

/**
 * @brief The three name lists, one per enum, each canonical spelling first (D12).
 *
 * Here rather than in the config parser because a list belongs beside the enum it names.
 * `LogCategory`'s in particular was written twice -- lowercase in `Config.cpp` to parse
 * `"logging": {"categories": [...]}`, and title-cased in `Logger.cpp` to print the tag on
 * every line -- so a category added to the enum could be printable and unparseable, or the
 * reverse. It is one list now, spelled the way the log line spells it, and matched
 * case-insensitively like every other name.
 *
 * `logCategoryNames` does **not** hold `all`, which is not a category but a wildcard over
 * every one of them; `Config::logCategoryMask` owns that word.
 */
[[nodiscard]] Names<LogLevel> logLevelNames();
[[nodiscard]] Names<LogOutput> logOutputNames();
[[nodiscard]] Names<LogCategory> logCategoryNames();

/**
 * @brief Categorised logger with colourised terminal output and async file writes.
 *
 * Terminal writes happen on the calling thread; file writes are queued to a
 * background writer so disk latency never lands in a frame.
 *
 * Unlike the original this is descended from, level and category filtering are
 * runtime state rather than compile-time constants.
 */
class Logger {
  public:
    /// Open the log file and start the writer thread. Safe to call again; re-inits.
    static void init(const std::string& filePath, LogOutput output = LogOutput::Both);

    /// Drain the queue, join the writer, close the file. Idempotent.
    static void shutdown();

    /**
     * @brief Change where log lines go, without re-opening the file.
     *
     * Exists for one caller: `--dump-settings=json` writes a machine-readable document to
     * stdout, and warnings and status lines go to stdout too -- so a run that emits one has
     * to take the log off the terminal or emit something that does not parse. It is a
     * separate call from `init` because the lines worth suppressing are logged *before*
     * `init` runs, while the config file is being read.
     */
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

    // -------------------------------------------------------------- std::string
    [[noreturn]] static void critical(LogCategory c, const std::string& msg);

    static void error(LogCategory c, const std::string& msg) { emitIf(LogLevel::Error, c, msg); }
    static void warn(LogCategory c, const std::string& msg) { emitIf(LogLevel::Warn, c, msg); }
    static void status(LogCategory c, const std::string& msg) { emitIf(LogLevel::Status, c, msg); }
    static void debug(LogCategory c, const std::string& msg) { emitIf(LogLevel::Debug, c, msg); }

    // ------------------------------------------------------------- printf style
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
