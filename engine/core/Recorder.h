#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core {

class AudioTap;

/// Most copies of one frame `framesOwedAt` will ever ask for. Four at 30 fps is an
/// eighth of a second of frozen picture, which is where a repeat stops reading as a
/// stutter and starts reading as a hang.
inline constexpr uint32_t kMaxFrameRepeat = 4;

/**
 * @brief How many frames a file at `fps` is short, `delivered` frames in.
 *
 * Free, stateless and in the header because it is the whole of the pacing rule and the
 * only part of this class a test can reach without an encoder behind it. Deriving the
 * answer from elapsed time rather than from a running total is the point: a frame the
 * game never drew, or one this refused to repeat, does not accumulate into drift.
 */
[[nodiscard]] uint32_t framesOwedAt(double elapsedSeconds, uint32_t fps, uint64_t delivered);

/**
 * @file engine/core/Recorder.h
 * @brief Record a running session to an mp4, in real time, with sound (S7).
 *
 * ## What this is not
 *
 * It is not `--capture`, and the difference is the whole design. A screenshot copies
 * one image and blocks on the fence that finishes it, which costs a frame and is
 * exactly the right trade for something that happens on a keypress. Thirty of those a
 * second would stall the render thread thirty times a second. So nothing here ever
 * blocks the thread that draws: the renderer hands over a pointer and returns, and
 * every expensive thing -- the pipe write, the encoder, the muxer -- happens on the
 * worker this class owns.
 *
 * It is also not a screen recorder. It records the images Substrate presented and the
 * samples Substrate mixed, and it cannot capture anything else on the display even in
 * principle. That is a correctness property rather than a feature: a recorder that
 * grabs a screen region grabs whatever is in that region.
 *
 * ## The shape
 *
 * Two ffmpeg processes, each with one pipe, writing two intermediate files; one more
 * ffmpeg at `stop()` that trims both to the window and muxes them into the mp4. Two
 * processes rather than one because one process needs two input streams, and a second
 * pipe into a child is a named FIFO and an opening-order deadlock to get wrong. Two
 * `popen` calls have neither problem, and the price -- a mux pass over data that is
 * already compressed -- is a `-c copy` and costs about a second.
 *
 * ## Keeping sound aligned with picture
 *
 * Both streams are clocked by real time and neither is clocked by the game. Audio
 * arrives at the device's rate, and whatever the ring could not fit is replaced by
 * exactly that much silence rather than spliced out -- splicing shortens the audio
 * against the picture, which is drift that grows. Video is paced the same way, from
 * the wall clock: `framesOwed()` says how many frames the file is short, so a game
 * running at 200 fps has most of its frames ignored and one at 20 fps has its frames
 * repeated. Neither case changes how long the recording claims to be.
 *
 * ## The window
 *
 * `windowSeconds` keeps the *end* of the session, not the beginning: recording runs
 * for as long as the game does and `stop()` trims to the last N seconds. Keeping the
 * tail is what makes the feature useful, because you know you want a recording of the
 * thing that just happened after it happens.
 */
class Recorder {
  public:
    struct Options {
        /// The mp4 to write. Parent directories are created.
        std::filesystem::path path;
        uint32_t width = 0;
        uint32_t height = 0;
        /// Frames per second of the *file*. The game's frame rate is unrelated and is
        /// not required to be stable.
        uint32_t fps = 30;
        /// Seconds kept, counting back from the end. 0 keeps the whole session.
        double windowSeconds = 30.0;
        /// ffmpeg's name for the layout the renderer hands over -- `bgra` or `rgba`.
        /// The renderer derives it from the swapchain format; nothing here inspects
        /// pixels, it only forwards bytes.
        std::string pixelFormat = "bgra";
        uint32_t sampleRate = 48000;
        uint32_t channels = 2;
    };

    Recorder() = default;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    /**
     * @brief Spawn the encoders and start the worker.
     *
     * `audio` may be null, in which case the recording is silent; it must outlive the
     * recorder otherwise. False means it could not start and the reason is logged --
     * no ffmpeg, an unwritable path, an implausible size.
     */
    bool start(Options options, AudioTap* audio);

    /// True between a successful `start()` and `stop()`.
    [[nodiscard]] bool active() const { return running.load(std::memory_order_acquire); }

    /// What `start()` was given, filled in with any defaults it applied.
    [[nodiscard]] const Options& settings() const { return opt; }

    /**
     * @brief How many copies of the next frame the file is currently short.
     *
     * Render thread. `elapsedSeconds` is measured from the start of the recording.
     * Zero means the file is up to date and this frame is not needed -- which is the
     * common case above 30 fps, and the point: a frame that is not owed costs no
     * readback, no copy and no encode.
     *
     * Calling this *reserves* the frames, so it must be followed by a `submitFrame`
     * carrying the count it returned. That is what lets a dropped submission repay
     * itself later rather than silently shortening the video.
     */
    uint32_t framesOwed(double elapsedSeconds);

    /**
     * @brief Render thread. Hand over one frame, to be written `repeat` times.
     *
     * Copies out of `pixels` and returns; the caller may reuse the memory immediately.
     * Never blocks on the encoder: when the queue is full the frame is dropped, and
     * its `repeat` is carried forward onto the next frame that does fit, so a stall
     * costs a moment of judder rather than a video that runs fast.
     */
    void submitFrame(const void* pixels, size_t bytes, uint32_t repeat);

    /**
     * @brief Close the pipes, wait for the encoders, then trim and mux.
     *
     * Blocking, and the one place this is expensive on purpose: it runs at shutdown,
     * where a second spent finishing the file is a second nobody is waiting on a frame
     * for. Returns the file written, or an empty path on failure.
     */
    std::filesystem::path stop();

    /// Frames handed to the encoder, frames the queue could not take, and frames
    /// actually written to the pipe -- which includes repeats and so exceeds the
    /// first. For the log line at `stop()` and for tests.
    [[nodiscard]] uint64_t framesSubmitted() const { return submitted.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t framesDropped() const { return dropped.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t framesWritten() const { return written.load(std::memory_order_relaxed); }

  private:
    /// Worker. Drains the video queue and the audio ring until stopped, then drains
    /// whatever is left so the tail of the session is not truncated.
    void encodeLoop();
    /// Move one queued frame to the video pipe. False when there was nothing queued.
    bool pumpVideo();
    /// Move whatever the audio tap holds to the audio pipe, silence included.
    void pumpAudio();
    /// Run the trim-and-mux pass. False leaves the intermediates on disk to look at.
    bool muxOutput();
    /// Close both encoder pipes. Called from `stop()`, after the worker is joined and
    /// never before -- see the comment there.
    void closePipes();

    Options opt;
    AudioTap* tap = nullptr;

    /// One captured image plus how many times the file wants it. The repeat lives here
    /// rather than being expanded into copies because the frames are megabytes each
    /// and a duplicate is the same bytes written twice.
    struct QueuedFrame {
        std::vector<uint8_t> pixels;
        uint32_t repeat = 1;
    };
    /// Bounded, and guarded by an ordinary mutex rather than by the lock-free
    /// discipline `AudioTap` needs. The producer here is the render thread, which
    /// already waits on fences and on the presentation engine; an uncontended lock it
    /// holds for a `memcpy` is not the hazard the same lock would be on the audio
    /// thread, and a deque of pooled buffers is a great deal easier to be sure of.
    std::deque<QueuedFrame> queue;
    /// Retired frame buffers, so a steady state allocates nothing.
    std::vector<std::vector<uint8_t>> spare;
    mutable std::mutex queueMutex;
    std::condition_variable queueSignal;
    size_t queueLimit = 4;

    /// Repeats belonging to frames the queue refused, owed to the next one it accepts.
    uint32_t carriedRepeat = 0;
    /// Frames `framesOwed` has handed out. Render thread only.
    uint64_t reserved = 0;

    std::FILE* videoPipe = nullptr;
    std::FILE* audioPipe = nullptr;
    std::filesystem::path videoIntermediate;
    std::filesystem::path audioIntermediate;

    std::thread worker;
    /// Whether a session is still being fed. Cleared by `stop()`, and also by the worker
    /// itself when the encoder stops reading -- which is why it cannot be what teardown
    /// keys on.
    std::atomic<bool> running{false};
    /// Whether there is a thread to join and pipes to close. Set once `start()` has both,
    /// cleared by `stop()` once it has given them back. `running` goes false on its own
    /// when the encoder dies; this does not, so it is what `stop()` and the destructor
    /// test. Plain rather than atomic: only the owning thread touches it.
    bool started = false;
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> written{0};

    /// Audio frames the tap reported lost, already paid for in silence.
    uint64_t silenceWritten = 0;
    /// Scratch for a drain, sized once. Worker thread only.
    std::vector<float> audioScratch;
};

/// True when ffmpeg is on PATH. Separate from `start()` so a caller can refuse the
/// flag up front with a useful message instead of failing once the window is open.
[[nodiscard]] bool recordingToolsAvailable();

} // namespace core
