#include "core/Logger.h"

#include "core/Profiler.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace core {

namespace {

#ifdef _WIN32
/// Teach the console to interpret the escapes below instead of printing them: legacy
/// conhost.exe renders every one as a literal `<-[32m`. Failure is ignored on purpose --
/// output redirected to a file has no console mode to set.
void enableVirtualTerminal() {
    for (DWORD handle : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        HANDLE h = GetStdHandle(handle);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
}
#endif

constexpr const char* GREEN = "\033[32m";
constexpr const char* ORANGE = "\033[38;5;208m";
constexpr const char* RED = "\033[31m";
constexpr const char* RESET = "\033[0m";

std::mutex g_terminalMutex;

std::ofstream g_logFile;
LogOutput g_output = LogOutput::Terminal;

std::thread g_writerThread;
std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::queue<std::string> g_queue;
bool g_running = false;

// Canonical spelling first: `warning` must stay after `warn`, or a save starts writing it.
constexpr Named<LogLevel> kLogLevels[] = {
    {"critical", LogLevel::Critical}, {"error", LogLevel::Error},   {"warn", LogLevel::Warn},
    {"warning", LogLevel::Warn},      {"status", LogLevel::Status}, {"debug", LogLevel::Debug},
};
static_assert(namesEveryValue(kLogLevels), "a level reachable from the enum and from no name");

/// `LogOutput` and `LogCategory` are masks with no `Count` for `namesEveryValue` to walk,
/// so the unit suite checks their totality instead, over `LogOutput::Both` and
/// `AllLogCategories`.
constexpr Named<LogOutput> kLogOutputs[] = {
    {"terminal", LogOutput::Terminal},
    {"file", LogOutput::File},
    {"both", LogOutput::Both},
};

/// Title-cased because this is also the tag printed on every line; parsing is
/// case-insensitive, so a config file may still say `"vulkan"`.
constexpr Named<LogCategory> kLogCategories[] = {
    {"Core", LogCategory::Core},     {"Vulkan", LogCategory::Vulkan},   {"GLTF", LogCategory::GLTF},
    {"Scene", LogCategory::Scene},   {"Render", LogCategory::Render},   {"Asset", LogCategory::Asset},
    {"Input", LogCategory::Input},   {"Profile", LogCategory::Profile}, {"Audio", LogCategory::Audio},
};

const char* categoryName(LogCategory c) {
    const char* name = nameOf(logCategoryNames(), c);
    return name != nullptr ? name : "Unknown";
}

struct LevelStyle {
    const char* label;
    const char* colour;
    bool toStderr;
};

LevelStyle levelStyle(LogLevel l) {
    switch (l) {
    case LogLevel::Critical: return {"CRITICAL", RED, true};
    case LogLevel::Error: return {"ERROR", RED, true};
    case LogLevel::Warn: return {"WARNING", ORANGE, false};
    case LogLevel::Status: return {"STATUS", GREEN, false};
    case LogLevel::Debug: return {"DEBUG", nullptr, false};
    case LogLevel::Count: break;
    }
    return {"UNKNOWN", nullptr, false};
}

std::string timestamp() {
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    auto sec = clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tmBuf{};
#ifdef _WIN32
    // Argument order is reversed from localtime_r's below, and getting it wrong is silent.
    localtime_s(&tmBuf, &sec);
#else
    localtime_r(&sec, &tmBuf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms;
    return oss.str();
}

void writerLoop() {
    Profiler::nameThread("log writer");
    for (;;) {
        std::string message;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCv.wait(lock, [] { return !g_queue.empty() || !g_running; });

            if (g_queue.empty()) {
                if (!g_running) break;
                continue;
            }

            message = std::move(g_queue.front());
            g_queue.pop();
        }

        if (g_logFile.is_open()) {
            g_logFile << message;
            g_logFile.flush();
        }
    }
}

} // namespace

Names<LogLevel> logLevelNames() {
    return kLogLevels;
}

Names<LogOutput> logOutputNames() {
    return kLogOutputs;
}

Names<LogCategory> logCategoryNames() {
    return kLogCategories;
}

std::atomic<int> Logger::s_level{static_cast<int>(LogLevel::Debug)};
std::atomic<uint32_t> Logger::s_categories{AllLogCategories};

void Logger::init(const std::string& filePath, LogOutput output) {
    shutdown();

#ifdef _WIN32
    enableVirtualTerminal();
#endif

    g_output = output;

    if (static_cast<uint8_t>(output) & static_cast<uint8_t>(LogOutput::File)) {
        std::filesystem::path path(filePath);
        if (path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
        }

        g_logFile.open(path, std::ios::app);
        if (!g_logFile.is_open()) {
            g_output = LogOutput::Terminal;
            error(LogCategory::Core, "Failed to open log file: %s", filePath.c_str());
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_running = true;
        }
        g_writerThread = std::thread(writerLoop);

        static bool atexitRegistered = false;
        if (!atexitRegistered) {
            std::atexit(Logger::shutdown);
            atexitRegistered = true;
        }
    }
}

void Logger::setOutput(LogOutput output) {
    // Must not touch the file: `init` owns opening it, and a run that turns the terminal
    // off before `init` still wants those lines in the log once there is one.
    g_output = output;
}

void Logger::shutdown() {
    bool wasRunning = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        wasRunning = g_running;
        g_running = false;
    }

    if (wasRunning) {
        g_queueCv.notify_all();
        if (g_writerThread.joinable()) {
            g_writerThread.join();
        }
    }

    if (g_logFile.is_open()) {
        g_logFile.flush();
        g_logFile.close();
    }
}

void Logger::enableCategory(LogCategory c, bool on) {
    uint32_t bit = static_cast<uint32_t>(c);
    uint32_t cur = s_categories.load(std::memory_order_relaxed);
    uint32_t next;
    do {
        next = on ? (cur | bit) : (cur & ~bit);
    } while (!s_categories.compare_exchange_weak(cur, next, std::memory_order_relaxed));
}

void Logger::emit(LogLevel l, LogCategory c, const std::string& msg) {
    const LevelStyle style = levelStyle(l);
    const std::string ts = timestamp();
    const char* cat = categoryName(c);

    if (static_cast<uint8_t>(g_output) & static_cast<uint8_t>(LogOutput::Terminal)) {
        std::ostream& os = style.toStderr ? std::cerr : std::cout;
        std::lock_guard<std::mutex> lock(g_terminalMutex);
        os << '[' << ts << "] [" << cat << "] [" << style.label << "] ";
        if (style.colour) os << style.colour;
        os << msg;
        if (style.colour) os << RESET;
        os << '\n' << std::flush;
    }

    if (static_cast<uint8_t>(g_output) & static_cast<uint8_t>(LogOutput::File)) {
        std::ostringstream oss;
        oss << '[' << ts << "] [" << cat << "] [" << style.label << "] " << msg << '\n';
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            if (!g_running) return; // writer not started; terminal already has it
            g_queue.push(oss.str());
        }
        g_queueCv.notify_one();
    }
}

std::string Logger::vformat(const char* fmt, va_list args) {
    va_list argsCopy;
    va_copy(argsCopy, args);
    const int len = std::vsnprintf(nullptr, 0, fmt, argsCopy);
    va_end(argsCopy);

    if (len < 0) return {};

    std::vector<char> buf(static_cast<size_t>(len) + 1);
    std::vsnprintf(buf.data(), buf.size(), fmt, args);
    return std::string(buf.data(), static_cast<size_t>(len));
}

void Logger::critical(LogCategory c, const std::string& msg) {
    // Emits unfiltered and flushes before exiting, or the reason for the exit never reaches
    // the log file.
    emit(LogLevel::Critical, c, msg);
    shutdown();
    std::exit(1);
}

#define SUBSTRATE_LOG_VARARG(fn, lvl)                                                                                  \
    void Logger::fn(LogCategory c, const char* fmt, ...) {                                                             \
        if (!wants(lvl, c)) return;                                                                                    \
        va_list args;                                                                                                  \
        va_start(args, fmt);                                                                                           \
        std::string msg = vformat(fmt, args);                                                                          \
        va_end(args);                                                                                                  \
        emit(lvl, c, msg);                                                                                             \
    }

SUBSTRATE_LOG_VARARG(error, LogLevel::Error)
SUBSTRATE_LOG_VARARG(warn, LogLevel::Warn)
SUBSTRATE_LOG_VARARG(status, LogLevel::Status)
SUBSTRATE_LOG_VARARG(debug, LogLevel::Debug)

#undef SUBSTRATE_LOG_VARARG

void Logger::critical(LogCategory c, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string msg = vformat(fmt, args);
    va_end(args);
    critical(c, msg);
}

} // namespace core
