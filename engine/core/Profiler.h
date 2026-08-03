#pragma once

#include "core/Format.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace core {

/**
 * @brief Configuration for the profiler.
 *
 * **Every default here matches the `profiler` block of the settings table**, in
 * `core/Settings.h`, with one deliberate exception noted on `outputFile`. They used to
 * disagree in three of six fields -- maxFrames 120 against 240, autoFlushFrames 300
 * against 120, clearAfterFlush true against false -- which meant the profiler a test or
 * any direct caller got was configured differently from the one `Engine::init` builds,
 * and which one applied depended on how you had arrived. The `physics` and `audio` pairs
 * agree in every field and are the model; this is now the third.
 *
 * The settings table is the side that moves. It is what a `substrate.json` and a
 * command-line flag edit, so a value changed there and not here reinstates the split.
 */
struct ProfilerConfig {
    bool enabled = true;        ///< Master enable/disable
    uint32_t averagingWindow = 60; ///< Rolling average window, in frames
    uint32_t maxFrames = 240;   ///< Max frames kept in memory (FIFO; 0 = unlimited)

    /// Chrome Tracing output path; empty disables file output.
    ///
    /// The one field that deliberately differs from the settings default, which is
    /// `debug_frames/profile.json`. Matching it would make merely constructing a
    /// `ProfilerConfig` -- in a unit test, or in any caller that is not `Engine` -- name a
    /// path that gets written to. A library default of "write nothing to disk" is the
    /// answer for a caller that did not ask for a trace; the engine asks, in Engine.cpp.
    std::string outputFile;
    uint32_t autoFlushFrames = 120; ///< Flush every N frames (0 = never)
    bool clearAfterFlush = false;   ///< Reset rolling averages after each flush
};

class ProfileScope;

/**
 * @brief Hierarchical CPU profiler with Chrome Tracing export.
 *
 * Scopes nest automatically by call stack and are recorded into thread-local
 * storage, so recording never contends on a mutex. Frames are merged at
 * beginFrame() and written by a background thread.
 *
 * The steady-state recording path performs **no heap allocation**: scope names are
 * string literals identified by pointer, hierarchy paths are folded into a 64-bit
 * hash, and per-thread storage is reserved up front. This matters because an
 * always-on profiler that allocates is measuring its own overhead.
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
     * @brief Open a timing scope with a formatted name.
     *
     * Allocates on first sighting of each distinct name, then interns it. Use only
     * where the name is genuinely dynamic, e.g. scopef("Shadow %u", i).
     *
     * **The set of names must be bounded.** Interning is permanent, so a name that
     * varies with the frame number or a pointer value grows the pool forever. The
     * pool is capped at 4096 distinct names; beyond that, new names collapse onto a
     * single `<scopef pool full>` bucket and a warning is logged once.
     */
    [[nodiscard]] __attribute__((format(SUBSTRATE_PRINTF_FORMAT, 1, 2))) static ProfileScope scopef(const char* fmt, ...);

    /**
     * @brief Record a quantity against the current frame. Allocation-free.
     *
     * @param literalName Must have static lifetime (a string literal). Stored by pointer.
     * @param value       The quantity. Emitted as a Chrome `ph:"C"` counter, which
     *                    Perfetto renders as a track graph above the zones it explains.
     *
     * **Last value per frame, not a stack.** Two writes of one name in one frame are a
     * caller correcting itself rather than two events, so the second replaces the first —
     * which is also what bounds the per-frame storage and keeps this allocation-free.
     *
     * A counter answers what a duration cannot: a zone can say a frame got slower and
     * never why. Draw calls, live instances, what the cull rejected, VRAM.
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
     * The name belongs to the *acquisition*, not to the slot. Slots are recycled — the
     * scene-load worker's slot becomes the recorder's later in the same run — so a name
     * attached to the slot would label the second thread's work with the first thread's
     * name, which is worse than no label at all. Each acquisition emits its own
     * `thread_name` metadata event and Perfetto takes the latest.
     *
     * Idempotent: calling it again with the same pointer emits nothing, so a device
     * callback may name itself on every invocation.
     */
    static void nameThread(const char* literalName);

    static void setEnabled(bool enabled);
    static bool enabled();

    /// Frame index of the frame currently being recorded.
    static uint64_t frameNumber();

    /**
     * @brief Record a GPU timing zone into an already-buffered frame.
     *
     * GPU results arrive several frames after the work was submitted, so they are
     * back-dated into the frame they belong to rather than the frame that collected
     * them. Dropped silently if that frame has already been flushed out of the
     * window. GPU zones land on their own Chrome Tracing track.
     *
     * @param startUs Offset from the frame's first GPU timestamp, not from CPU zero:
     *                the two clocks are uncorrelated. Durations are exact.
     *
     * This is the *uncalibrated* placement, and it is a lie the trace tells knowingly:
     * it puts the frame's first GPU timestamp at the frame's CPU start, when in truth
     * the GPU is still working through a frame the CPU finished recording. Prefer
     * recordCalibratedGpuZone where the device supports it (5.4).
     */
    static void recordGpuZone(uint64_t frameNumber, const char* literalName, double startUs, double durationMs);

    /**
     * @brief Record a GPU zone whose start is a real time on the CPU's own clock (5.4).
     *
     * `VK_EXT_calibrated_timestamps` gives a matched (device tick, host nanosecond)
     * pair, which is what turns a device tick into a point on the same
     * `std::chrono::steady_clock` timeline every CPU scope is already stamped against.
     * The result is a trace where the GPU row sits *after* the CPU row that submitted
     * it, by the queue latency, instead of on top of it.
     *
     * @param hostStartNs steady_clock nanoseconds. Clamped to the frame's start, so a
     *                    zone whose calibration drifted backwards past the frame
     *                    boundary lands at zero rather than making the trace negative.
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

/**
 * @brief RAII CPU timing scope. Move-only; records on destruction.
 */
class ProfileScope {
  public:
    ProfileScope() = default;
    ~ProfileScope();

    /// Move construction is what makes `auto s = Profiler::scope(...)` work: the factory
    /// builds the scope and hands it over, and the source is left inert.
    ProfileScope(ProfileScope&& other) noexcept;

    /**
     * Move *assignment* is deleted, and not as a matter of taste.
     *
     * Closing a scope pops a thread-local LIFO stack. Assigning over a scope that is
     * still open would have to close it -- and it cannot be closed, because whatever is
     * being assigned *from* was opened later and therefore sits above it on that stack.
     * There is no ordering in which both pops are legal, so there is no correct body to
     * write. Left defaulted, the operation silently leaked one stack entry and every
     * later scope on that thread reported the wrong depth.
     *
     * Deleting it makes the one shape that cannot work a compile error instead. Nothing
     * needs it: a scope is created where it is used and destroyed at the end of that
     * block, which is the whole of what an RAII timer is for. If a caller ever genuinely
     * needs to rebind one, the honest spelling is an `std::optional<ProfileScope>` and an
     * explicit `reset()`, which closes the old scope at a point the caller chose.
     */
    ProfileScope& operator=(ProfileScope&& other) noexcept = delete;

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

  private:
    friend class Profiler;
    ProfileScope(const char* name, uint64_t pathHash, uint32_t depth, uint32_t threadId);

    const char* name = nullptr;
    uint64_t pathHash = 0;
    int64_t startNs = 0; ///< steady_clock nanoseconds; compared against the frame's atomic start
    uint32_t depth = 0;
    uint32_t threadId = 0;
    bool valid = false;
};

} // namespace core
