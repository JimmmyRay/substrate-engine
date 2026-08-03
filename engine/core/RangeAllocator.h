#pragma once

#include <cstdint>
#include <vector>

/**
 * @file engine/core/RangeAllocator.h
 * @brief Sub-allocation inside one buffer, with coalescing and growth (C10).
 *
 * ## What this is for
 *
 * The scene owns one vertex buffer and one index buffer, sized at load and never touched
 * again. That is why nothing can be unloaded: there is no notion of a *range* belonging to
 * a model, so there is nothing to give back and nowhere to put the next model's geometry.
 * This is that notion, and it is deliberately the whole of it -- **integers, no Vulkan**,
 * so the part that is easy to get wrong is testable without a device.
 *
 * ## Why a free list and not a bump allocator with compaction
 *
 * Compaction is the other answer: keep everything dense, and when a model is unloaded,
 * slide the survivors down. It is rejected because the offsets are *live on the GPU* --
 * they are baked into every `VkDrawIndexedIndirectCommand`, every BLAS, and every
 * primitive record. Moving one range means rewriting all of them and re-cooking the
 * acceleration structures, which is a stall proportional to the whole scene at exactly the
 * moment a game is trying to stream without one.
 *
 * A free list keeps every surviving offset exactly where it is. The price is fragmentation,
 * which is why `largestFree` and `used` are part of the surface rather than diagnostics: a
 * caller that cannot fit a model needs to be able to tell "the buffer is full" from "the
 * buffer is shredded", because the answers are grow and compact-offline respectively.
 *
 * ## Units
 *
 * Elements, not bytes. A vertex allocator counts vertices and an index allocator counts
 * indices, so `baseVertex` and `firstIndex` come straight out of it -- which is what those
 * fields of an indirect draw already are, and converting to bytes at the boundary is how
 * an off-by-stride bug gets written.
 */
namespace core {

/**
 * @brief A first-fit free-list allocator over `[0, capacity)`.
 *
 * Not thread-safe, and deliberately not: the load path is one thread at a time by
 * construction, and a lock here would be a lock taken on every allocation to serve a
 * caller that does not exist.
 */
class RangeAllocator {
  public:
    /// Returned by `allocate` when the request does not fit. Not zero -- zero is a
    /// perfectly good offset, and an allocator whose failure value is a valid result is one
    /// whose callers eventually stop checking.
    static constexpr uint32_t kNoRange = 0xFFFFFFFFu;

    RangeAllocator() = default;
    explicit RangeAllocator(uint32_t capacity) { reset(capacity); }

    /// Discard everything and start again at `capacity` elements, all free.
    void reset(uint32_t capacity);

    /**
     * @brief First fit.
     *
     * First fit rather than best fit, and the reason is that best fit is only better when
     * the sizes are varied and small. These are mesh-sized: a handful of allocations per
     * model, each large. Best fit would walk the whole list to save a split that first fit
     * makes at the front, and the fragmentation difference between them at this scale is
     * not measurable.
     *
     * @return the first element of the range, or `kNoRange`. A zero-element request
     *         succeeds and returns a usable offset without consuming anything, because a
     *         primitive with no indices is a real thing a glTF contains and refusing it
     *         would push the check onto every caller.
     */
    [[nodiscard]] uint32_t allocate(uint32_t count);

    /**
     * @brief Give a range back, merging it with any free neighbours.
     *
     * Coalescing on free rather than on a later sweep: without it, loading and unloading
     * the same model repeatedly leaves the buffer in more pieces every cycle, and a
     * streaming world does exactly that. Freeing a zero-element range is a no-op.
     *
     * Double-freeing or freeing a range that was never allocated is a programming error the
     * allocator does not detect -- it has no record of what it handed out, by design. What
     * it does detect is a range outside the buffer, which it ignores.
     */
    void free(uint32_t first, uint32_t count);

    /**
     * @brief Extend the buffer to `newCapacity` elements.
     *
     * The new space arrives free and merged onto the tail, so growth followed by a single
     * large allocation succeeds even when every existing hole is too small. Shrinking is
     * refused rather than silently ignored: it would invalidate live ranges, and there is
     * no case for it that is not really compaction.
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

    /// A free hole, for a caller that wants to reason about layout -- a defragmenter, or a
    /// diagnostic that prints the map.
    struct Hole {
        uint32_t first = 0;
        uint32_t count = 0;
    };
    [[nodiscard]] const std::vector<Hole>& freeList() const { return holes; }

  private:
    /// Kept sorted by `first` and never adjacent, which is what makes coalescing a
    /// question about neighbours rather than a search.
    std::vector<Hole> holes;
    uint32_t total = 0;
};

} // namespace core
