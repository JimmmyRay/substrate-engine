#include "core/Profiler.h"

#include "core/AveragingBuffer.h"
#include "core/Logger.h"

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace core {

namespace {

// Scope paths fold into a 64-bit FNV-1a hash rather than concatenating into a string, which
// is what keeps the recording path allocation-free. The readable path string is
// materialised once per unique path, on the cold side.
constexpr uint64_t FNV_OFFSET = 1469598103934665603ull;
constexpr uint64_t FNV_PRIME = 1099511628211ull;

uint64_t hashString(const char* s, uint64_t h) {
    while (*s) {
        h ^= static_cast<unsigned char>(*s++);
        h *= FNV_PRIME;
    }
    return h;
}

/// Fold `name` onto a parent path hash. The separator keeps "a/b" distinct from "ab".
uint64_t childPathHash(uint64_t parent, const char* name) {
    uint64_t h = parent ? parent : FNV_OFFSET;
    h ^= static_cast<unsigned char>('/');
    h *= FNV_PRIME;
    return hashString(name, h);
}

std::mutex g_pathMutex;
std::unordered_map<uint64_t, std::string> g_pathStrings;
thread_local std::unordered_set<uint64_t> t_knownPaths;

void registerPath(uint64_t hash, uint64_t parentHash, const char* name) {
    // The thread-local set is what keeps the steady state off `g_pathMutex` entirely.
    if (t_knownPaths.find(hash) != t_knownPaths.end()) return;
    t_knownPaths.insert(hash);

    std::lock_guard<std::mutex> lock(g_pathMutex);
    if (g_pathStrings.find(hash) != g_pathStrings.end()) return;

    std::string full;
    if (parentHash != 0) {
        auto it = g_pathStrings.find(parentHash);
        if (it != g_pathStrings.end()) {
            full = it->second;
            full += '/';
        }
    }
    full += name;
    g_pathStrings.emplace(hash, std::move(full));
}

std::string pathString(uint64_t hash) {
    std::lock_guard<std::mutex> lock(g_pathMutex);
    auto it = g_pathStrings.find(hash);
    return it != g_pathStrings.end() ? it->second : std::string("?");
}

// `scopef()` names must outlive the frame they were recorded in, so they are interned. The
// pool must stay a `std::deque`: the `c_str()` handed out has to stay valid for the process
// lifetime, and a `vector` invalidates it on the next push.
//
// Interning is permanent, so the cap is what keeps `scopef("Upload %llu", frameNumber)` from
// leaking forever; past it, names collapse onto one bucket and a warning names the offender.
constexpr size_t kMaxInternedNames = 4096;

std::mutex g_namePoolMutex;
std::deque<std::string> g_namePool;
std::unordered_map<std::string, const char*> g_nameLookup;
bool g_internPoolFull = false;

const char* internName(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_namePoolMutex);
    auto it = g_nameLookup.find(s);
    if (it != g_nameLookup.end()) return it->second;

    if (g_namePool.size() >= kMaxInternedNames) {
        if (!g_internPoolFull) {
            g_internPoolFull = true;
            Logger::warn(LogCategory::Profile,
                         "Profiler: scopef() name pool full at %zu entries; '%s' and later new names collapse to "
                         "'<scopef pool full>'. A name that varies per frame is the usual cause.",
                         kMaxInternedNames, s.c_str());
        }
        return "<scopef pool full>";
    }

    g_namePool.push_back(s);
    const char* stable = g_namePool.back().c_str();
    g_nameLookup.emplace(s, stable);
    return stable;
}

/// Trace row for GPU zones. Must stay above any thread slot id, or the two share a track.
constexpr uint32_t kGpuTrackId = 1000;

struct ScopeTiming {
    const char* name = nullptr; ///< Static lifetime: literal or interned
    uint64_t pathHash = 0;
    uint32_t depth = 0;
    uint32_t threadId = 0;
    double startTimeUs = 0.0;
    double cpuTimeMs = 0.0;
    bool gpu = false;
};

/// One quantity, by name. `name` is held by pointer and must have static lifetime.
struct CounterSample {
    const char* name = nullptr;
    double value = 0.0;
};

/**
 * @brief Per-thread recording buffer, recycled when a thread exits.
 *
 * Slots are reused rather than accumulated: an engine spawning short-lived jobs would
 * otherwise grow the registry without bound and render one near-empty Chrome Tracing row
 * per thread ever created.
 *
 * `guard` is a spinlock rather than a mutex because exactly one thread pushes to a slot and
 * only the collector drains it. It cannot be dropped -- a worker recording mid-frame races
 * the collector.
 */
struct ThreadSlot {
    std::vector<ScopeTiming> scopes;

    /// Names this slot has been given since the last collect, in order. Drained beside
    /// `scopes`, under the same spinlock and by the same caller.
    ///
    /// `nullptr` is a real entry meaning *unnamed*, pushed by `acquireSlot` on every
    /// acquisition; without it a thread that never names itself inherits the label of
    /// whichever thread held the slot before it.
    std::vector<const char*> pendingNames;

    /// The name this acquisition has already emitted, so naming is idempotent.
    const char* name = nullptr;

    /// This frame's counters, one entry per distinct name. A second write of a name must
    /// overwrite rather than append: that is what bounds this vector, and what keeps the
    /// recording path from allocating after the first frame that used a given name.
    std::vector<CounterSample> counters;

    std::atomic_flag guard = ATOMIC_FLAG_INIT;
    uint32_t threadId = 0;
    bool inUse = false;

    void lock() {
        while (guard.test_and_set(std::memory_order_acquire)) {
        }
    }
    void unlock() { guard.clear(std::memory_order_release); }
};

std::mutex g_registryMutex;
std::vector<std::unique_ptr<ThreadSlot>> g_registry;
thread_local ThreadSlot* t_slot = nullptr;
thread_local std::vector<uint64_t> t_stack;

/// Releases this thread's slot back to the pool at thread exit.
struct ThreadSlotGuard {
    ~ThreadSlotGuard() {
        if (t_slot == nullptr) return;
        std::lock_guard<std::mutex> lock(g_registryMutex);
        // Buffered scopes stay put; the next collectFrame() drains them.
        t_slot->inUse = false;
        t_slot = nullptr;
    }
};
thread_local ThreadSlotGuard t_slotGuard;

ThreadSlot& acquireSlot() {
    if (t_slot != nullptr) return *t_slot;

    (void)&t_slotGuard; // odr-use so the thread-exit destructor is registered

    // Every acquisition pushes a `nullptr` name. Without it, a recycled slot leaves the new
    // thread's work under the previous thread's label.
    const auto claim = [](ThreadSlot& slot) {
        slot.inUse = true;
        slot.name = nullptr;
        slot.lock();
        slot.pendingNames.push_back(nullptr);
        slot.unlock();
        t_slot = &slot;
    };

    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (auto& slot : g_registry) {
        if (!slot->inUse) {
            claim(*slot);
            return *t_slot;
        }
    }

    auto owned = std::make_unique<ThreadSlot>();
    owned->scopes.reserve(256);
    owned->threadId = static_cast<uint32_t>(g_registry.size()) + 1;
    claim(*owned);
    g_registry.push_back(std::move(owned));
    return *t_slot;
}

uint32_t currentThreadId() { return acquireSlot().threadId; }

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// One `thread_name` metadata event: which track, and what to call it. `name == nullptr`
/// renders as `thread <id>`, which is what an unnamed acquisition gets.
struct ThreadName {
    uint32_t threadId = 0;
    const char* name = nullptr;
};

struct FrameData {
    uint64_t frameNumber = 0;
    /// steady_clock nanoseconds at which this frame opened. Held per frame rather than read
    /// from `frameStartNs`, which has moved on several frames by the time a calibrated GPU
    /// zone arrives to be resolved against it.
    int64_t startNs = 0;
    double durationUs = 0.0;
    std::vector<ScopeTiming> scopes;
    std::vector<ThreadName> threadNames;
    std::vector<CounterSample> counters;
};

std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::atomic<bool> g_fileClosed{false};
std::atomic<bool> g_signalFlushRequested{false};
std::atomic<bool> g_signalFlushDone{false};
bool g_handlersRegistered = false;
std::string g_outputFilePath;

} // namespace

struct Profiler::Impl {
    ProfilerConfig config;

    mutable std::mutex mutex;

    uint64_t frameCounter = 0;

    // Read by every closing scope on every thread while the main thread flips it at frame
    // boundaries. `frameStartNs` must be published before `frameStarted` and read after it,
    // or a thread observing an open frame can read a start time from the previous one.
    std::atomic<bool> frameStarted{false};
    std::atomic<int64_t> frameStartNs{0};

    std::deque<FrameData> pendingFrames;

    static constexpr size_t MaxAveragingEntries = 512;
    std::unordered_map<uint64_t, AveragingBuffer<double>> cpuAverages;

    std::thread writerThread;
    std::mutex writerMutex;
    std::condition_variable writerCv;
    std::deque<FrameData> writeQueue;
    std::atomic<bool> writerRunning{false};
    bool writerDirty = false;
    std::string writerOutputPath;

    ~Impl() { stopWriterThread(); }

    double average(uint64_t pathHash, double value) {
        auto it = cpuAverages.find(pathHash);
        if (it == cpuAverages.end()) {
            if (cpuAverages.size() >= MaxAveragingEntries) return value;
            it = cpuAverages.emplace(pathHash, AveragingBuffer<double>(config.averagingWindow)).first;
        }
        return it->second.nextValue(value);
    }

    void record(const ScopeTiming& timing) {
        ThreadSlot& slot = acquireSlot();
        slot.lock();
        slot.scopes.push_back(timing);
        slot.unlock();
    }

    /// Move every thread's recorded scopes into one frame. Caller must not hold `mutex`.
    void collectFrame(FrameData& frame) {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        for (auto& slot : g_registry) {
            slot->lock();
            if (!slot->scopes.empty()) {
                frame.scopes.insert(frame.scopes.end(), slot->scopes.begin(), slot->scopes.end());
                slot->scopes.clear();
            }
            for (const char* name : slot->pendingNames) frame.threadNames.push_back({slot->threadId, name});
            slot->pendingNames.clear();
            // Cleared rather than carried, so a caller that stops writing a counter sees
            // the track stop instead of the last value repeating forever.
            frame.counters.insert(frame.counters.end(), slot->counters.begin(), slot->counters.end());
            slot->counters.clear();
            slot->unlock();
        }
    }

    void endFrame() {
        if (!frameStarted.load(std::memory_order_acquire)) return;

        const int64_t startNs = frameStartNs.load(std::memory_order_relaxed);
        const double frameDurationUs = static_cast<double>(nowNs() - startNs) / 1000.0;

        FrameData frame;
        frame.frameNumber = frameCounter;
        frame.startNs = startNs;
        frame.durationUs = frameDurationUs;
        collectFrame(frame);

        {
            std::lock_guard<std::mutex> lock(mutex);
            pendingFrames.push_back(std::move(frame));
            // Frame 0 is pinned and frame 1 evicted in its place: window creation, device
            // init and asset load all happen in frame 0 and never again, so ageing it out
            // leaves the trace with no record of the most expensive work the process does.
            if (config.maxFrames > 0) {
                while (pendingFrames.size() > config.maxFrames) {
                    if (pendingFrames.front().frameNumber == 0 && pendingFrames.size() > 1) {
                        pendingFrames.erase(pendingFrames.begin() + 1);
                    } else {
                        pendingFrames.pop_front();
                    }
                }
            }
        }

        frameCounter++;
        frameStarted.store(false, std::memory_order_release);

        if (!config.outputFile.empty() && config.autoFlushFrames > 0 &&
            frameCounter % config.autoFlushFrames == 0) {
            queueFramesForWrite();
            if (config.clearAfterFlush) {
                std::lock_guard<std::mutex> lock(mutex);
                cpuAverages.clear();
            }
        }
    }

    void queueFramesForWrite() {
        {
            std::lock_guard<std::mutex> writerLock(writerMutex);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (pendingFrames.empty()) return;
                writeQueue.insert(writeQueue.end(), std::make_move_iterator(pendingFrames.begin()),
                                  std::make_move_iterator(pendingFrames.end()));
                pendingFrames.clear();
            }

            // Pinned here too: `pendingFrames` drains into `writeQueue` on the first flush,
            // so without this frame 0 survives its own window and is trimmed out of the one
            // that actually gets written.
            if (config.maxFrames > 0) {
                while (writeQueue.size() > config.maxFrames) {
                    if (writeQueue.front().frameNumber == 0 && writeQueue.size() > 1) {
                        writeQueue.erase(writeQueue.begin() + 1);
                    } else {
                        writeQueue.pop_front();
                    }
                }
            }
            writerDirty = true;
        }
        writerCv.notify_one();
    }

    void startWriterThread(const std::string& path) {
        writerOutputPath = path;
        writerRunning = true;
        writerThread = std::thread([this] { writerLoop(); });
    }

    void stopWriterThread() {
        if (!writerRunning) return;
        {
            std::lock_guard<std::mutex> lock(writerMutex);
            if (!writeQueue.empty()) writerDirty = true;
            writerRunning = false;
        }
        writerCv.notify_all();
        if (writerThread.joinable()) writerThread.join();
    }

    void writerLoop() {
        for (;;) {
            std::deque<FrameData> framesToWrite;
            bool shuttingDown = false;

            {
                std::unique_lock<std::mutex> lock(writerMutex);
                // Timed wait so a signal-requested flush is noticed without the handler
                // ever having to touch a mutex.
                writerCv.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return writerDirty || !writerRunning || g_signalFlushRequested.load(std::memory_order_relaxed);
                });

                if (g_signalFlushRequested.load(std::memory_order_relaxed)) {
                    // The interrupted thread may hold `mutex`; never block on it here.
                    std::unique_lock<std::mutex> mainLock(mutex, std::try_to_lock);
                    if (mainLock.owns_lock() && !pendingFrames.empty()) {
                        writeQueue.insert(writeQueue.end(), std::make_move_iterator(pendingFrames.begin()),
                                          std::make_move_iterator(pendingFrames.end()));
                        pendingFrames.clear();
                    }
                    writerDirty = true;
                    writerRunning = false;
                }

                if (writerDirty && !writeQueue.empty()) framesToWrite = writeQueue;
                writerDirty = false;
                shuttingDown = !writerRunning;
            }

            if (!framesToWrite.empty()) writeFramesToFile(framesToWrite);

            if (shuttingDown) {
                g_signalFlushDone.store(true, std::memory_order_release);
                break;
            }
        }
    }

    void writeFramesToFile(const std::deque<FrameData>& frames) {
        // Rewritten whole each time, so the file is always a complete, valid JSON array
        // holding exactly the current window. Appending would leave it unparseable.
        std::ofstream file(writerOutputPath, std::ios::trunc);
        if (!file.is_open()) return;
        writeTrace(file, frames);
    }

    static void writeTrace(std::ostream& out, const std::deque<FrameData>& frames) {
        out << "[\n";

        double cumulativeUs = 0.0;
        bool needsComma = false;

        // The GPU track never acquires a slot, so nothing else names it and Perfetto would
        // render an unlabelled `1000` beside the labelled CPU tracks.
        out << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" << kGpuTrackId
            << ",\"args\":{\"name\":\"GPU\"}}";
        needsComma = true;

        for (const auto& frame : frames) {
            const double frameBaseUs = cumulativeUs;
            cumulativeUs += frame.durationUs;

            // Metadata before the events that land on the track. Perfetto ignores an `M`
            // event's `ts` and lets a second event for the same `tid` replace the first,
            // which is why this emits per acquisition rather than once per slot at the end.
            for (const ThreadName& t : frame.threadNames) {
                if (needsComma) out << ",\n";
                needsComma = true;
                out << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" << t.threadId
                    << ",\"args\":{\"name\":\"";
                if (t.name != nullptr) {
                    out << escapeJson(t.name);
                } else {
                    out << "thread " << t.threadId;
                }
                out << "\"}}";
            }

            // Stamped with `frameBaseUs`, the same synthetic cumulative timeline the scopes
            // use. This file emits no wall-clock time, so a counter stamped from
            // `steady_clock` at the call site draws a graph that does not sit above the
            // zones it explains.
            for (const CounterSample& c : frame.counters) {
                if (needsComma) out << ",\n";
                needsComma = true;
                const std::string name = escapeJson(c.name != nullptr ? c.name : "?");
                out << std::fixed << std::setprecision(3);
                out << "{\"name\":\"" << name << "\",\"cat\":\"counter\",\"ph\":\"C\""
                    << ",\"ts\":" << frameBaseUs << ",\"pid\":1"
                    << ",\"args\":{\"" << name << "\":" << c.value << ",\"frame\":" << frame.frameNumber << "}}";
            }

            for (const auto& scope : frame.scopes) {
                if (needsComma) out << ",\n";
                needsComma = true;

                out << std::fixed << std::setprecision(3);
                out << "{\"name\":\"" << escapeJson(scope.name ? scope.name : "?") << "\""
                    << ",\"cat\":\"" << (scope.gpu ? "gpu" : "cpu") << "\""
                    << ",\"ph\":\"X\""
                    << ",\"ts\":" << (frameBaseUs + scope.startTimeUs) << ",\"dur\":" << (scope.cpuTimeMs * 1000.0)
                    << ",\"pid\":1"
                    << ",\"tid\":" << scope.threadId << ",\"args\":{\"frame\":" << frame.frameNumber
                    << ",\"depth\":" << scope.depth << ",\"path\":\"" << escapeJson(pathString(scope.pathHash))
                    << "\"}"
                    << "}";
            }
        }

        out << "\n]\n";
    }
};

namespace {

/// Ask the writer thread to flush, and wait up to half a second for it to say it did.
///
/// Async-signal-safe only: atomics and a sleep, no mutexes, no allocation, no stdio. The
/// writer thread does the work; this only waits for it. `Sleep` and `nanosleep` are both
/// bare kernel calls that take no CRT lock.
void waitForSignalFlush() {
    if (!g_fileClosed.load(std::memory_order_relaxed) && !g_signalFlushRequested.exchange(true)) {
        for (int i = 0; i < 500 && !g_signalFlushDone.load(std::memory_order_acquire); ++i) {
#ifdef _WIN32
            Sleep(1);
#else
            timespec ts{0, 1000000}; // 1 ms
            nanosleep(&ts, nullptr);
#endif
        }
    }
}

extern "C" void profilerSignalHandler(int sig) {
    waitForSignalFlush();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

#ifdef _WIN32
/// Windows never raises SIGTERM, so a harness that stops a run the way every one of them
/// does -- ask, then wait -- would get a truncated trace and no way to tell it apart from a
/// fast frame. This is the same wait, reached through the mechanism Windows does have.
///
/// Returning FALSE hands the event on to the default handler, which ends the process. The
/// flush has already happened by then, and the OS allows several seconds before it stops
/// waiting for this function to return.
extern "C" BOOL WINAPI profilerConsoleHandler(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        waitForSignalFlush();
        break;
    default:
        break;
    }
    return FALSE;
}
#endif

} // namespace

Profiler& Profiler::instance() {
    static Profiler inst;
    return inst;
}

void Profiler::init(const ProfilerConfig& config) {
    auto& inst = instance();
    inst.impl = std::make_unique<Impl>();
    inst.impl->config = config;

    // `enabled` is tested here and not only on the recording path: without it,
    // `--no-profiler --trace <path>` truncates the file, registers the signal handlers and
    // starts a writer thread, leaving a zero-byte trace and a thread behind.
    if (config.enabled && !config.outputFile.empty()) {
        // The directory is created here because a packaged build has no run.sh to mkdir
        // `debug_frames/`. Without it the truncate below fails silently -- an ofstream that
        // cannot open is not an error until someone asks -- and the trace never appears.
        std::error_code ec;
        if (const std::filesystem::path out(config.outputFile); out.has_parent_path()) {
            std::filesystem::create_directories(out.parent_path(), ec);
        }

        std::ofstream truncate(config.outputFile, std::ios::trunc);
        truncate.close();

        g_outputFilePath = config.outputFile;
        g_fileClosed = false;
        g_signalFlushRequested = false;
        g_signalFlushDone = false;

        if (!g_handlersRegistered) {
            std::atexit(Profiler::closeOutputFile);
            std::signal(SIGTERM, profilerSignalHandler);
            std::signal(SIGINT, profilerSignalHandler);
#ifdef _WIN32
            SetConsoleCtrlHandler(profilerConsoleHandler, TRUE);
#endif
            g_handlersRegistered = true;
        }

        if (config.autoFlushFrames > 0) {
            inst.impl->startWriterThread(config.outputFile);
        }
    }

    // Re-announce every live acquisition: the registry outlives `shutdown()`, so a second
    // `init` in one process inherits already-named slots that `nameThread` declines to
    // re-emit, and every track in the new trace comes out unlabelled.
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        for (auto& slot : g_registry) {
            if (!slot->inUse) continue;
            slot->lock();
            slot->pendingNames.push_back(slot->name);
            slot->unlock();
        }
    }

    // `main` names the thread this profiler's frames belong to, not the process's main
    // thread -- a tool that inits from a worker gets that worker labelled `main`.
    nameThread("main");

    Logger::status(LogCategory::Profile, "Profiler initialised (window=%u, maxFrames=%u, out=%s)",
                   config.averagingWindow, config.maxFrames,
                   config.outputFile.empty() ? "<none>" : config.outputFile.c_str());
}

void Profiler::closeOutputFile() {
    if (g_fileClosed.exchange(true) || g_outputFilePath.empty()) return;

    auto& inst = instance();
    if (!inst.impl) return;

    if (inst.impl->frameStarted.load(std::memory_order_relaxed)) inst.impl->endFrame();
    inst.impl->queueFramesForWrite();
    inst.impl->stopWriterThread();
}

void Profiler::shutdown() {
    auto& inst = instance();
    if (!inst.impl) return;

    if (inst.impl->frameStarted.load(std::memory_order_relaxed)) inst.impl->endFrame();
    closeOutputFile();
    inst.impl.reset();

    // Emptied, never freed, and `inUse` left as it is. `t_slot` is a thread_local raw
    // pointer into this registry that only the *calling* thread's copy is reachable from
    // here, so freeing leaves every other live thread writing into deleted storage on its
    // next `Profiler::scope()` or at thread exit. Clearing `inUse` is its own bug: a slot
    // handed to a second thread while the first still holds the pointer is two writers on
    // one buffer.
    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (auto& slot : g_registry) {
        slot->lock();
        slot->scopes.clear();
        slot->unlock();
    }
}

ProfileScope Profiler::beginFrame() {
    auto& inst = instance();
    if (!inst.impl || !inst.impl->config.enabled) return ProfileScope{};

    if (inst.impl->frameStarted.load(std::memory_order_relaxed)) inst.impl->endFrame();

    inst.impl->frameStartNs.store(nowNs(), std::memory_order_relaxed);
    inst.impl->frameStarted.store(true, std::memory_order_release);

    return scope("Frame");
}

ProfileScope Profiler::scope(const char* literalName) {
    auto& inst = instance();
    if (!inst.impl || !inst.impl->config.enabled) return ProfileScope{};

    if (t_stack.capacity() == 0) t_stack.reserve(64);

    const uint64_t parent = t_stack.empty() ? 0 : t_stack.back();
    const uint64_t hash = childPathHash(parent, literalName);
    registerPath(hash, parent, literalName);

    const uint32_t depth = static_cast<uint32_t>(t_stack.size());
    t_stack.push_back(hash);

    return ProfileScope(literalName, hash, depth, currentThreadId());
}

ProfileScope Profiler::scopef(const char* fmt, ...) {
    auto& inst = instance();
    if (!inst.impl || !inst.impl->config.enabled) return ProfileScope{};

    // `Logger::vformat` rather than a fixed buffer: a truncated name interns as a
    // *different* name, so a length cap here silently splits one zone into two.
    va_list args;
    va_start(args, fmt);
    const std::string name = Logger::vformat(fmt, args);
    va_end(args);

    return scope(internName(name));
}

void Profiler::dump() {
    auto& inst = instance();
    if (!inst.impl) return;

    std::lock_guard<std::mutex> lock(inst.impl->mutex);
    if (inst.impl->pendingFrames.empty()) return;

    const auto& last = inst.impl->pendingFrames.back();

    std::cout << "\n=== Profiler Frame #" << last.frameNumber << " ===\n";
    std::cout << std::left << std::setw(50) << "Scope" << std::right << std::setw(12) << "CPU (avg)" << "\n";
    std::cout << std::string(62, '-') << "\n";

    for (const auto& scope : last.scopes) {
        const std::string indent(scope.depth * 2, ' ');
        const double avg = inst.impl->average(scope.pathHash, scope.cpuTimeMs);

        std::cout << std::left << std::setw(50) << (indent + (scope.name ? scope.name : "?")) << std::right
                  << std::fixed << std::setprecision(3) << std::setw(10) << avg << "ms\n";
    }
    std::cout << std::endl;
}

std::string Profiler::toJson() {
    auto& inst = instance();
    if (!inst.impl) return "{}";

    std::lock_guard<std::mutex> lock(inst.impl->mutex);
    if (inst.impl->pendingFrames.empty()) return "{}";

    const auto& last = inst.impl->pendingFrames.back();

    std::ostringstream ss;
    ss << "{\n  \"frame\": " << last.frameNumber << ",\n  \"scopes\": [\n";

    for (size_t i = 0; i < last.scopes.size(); ++i) {
        const auto& s = last.scopes[i];
        ss << "    {\n";
        ss << "      \"name\": \"" << escapeJson(s.name ? s.name : "?") << "\",\n";
        ss << "      \"path\": \"" << escapeJson(pathString(s.pathHash)) << "\",\n";
        ss << "      \"depth\": " << s.depth << ",\n";
        ss << "      \"threadId\": " << s.threadId << ",\n";
        ss << std::fixed << std::setprecision(3);
        ss << "      \"cpuMs\": " << s.cpuTimeMs << "\n";
        ss << "    }" << (i + 1 < last.scopes.size() ? "," : "") << "\n";
    }

    ss << "  ]\n}\n";
    return ss.str();
}

bool Profiler::writeToFile(const std::string& path) {
    auto& inst = instance();
    if (!inst.impl) return true;

    std::deque<FrameData> frames;
    {
        std::lock_guard<std::mutex> lock(inst.impl->mutex);
        if (inst.impl->pendingFrames.empty()) return true;
        frames = inst.impl->pendingFrames;
    }

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        Logger::warn(LogCategory::Profile, "Profiler: failed to open output file: %s", path.c_str());
        return false;
    }

    Impl::writeTrace(file, frames);
    return true;
}

void Profiler::counter(const char* literalName, double value) {
    if (literalName == nullptr) return;

    auto& inst = instance();
    if (!inst.impl || !inst.impl->config.enabled) return;
    // Dropped rather than buffered when no frame is open: a counter means "this was true
    // during frame N", and there is no N here.
    if (!inst.impl->frameStarted.load(std::memory_order_acquire)) return;

    ThreadSlot& slot = acquireSlot();
    slot.lock();
    // Linear over a handful of entries; a map here would allocate on the recording path.
    // Pointer comparison, because these names are literals.
    for (CounterSample& c : slot.counters) {
        if (c.name == literalName) {
            c.value = value;
            slot.unlock();
            return;
        }
    }
    slot.counters.push_back({literalName, value});
    slot.unlock();
}

void Profiler::nameThread(const char* literalName) {
    if (literalName == nullptr) return;

    ThreadSlot& slot = acquireSlot();
    // Pointer comparison, not `strcmp`: this is called from a device callback hundreds of
    // times a second. Two equal strings at different addresses cost one extra metadata
    // event, which renders identically.
    if (slot.name == literalName) return;

    slot.lock();
    slot.pendingNames.push_back(literalName);
    slot.unlock();
    slot.name = literalName;
}

void Profiler::setEnabled(bool enabled) {
    auto& inst = instance();
    if (inst.impl) inst.impl->config.enabled = enabled;
}

bool Profiler::enabled() {
    auto& inst = instance();
    return inst.impl && inst.impl->config.enabled;
}

uint64_t Profiler::frameNumber() {
    auto& inst = instance();
    return inst.impl ? inst.impl->frameCounter : 0;
}

namespace {

/// Append a GPU zone to a buffered frame. Must be called with the profiler's mutex held.
template <typename OffsetFn>
void appendGpuZone(std::deque<FrameData>& frames, uint64_t frame, const char* literalName, double durationMs,
                   OffsetFn&& offsetUs) {
    // Newest first: the target frame is almost always within the last few.
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        if (it->frameNumber != frame) continue;

        ScopeTiming timing;
        timing.name = literalName;
        timing.pathHash = childPathHash(0, literalName);
        timing.depth = 0;
        timing.threadId = kGpuTrackId;
        timing.startTimeUs = offsetUs(*it);
        timing.cpuTimeMs = durationMs;
        timing.gpu = true;
        it->scopes.push_back(timing);

        registerPath(timing.pathHash, 0, literalName);
        return;
    }
    // Frame already flushed out of the window -- drop rather than misattribute.
}

} // namespace

void Profiler::recordGpuZone(uint64_t frame, const char* literalName, double startUs, double durationMs) {
    auto& inst = instance();
    if (!inst.impl || !inst.impl->config.enabled) return;

    std::lock_guard<std::mutex> lock(inst.impl->mutex);
    appendGpuZone(inst.impl->pendingFrames, frame, literalName, durationMs,
                  [startUs](const FrameData&) { return startUs; });
}

void Profiler::recordCalibratedGpuZone(uint64_t frame, const char* literalName, int64_t hostStartNs,
                                       double durationMs) {
    auto& inst = instance();
    if (!inst.impl || !inst.impl->config.enabled) return;

    std::lock_guard<std::mutex> lock(inst.impl->mutex);
    appendGpuZone(inst.impl->pendingFrames, frame, literalName, durationMs, [hostStartNs](const FrameData& f) {
        // Clamped, never negative. The calibration is a linear fit around one sample pair,
        // so a zone near the frame boundary can map a few hundred nanoseconds before it,
        // and a negative `ts` makes the whole trace unreadable rather than one event early.
        const int64_t delta = hostStartNs - f.startNs;
        return static_cast<double>(delta > 0 ? delta : 0) / 1000.0;
    });
}

ProfileScope::ProfileScope(const char* scopeName, uint64_t scopePathHash, uint32_t scopeDepth, uint32_t scopeThreadId)
    : name(scopeName)
    , pathHash(scopePathHash)
    , startNs(nowNs())
    , depth(scopeDepth)
    , threadId(scopeThreadId)
    , valid(true) {}

ProfileScope::~ProfileScope() {
    if (!valid) return;

    const int64_t endNs = nowNs();

    auto& inst = Profiler::instance();
    if (inst.impl && inst.impl->frameStarted.load(std::memory_order_acquire)) {
        const int64_t frameStart = inst.impl->frameStartNs.load(std::memory_order_relaxed);

        ScopeTiming timing;
        timing.name = name;
        timing.pathHash = pathHash;
        timing.depth = depth;
        timing.threadId = threadId;
        // A worker-thread scope can straddle a frame boundary. Attributed to the frame it
        // *closes* in, and clamped, or the trace gets a negative offset.
        timing.startTimeUs = static_cast<double>(startNs > frameStart ? startNs - frameStart : 0) / 1000.0;
        timing.cpuTimeMs = static_cast<double>(endNs - startNs) / 1000000.0;
        inst.impl->record(timing);
    }

    if (!t_stack.empty()) t_stack.pop_back();
}

ProfileScope::ProfileScope(ProfileScope&& other) noexcept
    : name(other.name)
    , pathHash(other.pathHash)
    , startNs(other.startNs)
    , depth(other.depth)
    , threadId(other.threadId)
    , valid(other.valid) {
    other.valid = false;
}

} // namespace core
