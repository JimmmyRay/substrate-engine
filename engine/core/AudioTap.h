#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace core {

/**
 * @file engine/core/AudioTap.h
 * @brief A copy of the mix, handed from the audio thread to a recorder (S7).
 *
 * ## Why a ring and not a buffer with a lock
 *
 * The producer is miniaudio's audio thread, inside `onProcess` -- the callback fired at
 * the end of every `ma_engine_read_pcm_frames`. That thread has a deadline: it is filling
 * a device period, and anything it does that can block is a glitch you can hear. So
 * `write` takes no lock, allocates nothing, and never waits. A mutex here would be
 * inaudible in a test and audible in a room.
 *
 * The consumer is the recorder's worker thread. Exactly one of each, which is what makes
 * two atomics sufficient: the writer owns `head`, the reader owns `tail`, and neither
 * writes the other's index.
 *
 * ## What happens when the reader falls behind
 *
 * The **newest** frames are dropped and counted. That is the opposite of what a live
 * monitor would do -- it would drop the oldest to stay current -- and the difference is
 * the point: this feeds a file, and a file wants the audio in order, with any gap
 * *stated* rather than papered over. `dropped()` is how many frames never made it in, so
 * whoever writes the stream can insert exactly that much silence and keep the picture in
 * sync with the sound.
 *
 * Dropping at all is a stated policy rather than a hidden cap: at 48 kHz stereo the ring
 * costs 384 KiB per second of capacity, so a few seconds of it is enough that only a
 * genuinely stalled consumer ever loses anything -- and when one does, it says so.
 */
class AudioTap {
  public:
    /**
     * @brief Allocate `capacityFrames` of storage. A second call re-sizes and empties.
     *
     * Capacity is in *frames* rather than in seconds, and the caller converts. That is
     * not pedantry: the useful minimum is one device period, which this has no way to
     * know, and a `start()` that silently clamped what it was asked for would be a ring
     * whose size is not the size you gave it.
     *
     * Called before the device is started, because this is the one operation here that
     * allocates.
     */
    void start(uint32_t channels, uint64_t capacityFrames);

    /// Go inert: `write` and `read` both refuse from here on, and the ring is emptied. The
    /// storage itself is *not* released -- a reader can be between `read`'s check of
    /// `running` and its copy, and freeing under it would be a use-after-free that only
    /// shows up under load. It goes when the tap does.
    void stop();

    [[nodiscard]] bool active() const { return running.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t channels() const { return channelCount; }
    [[nodiscard]] uint64_t capacity() const { return capacityFrames; }

    /**
     * @brief Audio thread. Copy `frameCount` interleaved frames in.
     *
     * Never blocks, never allocates.
     *
     * @return how many frames were accepted. A short return means the reader has fallen
     *         behind; the remainder is dropped and counted, and is *not* held back for a
     *         later call -- a producer that re-presented it would duplicate whatever did
     *         fit.
     */
    uint64_t write(const float* frames, uint64_t frameCount);

    /**
     * @brief Recorder thread. Take up to `maxFrames` interleaved frames out.
     *
     * @return how many frames were actually written to `dst`.
     */
    [[nodiscard]] uint64_t read(float* dst, uint64_t maxFrames);

    /// Frames the writer could not fit since `start()`. A gap in the recording of exactly
    /// this length, which the caller is expected to replace with silence rather than
    /// splice out -- splicing would shorten the audio against the picture.
    [[nodiscard]] uint64_t dropped() const { return droppedFrames.load(std::memory_order_relaxed); }

    /// Frames currently waiting. For the overlay and for tests; the reader may see more by
    /// the time it acts on this, never fewer.
    [[nodiscard]] uint64_t pending() const;

  private:
    /// Interleaved samples, `capacityFrames * channelCount` of them. Sized once in
    /// `start()` and never touched again while the audio thread is running.
    std::vector<float> samples;
    uint64_t capacityFrames = 0;
    uint32_t channelCount = 0;

    /// Written by the audio thread, read by the recorder.
    std::atomic<uint64_t> head{0};
    /// Written by the recorder, read by the audio thread.
    std::atomic<uint64_t> tail{0};
    std::atomic<uint64_t> droppedFrames{0};
    std::atomic<bool> running{false};
};

} // namespace core
