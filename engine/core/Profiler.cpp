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

// ---------------------------------------------------------------- hashing
// Scope paths are folded into a 64-bit FNV-1a hash rather than concatenated into
// a string. That keeps the recording path free of allocation; the readable path
// string is materialised once per unique path, on the cold side.
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

// ------------------------------------------------------------ path strings
std::mutex g_pathMutex;
std::unordered_map<uint64_t, std::string> g_pathStrings;
thread_local std::unordered_set<uint64_t> t_knownPaths;

void registerPath(uint64_t hash, uint64_t parentHash, const char* name) {
    // Thread-local set means the steady-state case takes no lock at all.
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

// -------------------------------------------------------- dynamic name pool
// scopef() names must outlive the frame they were recorded in, so they are
// interned. A deque never invalidates references to existing elements, which is
// what makes the returned c_str() stable for the process lifetime.
//
// Interning is permanent, so an unbounded pool is a slow leak the moment a caller
// formats something that varies every frame -- scopef("Upload %llu", frameNumber)
// is the obvious mistake. The pool is capped instead: past the cap, names collapse
// onto one bucket and a warning names the offender. Bounded and diagnosable beats
// correct-until-someone-holds-it-wrong.
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

// ------------------------------------------------------------- thread state
/// Trace row for GPU zones. Well above any plausible thread slot id.
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

/// One quantity, by name. `name` is a literal held by pointer, exactly as a scope's is.
struct CounterSample {
    const char* name = nullptr;
    double value = 0.0;
};

/**
 * @brief Per-thread recording buffer, recycled when a thread exits.
 *
 * Slots are reused rather than accumulated, for two reasons: an engine that spawns
 * short-lived jobs would otherwise grow the registry without bound, and Chrome
 * Tracing would render one near-empty row per thread ever created.
 *
 * `guard` is a spinlock, not a mutex. Exactly one thread ever pushes to a slot and
 * only the collector drains it, so contention is near-zero — but the access still
 * has to be synchronised, or a worker recording mid-frame races the collector.
 */
struct ThreadSlot {
    std::vector<ScopeTiming> scopes;

    /// Names this slot has been given since the last collect, in the order they were
    /// given. Drained into the frame beside `scopes`, under the same spinlock and by the
    /// same caller, which is what keeps this off the recording path's fast route.
    ///
    /// `nullptr` is a real entry and means *unnamed*: it is pushed by `acquireSlot` on
    /// every acquisition, so a thread that never names itself cannot inherit the label of
    /// whichever thread held the slot before it.
    std::vector<const char*> pendingNames;

    /// The name this acquisition has already emitted, so naming is idempotent.
    const char* name = nullptr;

    /// This frame's counters, one entry per distinct name. A second write of a name
    /// overwrites rather than appends -- which is the last-value-per-frame contract and
    /// is also what bounds this vector, so the recording path never allocates after the
    /// first frame that used a given name.
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
        // Any scopes still buffered stay put; the next collectFrame() drains them.
        t_slot->inUse = false;
        t_slot = nullptr;
    }
};
thread_local ThreadSlotGuard t_slotGuard;

ThreadSlot& acquireSlot() {
    if (t_slot != nullptr) return *t_slot;

    (void)&t_slotGuard; // odr-use so the thread-exit destructor is registered

    // Every acquisition pushes a name, and it is `nullptr` until somebody says otherwise.
    // That is the whole of the recycling fix: the slot the scene-load worker used carries
    // its label until the recorder takes it, and this resets it to "thread N" on the spot
    // rather than leaving the recorder's work under the loader's name.
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
    /// steady_clock nanoseconds at which this frame opened. Kept so a GPU zone carrying
    /// a calibrated host timestamp can be turned into an offset within the frame long
    /// after the frame closed (5.4) -- the results arrive several frames late, by which
    /// point `frameStartNs` has moved on three times.
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

// --------------------------------------------------------- signal handling
std::atomic<bool> g_fileClosed{false};
std::atomic<bool> g_signalFlushRequested{false};
std::atomic<bool> g_signalFlushDone{false};
bool g_handlersRegistered = false;
std::string g_outputFilePath;

} // namespace

// ============================================================== Profiler::Impl

struct Profiler::Impl {
    ProfilerConfig config;

    mutable std::mutex mutex;

    uint64_t frameCounter = 0;

    // Frame state is read by every closing scope on every thread while the main
    // thread flips it at frame boundaries, so it has to be atomic. Ordering matters:
    // frameStartNs is published before frameStarted, and read after it, so anyone who
    // observes an open frame also observes a valid start time.
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
            // Counters are cleared rather than carried: a counter is a statement about
            // *this* frame, and a caller that stops writing one should see the track stop
            // rather than see the last value repeated forever.
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
            // Frame 0 is the startup frame: window creation, device init and asset
            // load all happen inside it and never happen again. Ageing it out of a
            // rolling window leaves the trace with no record of the most expensive
            // work the process ever does, so it is pinned and frame 1 evicts instead.
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

            // Frame 0 is pinned here too. pendingFrames is drained into writeQueue on
            // the first flush, so without this the startup frame survives its own
            // window only to be trimmed out of the one that actually gets written.
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
        // Rewrite the whole file each time so it always holds exactly the current
        // window and is always a complete, valid JSON array.
        std::ofstream file(writerOutputPath, std::ios::trunc);
        if (!file.is_open()) return;
        writeTrace(file, frames);
    }

    static void writeTrace(std::ostream& out, const std::deque<FrameData>& frames) {
        out << "[\n";

        double cumulativeUs = 0.0;
        bool needsComma = false;

        // The GPU track is not a thread and never acquires a slot, so nothing else would
        // ever name it -- and an unlabelled `1000` beside labelled CPU tracks is the exact
        // complaint this fixes, one row further down the list.
        out << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" << kGpuTrackId
            << ",\"args\":{\"name\":\"GPU\"}}";
        needsComma = true;

        for (const auto& frame : frames) {
            const double frameBaseUs = cumulativeUs;
            cumulativeUs += frame.durationUs;

            // Metadata first within the frame, so a track is named before the events that
            // land on it. Chrome's `M` phase carries no duration and Perfetto ignores its
            // `ts` -- a second event for the same `tid` replaces the first, which is
            // exactly what a recycled slot needs and is why this emits per acquisition
            // rather than once per slot at the end.
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

            // **On the same synthetic cumulative timeline the scopes use.** `writeTrace`
            // emits no wall-clock time -- it concatenates frames by `cumulativeUs` -- so a
            // counter stamped from `steady_clock` at the call site would draw a graph that
            // does not sit above the zones it explains. That is the whole reason the emit
            // is here and not beside the caller.
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

// ============================================================ signal handling

namespace {

extern "C" void profilerSignalHandler(int sig) {
    // Async-signal-safe only: atomics and a sleep. No mutexes, no allocation, no
    // stdio. The writer thread does the actual work; we just wait briefly for it.
    //
    // Sleep() is the Windows equivalent and is safe here for the same reason nanosleep is:
    // it is a bare kernel call that takes no CRT lock. Worth knowing that this handler
    // barely runs on Windows at all -- SIGTERM is never raised by the OS and SIGINT only
    // arrives for Ctrl-C in a console process, delivered on a separate thread. The trace
    // still lands, through the atexit handler and the writer thread's 100 ms flush; what
    // is lost is only the flush on a kill, and `timeout -s TERM` has no Windows analogue
    // to lose it from.
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

    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

} // namespace

// =================================================================== Profiler

Profiler& Profiler::instance() {
    static Profiler inst;
    return inst;
}

void Profiler::init(const ProfilerConfig& config) {
    auto& inst = instance();
    inst.impl = std::make_unique<Impl>();
    inst.impl->config = config;

    // **`enabled` is tested here and not only on the recording path.** A disabled profiler
    // that still truncated the trace, still registered the signal handlers and still
    // started a writer thread was `--no-profiler --trace <path>` leaving a zero-byte file
    // and a thread behind -- the same flag saying "no trace" and producing one, one layer
    // down from the GPU query pool it also used to leave running.
    if (config.enabled && !config.outputFile.empty()) {
        // Created here for the same reason Logger::init creates the log's, and it is this
        // one that matters: the default trace goes to debug_frames/, which existed only
        // because run.sh happened to mkdir it. A packaged build has no run.sh, so without
        // this the truncate below fails silently and the trace never appears -- with no
        // message, because an ofstream that cannot open is not an error until someone asks.
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
            g_handlersRegistered = true;
        }

        if (config.autoFlushFrames > 0) {
            inst.impl->startWriterThread(config.outputFile);
        }
    }

    // **A new profiler is a new trace, and the metadata already emitted went into the old
    // one.** The registry deliberately outlives `shutdown()` -- see `closeOutputFile` --
    // so a second `init` in one process inherits slots that are already named and whose
    // names `nameThread` would therefore decline to re-emit, leaving every track in the
    // new trace unlabelled. Re-announcing every live acquisition here is what makes the
    // second trace look like the first.
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        for (auto& slot : g_registry) {
            if (!slot->inUse) continue;
            slot->lock();
            slot->pendingNames.push_back(slot->name);
            slot->unlock();
        }
    }

    // The thread that brings the profiler up is the one that will run the frames, so it
    // names itself here rather than leaving every caller to remember. A test or a tool
    // that inits from a worker gets a track labelled `main` that is not the process's main
    // thread, which is the honest reading: it is the thread this profiler's frames belong
    // to.
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

    // Emptied, not freed, and `inUse` is left exactly as it is. `t_slot` is a thread_local
    // raw pointer into this registry, and this function can only reach the *calling* thread's
    // copy of it -- every other live thread would be left pointing into deleted storage, and
    // would write through it on its next `Profiler::scope()` or from `ThreadSlotGuard` at
    // thread exit. Clearing `inUse` would be its own bug: a slot handed to a second thread
    // while the first still holds the pointer is two writers on one buffer.
    //
    // So the registry deliberately outlives `shutdown()`. It is a pool bounded by the number
    // of threads that have ever profiled -- which is what the recycling at `acquireSlot` is
    // for -- and a following `init()` picks it up as it stands.
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

    // Logger::vformat rather than a buffer of this function's own. A zone name is built
    // from whatever the call site interpolates -- a scene path, a pass name, an index --
    // and a truncated one interns as a *different* name, so the cost of the old 256-byte
    // buffer was a silently split zone rather than a shortened label.
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
    // Dropped rather than buffered when no frame is open. A counter's whole meaning is
    // "this was true during frame N", and there is no N here.
    if (!inst.impl->frameStarted.load(std::memory_order_acquire)) return;

    ThreadSlot& slot = acquireSlot();
    slot.lock();
    // Linear, because the list is the handful of quantities one thread writes per frame
    // and a map would allocate. Pointer comparison for the same reason `scope` hashes
    // pointers: these are literals.
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
    // Pointer comparison, not `strcmp`: names are literals, so the same name is the same
    // pointer, and this is called from a device callback that runs hundreds of times a
    // second. A caller passing two equal strings at different addresses gets two identical
    // metadata events, which costs a line in the trace and renders the same.
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

/// Append a GPU zone to a buffered frame. `offsetUs` is resolved against the frame by
/// the caller's lambda, which is the only thing the two entry points disagree about.
/// Must be called with the profiler's mutex held.
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
        // Clamped rather than allowed negative. The calibration is a linear fit around
        // one sample pair, so a zone very near the frame boundary can map a few hundred
        // nanoseconds before it -- and a negative `ts` makes the whole trace unreadable
        // rather than one event slightly early.
        const int64_t delta = hostStartNs - f.startNs;
        return static_cast<double>(delta > 0 ? delta : 0) / 1000.0;
    });
}

// =============================================================== ProfileScope

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
        // A scope on a worker thread can straddle a frame boundary: it is attributed
        // to the frame it *closes* in, and clamped so the trace stays well-formed.
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
