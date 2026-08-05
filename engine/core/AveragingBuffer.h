#pragma once

#include <cstddef>
#include <vector>

namespace core {

/// @brief Fixed-capacity ring buffer reporting a rolling average over the last `capacity`
/// samples, in O(1).
template <typename T> class AveragingBuffer {
    std::vector<T> values;
    T totalValue = T{};
    size_t currIndex = 0;
    size_t filledCount = 0;

  public:
    explicit AveragingBuffer(size_t capacity) : values(capacity ? capacity : 1, T{}) {}

    /// Push a new sample, return the current rolling average.
    T nextValue(T v) {
        if (filledCount < values.size()) {
            ++filledCount;
        } else {
            totalValue -= values[currIndex];
        }

        totalValue += v;
        values[currIndex] = v;
        currIndex = (currIndex + 1) % values.size();

        return totalValue / static_cast<T>(filledCount);
    }

    [[nodiscard]] size_t size() const { return filledCount; }
};

} // namespace core
