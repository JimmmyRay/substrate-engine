#include "core/RangeAllocator.h"

#include <algorithm>

namespace core {

void RangeAllocator::reset(uint32_t capacity) {
    total = capacity;
    holes.clear();
    if (capacity > 0) holes.push_back({0, capacity});
}

uint32_t RangeAllocator::allocate(uint32_t count) {
    // A glTF really does contain primitives with no indices; refusing them pushes the check
    // onto every caller. The offset returned is where the range would have started.
    if (count == 0) return holes.empty() ? 0u : holes.front().first;

    for (size_t i = 0; i < holes.size(); ++i) {
        if (holes[i].count < count) continue;

        const uint32_t first = holes[i].first;
        if (holes[i].count == count) {
            holes.erase(holes.begin() + static_cast<long>(i));
        } else {
            holes[i].first += count;
            holes[i].count -= count;
        }
        return first;
    }
    return kNoRange;
}

void RangeAllocator::free(uint32_t first, uint32_t count) {
    if (count == 0) return;
    // A range outside the buffer is refused, never inserted: it corrupts the free list in a
    // way that surfaces as a later allocation handing back an offset past the end, which is
    // a GPU fault a long way from here.
    if (first >= total || count > total - first) return;

    // Insertion point: the first hole starting after this range.
    const auto at = std::lower_bound(holes.begin(), holes.end(), first,
                                     [](const Hole& h, uint32_t value) { return h.first < value; });
    const auto index = static_cast<size_t>(at - holes.begin());
    holes.insert(at, Hole{first, count});

    // Right first, then left: merging left can move this entry's index, and the right-hand
    // neighbour's position is only stable until it does.
    if (index + 1 < holes.size() && holes[index].first + holes[index].count == holes[index + 1].first) {
        holes[index].count += holes[index + 1].count;
        holes.erase(holes.begin() + static_cast<long>(index) + 1);
    }
    if (index > 0 && holes[index - 1].first + holes[index - 1].count == holes[index].first) {
        holes[index - 1].count += holes[index].count;
        holes.erase(holes.begin() + static_cast<long>(index));
    }
}

bool RangeAllocator::grow(uint32_t newCapacity) {
    if (newCapacity <= total) return false;

    const uint32_t added = newCapacity - total;
    // Merged onto the tail when the buffer already ends in free space, or growing twice in
    // a row leaves two holes and a large allocation fails after incremental growth.
    if (!holes.empty() && holes.back().first + holes.back().count == total) {
        holes.back().count += added;
    } else {
        holes.push_back({total, added});
    }
    total = newCapacity;
    return true;
}

uint32_t RangeAllocator::freeElements() const {
    uint32_t sum = 0;
    for (const Hole& h : holes) sum += h.count;
    return sum;
}

uint32_t RangeAllocator::largestFree() const {
    uint32_t best = 0;
    for (const Hole& h : holes) best = std::max(best, h.count);
    return best;
}

} // namespace core
