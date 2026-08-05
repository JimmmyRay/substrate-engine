#pragma once

#include "core/Format.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace core {

/// @brief Configuration for the profiler.
///
/// Every default here has to match `Config`'s `profiler` block, with the one exception
/// noted on `outputFile`. Where they disagree, a test or any direct caller gets a
/// differently configured profiler from the one `Engine::init` builds.
struct ProfilerConfig {
    bool enabled = true;        ///< Master enable/disable
    uint32_t averagingWindow = 60; ///< Rolling average window, in frames
    uint32_t maxFrames = 240;   ///< Max frames kept in memory (FIFO; 0 = unlimited)

    /// Chrome Tracing output path; empty disables file output.
    ///
    /// Deliberately empty where `Config` defaults it to a path. Filling it in here makes
    /// merely constructing a `ProfilerConfig` -- in a unit test, or any caller that is not
    /// `Engine` -- name a file that gets written to.
    std::string outputFile;
    uint32_t autoFlushFrames = 120; ///< Flush every N frames (0 = never)
    bool clearAfterFlush = false;   ///< Reset rolling averages after each flush
};

class ProfileScope;

/**
 * @brief Hierarchical CPU profiler with Chrome Tracing export.
 *
 * Scopes nest by call stack into thread-local storage, so recording never takes a mutex.
 * Frames are merged at `beginFrame()` and written by a background thread; anything added to
 * the recording path that reaches shared state has to be synchronised against both.
 *
 * The steady-state recording path must not allocate -- an always-on profiler that allocates
 * is measuring its own overhead. Scope names are literals identified by pointer, hierarchy
 * paths fold into a 64-bit hash, and per-thread storage is reserved up front.
 *
 * @code
 * Profiler::init({.outputFile = "debug_frames/profile.json"});
 * while (running) {
 *     auto frame = Profiler::beginFrame();
 *     {
 *         auto s = Profiler::scope("Renderer::draw");
 *         ...
 *     }
 * }
 * Profiler::shutdown();
 * @endcode
 */
class Profiler {
  public:
    static void init(const ProfilerConfig& config = {});
    static void shutdown();

    /**
     * @brief Open a timing scope. Allocation-free.
     * @param literalName Must have static lifetime (a string literal). Stored by pointer.
     */
    [[nodiscard]] static ProfileScope scope(const char* literalName);

    /**
     * @brief Open a timing scope with a formatted name. Allocates on first sighting of each
     *        distinct name, then interns it permanently.
     *
     * The set of names must be bounded: one varying with the frame number or a pointer
     * value grows the pool forever. It is capped at 4096, past which new names collapse
     * onto a single `<scopef pool full>` bucket.
     */
    [[nodiscard]] __attribute__((format(SUBSTRATE_PRINTF_FORMAT, 1, 2))) static ProfileScope scopef(const char* fmt, ...);

    /**
     * @brief Record a quantity against the current frame. Allocation-free.
     *
     * @param literalName Must have static lifetime (a string literal). Stored by pointer.
     * @param value       The quantity, emitted as a Chrome `ph:"C"` counter.
     *
     * Last value per frame, not a stack: the second write of one name replaces the first,
     * which is what bounds the per-frame storage and keeps this allocation-free.
     */
    static void counter(const char* literalName, double value);

    /**
     * @brief Close the previous frame and open a new one.
     * @return Scope timing the whole frame — hold it until the frame ends.
     */
    [[nodiscard]] static ProfileScope beginFrame();

    /// Print a rolling-average table for the most recent frame.
    static void dump();

    /// Most recent frame as a JSON string.
    static std::string toJson();

    /// Write the buffered frames to a Chrome Tracing file. Overwrites.
    static bool writeToFile(const std::string& path);

    /**
     * @brief Label the calling thread's track in the trace.
     *
     * @param literalName Must have static lifetime (a string literal). Stored by pointer.
     *
     * The name belongs to the *acquisition*, not the slot: slots are recycled -- the
     * scene-load worker's becomes the recorder's later in the same run -- so a name held on
     * the slot labels the second thread's work with the first thread's name.
     *
     * Idempotent for the same pointer, so a device callback may name itself every call.
     */
    static void nameThread(const char* literalName);

    static void setEnabled(bool enabled);
    static bool enabled();

    /// Frame index of the frame currently being recorded.
    static uint64_t frameNumber();

    /**
     * @brief Record a GPU timing zone into an already-buffered frame.
     *
     * GPU results arrive several frames after submission, so they are back-dated into the
     * frame they belong to. Silently dropped if that frame has already been flushed.
     *
     * @param startUs Offset from the frame's first GPU timestamp, not from CPU zero: the
     *                two clocks are uncorrelated. Durations are exact.
     *
     * The placement is uncalibrated, and knowingly wrong -- it puts the frame's first GPU
     * timestamp at the frame's CPU start. Prefer `recordCalibratedGpuZone` where the device
     * supports it.
     */
    static void recordGpuZone(uint64_t frameNumber, const char* literalName, double startUs, double durationMs);

    /**
     * @brief Record a GPU zone whose start is a real time on the CPU's own clock.
     *
     * Needs `VK_EXT_calibrated_timestamps` to have put the device tick onto the same
     * `std::chrono::steady_clock` timeline every CPU scope is stamped against.
     *
     * @param hostStartNs steady_clock nanoseconds, clamped to the frame's start: a zone
     *                    whose calibration drifted backwards past the frame boundary would
     *                    otherwise make the trace negative.
     */
    static void recordCalibratedGpuZone(uint64_t frameNumber, const char* literalName, int64_t hostStartNs,
                                        double durationMs);

    /// Flush and close the output file. Idempotent; also runs via atexit.
    static void closeOutputFile();

  private:
    friend class ProfileScope;

    Profiler() = default;
    static Profiler& instance();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// @brief RAII CPU timing scope. Move-only; records on destruction.
class ProfileScope {
  public:
    ProfileScope() = default;
    ~ProfileScope();

    ProfileScope(ProfileScope&& other) noexcept;

    /**
     * Move assignment cannot be written correctly and must stay deleted.
     *
     * Closing a scope pops a thread-local LIFO stack. Assigning over a still-open scope
     * would have to close it, and it cannot be: whatever is being assigned *from* was
     * opened later and sits above it on that stack. Defaulted, it silently leaks one stack
     * entry and every later scope on that thread reports the wrong depth. A caller that
     * genuinely needs to rebind one wants `std::optional<ProfileScope>` and an explicit
     * `reset()`.
     */
    ProfileScope& operator=(ProfileScope&& other) noexcept = delete;

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

  private:
    friend class Profiler;
    ProfileScope(const char* name, uint64_t pathHash, uint32_t depth, uint32_t threadId);

    const char* name = nullptr;
    uint64_t pathHash = 0;
    int64_t startNs = 0; ///< steady_clock nanoseconds, compared against the frame's atomic start
    uint32_t depth = 0;
    uint32_t threadId = 0;
    bool valid = false;
};

} // namespace core
