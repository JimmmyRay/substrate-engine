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

/// Most copies of one frame `framesOwedAt` will ever ask for. Four at 30 fps is an eighth
/// of a second of frozen picture, past which a repeat reads as a hang rather than a
/// stutter.
inline constexpr uint32_t kMaxFrameRepeat = 4;

/// @brief How many frames a file at `fps` is short, `delivered` frames in.
///
/// Derived from elapsed time rather than from a running total, so a frame the game never
/// drew, or one this refused to repeat, does not accumulate into drift.
[[nodiscard]] uint32_t framesOwedAt(double elapsedSeconds, uint32_t fps, uint64_t delivered);

/**
 * @file engine/core/Recorder.h
 * @brief Record a running session to an mp4, in real time, with sound.
 *
 * Nothing here may block the thread that draws: the renderer hands over a pointer and
 * returns, and the pipe write, the encoder and the muxer all run on the worker this class
 * owns. Blocking on a fence the way `--capture` does costs a frame, which is the right
 * trade once on a keypress and not thirty times a second.
 *
 * Two ffmpeg processes, one pipe each, writing two intermediates; a third at `stop()` trims
 * both to the window and muxes. One process instead would need two input streams, and a
 * second pipe into a child is a named FIFO with an opening-order deadlock to get wrong.
 *
 * Both streams are clocked by real time, never by the game. Dropped audio is replaced by
 * exactly that much silence rather than spliced out, because splicing shortens the audio
 * against the picture and the drift grows. Video is paced from the wall clock the same way.
 *
 * `windowSeconds` keeps the *end* of the session: recording runs as long as the game does
 * and `stop()` trims to the last N seconds.
 */
class Recorder {
  public:
    struct Options {
        /// The mp4 to write. Parent directories are created.
        std::filesystem::path path;
        uint32_t width = 0;
        uint32_t height = 0;
        /// Frames per second of the *file*. Unrelated to the game's frame rate, which is
        /// not required to be stable.
        uint32_t fps = 30;
        /// Seconds kept, counting back from the end. 0 keeps the whole session.
        double windowSeconds = 30.0;
        /// ffmpeg's name for the layout the renderer hands over -- `bgra` or `rgba`. Wrong
        /// here swaps the channels in the file: nothing inspects pixels, it forwards bytes.
        std::string pixelFormat = "bgra";
        uint32_t sampleRate = 48000;
        uint32_t channels = 2;
    };

    Recorder() = default;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    /// @brief Spawn the encoders and start the worker.
    ///
    /// `audio` may be null, giving a silent recording; otherwise it must outlive the
    /// recorder. False means it could not start, with the reason logged.
    bool start(Options options, AudioTap* audio);

    /// True between a successful `start()` and `stop()`.
    [[nodiscard]] bool active() const { return running.load(std::memory_order_acquire); }

    /// What `start()` was given, filled in with any defaults it applied.
    [[nodiscard]] const Options& settings() const { return opt; }

    /**
     * @brief Render thread. How many copies of the next frame the file is currently short,
     *        with `elapsedSeconds` measured from the start of the recording.
     *
     * This *reserves* the frames, so it must be followed by a `submitFrame` carrying the
     * count it returned; otherwise a dropped submission silently shortens the video instead
     * of repaying itself.
     */
    uint32_t framesOwed(double elapsedSeconds);

    /**
     * @brief Render thread. Hand over one frame, to be written `repeat` times.
     *
     * Copies out of `pixels` and returns, so the caller may reuse the memory immediately.
     * Never blocks on the encoder: a full queue drops the frame and carries its `repeat`
     * onto the next one that fits, so a stall costs judder rather than a video running fast.
     */
    void submitFrame(const void* pixels, size_t bytes, uint32_t repeat);

    /// Close the pipes, wait for the encoders, then trim and mux. Blocking, and expensive
    /// on purpose: it runs at shutdown, where nobody is waiting on a frame. Returns the
    /// file written, or an empty path on failure.
    std::filesystem::path stop();

    /// Frames handed to the encoder, frames the queue could not take, and frames written to
    /// the pipe -- which counts repeats and so exceeds the first.
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
    /// Close both encoder pipes. Only after the worker is joined -- closing under a running
    /// worker leaves it writing to a closed `FILE*`.
    void closePipes();

    Options opt;
    AudioTap* tap = nullptr;

    /// One captured image plus how many times the file wants it. Held as a count rather
    /// than expanded into copies: the frames are megabytes each.
    struct QueuedFrame {
        std::vector<uint8_t> pixels;
        uint32_t repeat = 1;
    };
    /// Bounded, and guarded by an ordinary mutex rather than the lock-free discipline
    /// `AudioTap` needs -- the producer is the render thread, which already waits on fences
    /// and the presentation engine.
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
    /// Whether a session is still being fed. Cleared by `stop()` and by the worker itself
    /// when the encoder stops reading, which is why teardown cannot key on it.
    std::atomic<bool> running{false};
    /// Whether there is a thread to join and pipes to close. What `stop()` and the
    /// destructor test, because it does not go false on its own when the encoder dies.
    /// Plain rather than atomic: only the owning thread touches it.
    bool started = false;
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> written{0};

    /// Audio frames the tap reported lost, already paid for in silence.
    uint64_t silenceWritten = 0;
    /// Scratch for a drain, sized once. Worker thread only.
    std::vector<float> audioScratch;
};

/// True when ffmpeg is on PATH. Separate from `start()` so a caller can refuse the flag up
/// front rather than failing once the window is open.
[[nodiscard]] bool recordingToolsAvailable();

} // namespace core
