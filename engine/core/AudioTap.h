#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace core {

/**
 * @file engine/core/AudioTap.h
 * @brief A copy of the mix, handed from the audio thread to a recorder.
 *
 * Single producer (miniaudio's audio thread, in `onProcess`) and single consumer (the
 * recorder's worker). Two atomics are only sufficient under that: the writer owns `head`,
 * the reader owns `tail`, and a second thread on either end corrupts the ring. Anything in
 * `write` that can block or allocate is an audible glitch, not a slow test.
 */
class AudioTap {
  public:
    /**
     * @brief Allocate storage for `capacityFrames` interleaved frames; a second call
     * re-sizes and empties.
     *
     * The only allocating operation here, so calling it once the device is running puts a
     * `vector` resize on the audio thread's deadline.
     */
    void start(uint32_t channels, uint64_t capacityFrames);

    /// Go inert and empty the ring. Storage is held until the destructor; see the
    /// use-after-free noted in `AudioTap::stop`.
    void stop();

    [[nodiscard]] bool active() const { return running.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t channels() const { return channelCount; }
    [[nodiscard]] uint64_t capacity() const { return capacityFrames; }

    /**
     * @brief Audio thread only. Copy `frameCount` interleaved frames in.
     *
     * @return frames accepted. A short return has already dropped and counted the
     *         remainder; re-presenting it on the next call duplicates whatever did fit.
     */
    uint64_t write(const float* frames, uint64_t frameCount);

    /// Recorder thread only. Takes up to `maxFrames` interleaved frames out, returning how
    /// many reached `dst`.
    [[nodiscard]] uint64_t read(float* dst, uint64_t maxFrames);

    /// Frames the writer could not fit since `start()`. Replace them with that much
    /// silence; splicing the gap out instead shortens the audio against the picture.
    [[nodiscard]] uint64_t dropped() const { return droppedFrames.load(std::memory_order_relaxed); }

    /// Frames waiting. A lower bound by the time the reader acts on it, never an upper one.
    [[nodiscard]] uint64_t pending() const;

  private:
    /// Interleaved samples, `capacityFrames * channelCount` of them. Re-sizing this while
    /// the audio thread runs reallocates under `write`.
    std::vector<float> samples;
    uint64_t capacityFrames = 0;
    uint32_t channelCount = 0;

    /// Written by the audio thread only.
    std::atomic<uint64_t> head{0};
    /// Written by the recorder only.
    std::atomic<uint64_t> tail{0};
    std::atomic<uint64_t> droppedFrames{0};
    std::atomic<bool> running{false};
};

} // namespace core
