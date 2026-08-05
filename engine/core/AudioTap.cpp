#include "core/AudioTap.h"

#include <algorithm>
#include <cstring>

namespace core {

void AudioTap::start(uint32_t channels, uint64_t capacityFrames_) {
    running.store(false, std::memory_order_release);

    channelCount = std::max(channels, 1u);
    // Floor of two: one slot is held back to tell full from empty, so a capacity of one
    // would never accept a frame.
    capacityFrames = std::max<uint64_t>(capacityFrames_, 2);

    samples.assign(static_cast<size_t>(capacityFrames) * channelCount, 0.0f);
    head.store(0, std::memory_order_relaxed);
    tail.store(0, std::memory_order_relaxed);
    droppedFrames.store(0, std::memory_order_relaxed);

    running.store(true, std::memory_order_release);
}

void AudioTap::stop() {
    running.store(false, std::memory_order_release);
    // Releasing `samples` here is a use-after-free: `read()` tests `running` and then
    // copies, and nothing makes those one step, so a reader preempted between them keeps a
    // pointer into freed storage. It goes in the destructor, where no reader is left.
    head.store(0, std::memory_order_relaxed);
    tail.store(0, std::memory_order_relaxed);
}

uint64_t AudioTap::write(const float* frames, uint64_t frameCount) {
    if (!running.load(std::memory_order_acquire) || frames == nullptr || frameCount == 0) return 0;

    const uint64_t writeIndex = head.load(std::memory_order_relaxed);
    const uint64_t readIndex = tail.load(std::memory_order_acquire);

    // The `- 1` leaves one slot permanently empty; without it a full ring and an empty one
    // are the same pair of indices.
    const uint64_t free = capacityFrames - (writeIndex - readIndex) - 1;
    const uint64_t take = std::min(frameCount, free);
    if (take < frameCount) {
        droppedFrames.fetch_add(frameCount - take, std::memory_order_relaxed);
    }
    if (take == 0) return 0;

    // Split into at most two copies rather than wrapping per sample: a modulo in here is a
    // division in the audio thread's inner loop.
    const uint64_t offset = writeIndex % capacityFrames;
    const uint64_t firstFrames = std::min(take, capacityFrames - offset);

    std::memcpy(&samples[static_cast<size_t>(offset) * channelCount], frames,
                static_cast<size_t>(firstFrames) * channelCount * sizeof(float));
    if (take > firstFrames) {
        std::memcpy(samples.data(), frames + static_cast<size_t>(firstFrames) * channelCount,
                    static_cast<size_t>(take - firstFrames) * channelCount * sizeof(float));
    }

    // Released last, so a reader that sees the new index also sees the samples behind it.
    head.store(writeIndex + take, std::memory_order_release);
    return take;
}

uint64_t AudioTap::read(float* dst, uint64_t maxFrames) {
    if (!running.load(std::memory_order_acquire) || dst == nullptr || maxFrames == 0) return 0;

    const uint64_t readIndex = tail.load(std::memory_order_relaxed);
    const uint64_t writeIndex = head.load(std::memory_order_acquire);

    const uint64_t take = std::min(maxFrames, writeIndex - readIndex);
    if (take == 0) return 0;

    const uint64_t offset = readIndex % capacityFrames;
    const uint64_t firstFrames = std::min(take, capacityFrames - offset);

    std::memcpy(dst, &samples[static_cast<size_t>(offset) * channelCount],
                static_cast<size_t>(firstFrames) * channelCount * sizeof(float));
    if (take > firstFrames) {
        std::memcpy(dst + static_cast<size_t>(firstFrames) * channelCount, samples.data(),
                    static_cast<size_t>(take - firstFrames) * channelCount * sizeof(float));
    }

    tail.store(readIndex + take, std::memory_order_release);
    return take;
}

uint64_t AudioTap::pending() const {
    return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire);
}

} // namespace core
