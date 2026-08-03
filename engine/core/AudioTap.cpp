#include "core/AudioTap.h"

#include <algorithm>
#include <cstring>

namespace core {

void AudioTap::start(uint32_t channels, uint64_t capacityFrames_) {
    running.store(false, std::memory_order_release);

    channelCount = std::max(channels, 1u);
    // Two, so that the one slot held back below still leaves somewhere to put a frame.
    capacityFrames = std::max<uint64_t>(capacityFrames_, 2);

    samples.assign(static_cast<size_t>(capacityFrames) * channelCount, 0.0f);
    head.store(0, std::memory_order_relaxed);
    tail.store(0, std::memory_order_relaxed);
    droppedFrames.store(0, std::memory_order_relaxed);

    running.store(true, std::memory_order_release);
}

void AudioTap::stop() {
    running.store(false, std::memory_order_release);
    // The storage stays. `read()` tests `running` and then copies, and nothing makes those
    // one step -- so a reader that passed the test and was preempted here would have had the
    // buffer freed under it. `AudioEngine::shutdown` orders the recorder's join ahead of this
    // and so is safe, but `stopCapture()` is public and a caller that has not read that order
    // should not be able to cause a use-after-free with it.
    //
    // Not released until the destructor, where by construction there is no reader left. The
    // cost is one ring buffer -- a few seconds of float samples -- held until the tap dies,
    // which is a trade worth making for a hazard that is otherwise invisible.
    head.store(0, std::memory_order_relaxed);
    tail.store(0, std::memory_order_relaxed);
}

uint64_t AudioTap::write(const float* frames, uint64_t frameCount) {
    if (!running.load(std::memory_order_acquire) || frames == nullptr || frameCount == 0) return 0;

    const uint64_t writeIndex = head.load(std::memory_order_relaxed);
    const uint64_t readIndex = tail.load(std::memory_order_acquire);

    // One slot is left permanently empty so a full ring and an empty one are different
    // states rather than the same pair of indices.
    const uint64_t free = capacityFrames - (writeIndex - readIndex) - 1;
    const uint64_t take = std::min(frameCount, free);
    if (take < frameCount) {
        droppedFrames.fetch_add(frameCount - take, std::memory_order_relaxed);
    }
    if (take == 0) return 0;

    // Two copies at most: one to the end of the storage, one from the start of it. A
    // modulo per sample would be the obvious version and is a division in the audio
    // thread's inner loop.
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
