#pragma once

#include <cstddef>
#include <vector>

namespace core {

/**
 * @brief Fixed-capacity ring buffer that reports a rolling average.
 *
 * Keeps a running total so nextValue() is O(1) rather than O(capacity).
 */
template <typename T> class AveragingBuffer {
    std::vector<T> values;
    T totalValue = T{};
    size_t currIndex = 0;
    size_t filledCount = 0;

  public:
    explicit AveragingBuffer(size_t capacity) : values(capacity ? capacity : 1, T{}) {}

    /// Push a new sample, return the current rolling average.
    T nextValue(T v) {
        // if we haven't wrapped yet, inc the filled count
        if (filledCount < values.size()) {
            ++filledCount;
        } else {
            // subtract the value we're about to overwrite
            totalValue -= values[currIndex];
        }

        totalValue += v;
        values[currIndex] = v;
        currIndex = (currIndex + 1) % values.size();

        // average over however many slots are filled (until wrap, then capacity)
        return totalValue / static_cast<T>(filledCount);
    }

    [[nodiscard]] size_t size() const { return filledCount; }
};

} // namespace core
