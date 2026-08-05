#pragma once

#include <cstdint>
#include <vector>

/**
 * @file engine/core/RangeAllocator.h
 * @brief Sub-allocation inside one buffer, with coalescing and growth.
 *
 * Integers only, no Vulkan, so the part that is easy to get wrong is testable without a
 * device.
 *
 * A free list rather than compaction, because the offsets it hands out are live on the GPU
 * -- baked into every `VkDrawIndexedIndirectCommand`, every BLAS and every primitive
 * record. Moving a range means rewriting all of them and re-cooking the acceleration
 * structures. The price is fragmentation, which is why `largestFree` and `used` are on the
 * surface: a caller that cannot fit a model has to tell "full" from "shredded".
 *
 * Units are **elements, not bytes**, so `baseVertex` and `firstIndex` come straight out.
 * Converting to bytes at the boundary is how an off-by-stride bug gets written.
 */
namespace core {

/// @brief A first-fit free-list allocator over `[0, capacity)`.
///
/// Not thread-safe: the load path is one thread at a time, and a second one needs external
/// synchronisation.
class RangeAllocator {
  public:
    /// Returned by `allocate` when the request does not fit. Must not be zero -- zero is a
    /// perfectly good offset.
    static constexpr uint32_t kNoRange = 0xFFFFFFFFu;

    RangeAllocator() = default;
    explicit RangeAllocator(uint32_t capacity) { reset(capacity); }

    /// Discard everything and start again at `capacity` elements, all free.
    void reset(uint32_t capacity);

    /**
     * @brief First fit.
     *
     * @return the first element of the range, or `kNoRange`. A zero-element request
     *         succeeds and consumes nothing: a glTF really does contain primitives with no
     *         indices, and refusing them pushes the check onto every caller.
     */
    [[nodiscard]] uint32_t allocate(uint32_t count);

    /**
     * @brief Give a range back, merging it with any free neighbours.
     *
     * Coalescing happens here rather than on a later sweep, or loading and unloading the
     * same model leaves the buffer in more pieces every cycle. Freeing a zero-element range
     * is a no-op.
     *
     * The allocator keeps no record of what it handed out, so a double free or a free of a
     * range never allocated goes undetected. A range outside the buffer is ignored.
     */
    void free(uint32_t first, uint32_t count);

    /**
     * @brief Extend the buffer to `newCapacity` elements, the new space merged onto the
     *        tail.
     *
     * Shrinking is refused rather than ignored: it would invalidate live ranges.
     *
     * @return false when `newCapacity` is not larger than the current capacity.
     */
    bool grow(uint32_t newCapacity);

    [[nodiscard]] uint32_t capacity() const { return total; }
    /// Elements handed out and not yet returned.
    [[nodiscard]] uint32_t used() const { return total - freeElements(); }
    [[nodiscard]] uint32_t freeElements() const;
    /// The biggest single allocation that would currently succeed. The difference between
    /// this and `capacity() - used()` *is* the fragmentation.
    [[nodiscard]] uint32_t largestFree() const;
    /// How many separate holes the free space is in. One means unfragmented; zero means
    /// full.
    [[nodiscard]] uint32_t holeCount() const { return static_cast<uint32_t>(holes.size()); }

    struct Hole {
        uint32_t first = 0;
        uint32_t count = 0;
    };
    [[nodiscard]] const std::vector<Hole>& freeList() const { return holes; }

  private:
    /// Must stay sorted by `first` and never adjacent: coalescing only looks at neighbours,
    /// and `largestFree` and `allocate` both assume it.
    std::vector<Hole> holes;
    uint32_t total = 0;
};

} // namespace core
