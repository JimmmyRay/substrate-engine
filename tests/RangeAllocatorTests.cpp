#include "core/RangeAllocator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

using namespace core;

/**
 * @file tests/RangeAllocatorTests.cpp
 * @brief Sub-allocation, coalescing and growth.
 *
 * The allocator is the part of streaming that is easy to get wrong and impossible to
 * observe: a free list that fails to coalesce still *works*, and only stops working after
 * a few hundred load/unload cycles have shredded the buffer -- by which point the symptom
 * is "streaming stopped working in long sessions" and the cause is twenty minutes of
 * arithmetic somewhere else.
 *
 * So the load-bearing tests here are the ones that would pass on a broken implementation
 * if they were written the obvious way: the coalescing tests assert `holeCount`, and the
 * randomised one asserts that allocated ranges **never overlap**, which is the only
 * property that actually matters and the only one a bug reliably breaks.
 */
namespace {

struct Range {
    uint32_t first;
    uint32_t count;
};

/// The property no allocator may ever violate. Checked by sorting, because two ranges
/// overlap if and only if some range starts before its predecessor ends.
void expectNoOverlap(std::vector<Range> live, uint32_t capacity) {
    std::sort(live.begin(), live.end(), [](const Range& a, const Range& b) { return a.first < b.first; });
    for (size_t i = 0; i < live.size(); ++i) {
        EXPECT_LE(live[i].first + live[i].count, capacity) << "range " << i << " runs past the buffer";
        if (i == 0) continue;
        EXPECT_GE(live[i].first, live[i - 1].first + live[i - 1].count)
            << "range " << i << " overlaps the one before it";
    }
}

} // namespace

TEST(RangeAllocator, AFreshAllocatorIsOneHole) {
    const RangeAllocator a(1000);
    EXPECT_EQ(a.capacity(), 1000u);
    EXPECT_EQ(a.used(), 0u);
    EXPECT_EQ(a.holeCount(), 1u);
    EXPECT_EQ(a.largestFree(), 1000u);
}

TEST(RangeAllocator, AllocationsAreContiguousAndInOrder) {
    RangeAllocator a(100);
    EXPECT_EQ(a.allocate(10), 0u);
    EXPECT_EQ(a.allocate(20), 10u);
    EXPECT_EQ(a.allocate(5), 30u);
    EXPECT_EQ(a.used(), 35u);
    EXPECT_EQ(a.holeCount(), 1u);
}

TEST(RangeAllocator, AFullBufferRefusesRatherThanWrapping) {
    RangeAllocator a(100);
    EXPECT_EQ(a.allocate(100), 0u);
    EXPECT_EQ(a.holeCount(), 0u);
    EXPECT_EQ(a.largestFree(), 0u);
    // Not zero, which is a perfectly good offset -- an allocator whose failure value is a
    // valid result is one whose callers stop checking.
    EXPECT_EQ(a.allocate(1), RangeAllocator::kNoRange);
}

TEST(RangeAllocator, AnOversizedRequestFailsWithoutDisturbingAnything) {
    RangeAllocator a(100);
    (void)a.allocate(40);
    EXPECT_EQ(a.allocate(1000), RangeAllocator::kNoRange);
    EXPECT_EQ(a.used(), 40u);
    EXPECT_EQ(a.largestFree(), 60u);
}

TEST(RangeAllocator, AZeroLengthRequestSucceedsAndCostsNothing) {
    // A primitive with no indices is a real thing a glTF contains.
    RangeAllocator a(100);
    EXPECT_NE(a.allocate(0), RangeAllocator::kNoRange);
    EXPECT_EQ(a.used(), 0u);
}

// ==================================================================== coalescing

TEST(RangeAllocator, FreeingBetweenTwoHolesMergesAllThree) {
    // The test that fails on an allocator which frees correctly but never merges. Without
    // the merge this leaves three holes and `largestFree` stays at 10.
    RangeAllocator a(100);
    const uint32_t x = a.allocate(10);
    const uint32_t y = a.allocate(10);
    const uint32_t z = a.allocate(10);
    (void)a.allocate(70); // fill the tail, so the only free space is what we release

    a.free(x, 10);
    a.free(z, 10);
    EXPECT_EQ(a.holeCount(), 2u);

    a.free(y, 10);
    EXPECT_EQ(a.holeCount(), 1u) << "the middle range should have fused its two neighbours";
    EXPECT_EQ(a.largestFree(), 30u);
}

TEST(RangeAllocator, FreeingMergesLeftAndRightIndependently) {
    RangeAllocator a(100);
    const uint32_t x = a.allocate(10);
    const uint32_t y = a.allocate(10);
    (void)a.allocate(80);

    a.free(x, 10);
    EXPECT_EQ(a.holeCount(), 1u);
    a.free(y, 10);
    EXPECT_EQ(a.holeCount(), 1u);
    EXPECT_EQ(a.largestFree(), 20u);
}

TEST(RangeAllocator, AReleasedRangeIsHandedOutAgain) {
    RangeAllocator a(100);
    (void)a.allocate(30);
    const uint32_t middle = a.allocate(30);
    (void)a.allocate(40);
    ASSERT_EQ(a.used(), 100u);

    a.free(middle, 30);
    EXPECT_EQ(a.allocate(30), middle) << "the hole is where the model that left used to be";
    EXPECT_EQ(a.used(), 100u);
}

TEST(RangeAllocator, LoadAndUnloadDoesNotShredTheBuffer) {
    // The failure this allocator exists to avoid, run long enough to show. Without
    // coalescing the hole count climbs with every cycle and the last allocation fails.
    RangeAllocator a(1000);
    for (int cycle = 0; cycle < 500; ++cycle) {
        const uint32_t p = a.allocate(400);
        const uint32_t q = a.allocate(400);
        ASSERT_NE(p, RangeAllocator::kNoRange) << "cycle " << cycle;
        ASSERT_NE(q, RangeAllocator::kNoRange) << "cycle " << cycle;
        a.free(p, 400);
        a.free(q, 400);
        ASSERT_EQ(a.holeCount(), 1u) << "cycle " << cycle << " left the buffer in pieces";
    }
    EXPECT_EQ(a.used(), 0u);
    EXPECT_EQ(a.largestFree(), 1000u);
}

TEST(RangeAllocator, FragmentationIsVisibleAndDistinctFromFullness) {
    // The two states a caller has to tell apart: plenty free but nowhere to put anything,
    // versus genuinely full. They need different responses -- grow, and compact.
    RangeAllocator a(100);
    std::vector<uint32_t> odd;
    for (int i = 0; i < 10; ++i) {
        const uint32_t r = a.allocate(10);
        if (i % 2 == 1) odd.push_back(r);
    }
    for (const uint32_t r : odd) a.free(r, 10);

    EXPECT_EQ(a.used(), 50u);
    EXPECT_EQ(a.capacity() - a.used(), 50u);
    EXPECT_EQ(a.largestFree(), 10u) << "half the buffer is free and the biggest hole is a tenth";
    EXPECT_EQ(a.allocate(20), RangeAllocator::kNoRange);
}

// ==================================================================== growth

TEST(RangeAllocator, GrowthAddsSpaceAtTheEnd) {
    RangeAllocator a(100);
    (void)a.allocate(100);
    ASSERT_EQ(a.allocate(50), RangeAllocator::kNoRange);

    EXPECT_TRUE(a.grow(200));
    EXPECT_EQ(a.capacity(), 200u);
    EXPECT_EQ(a.allocate(50), 100u);
}

TEST(RangeAllocator, GrowthMergesOntoAFreeTail) {
    // Two growths in a row must leave one hole, or a large model cannot land after
    // incremental growth even though the space exists.
    RangeAllocator a(100);
    (void)a.allocate(50);
    EXPECT_TRUE(a.grow(150));
    EXPECT_TRUE(a.grow(200));
    EXPECT_EQ(a.holeCount(), 1u);
    EXPECT_EQ(a.largestFree(), 150u);
    EXPECT_EQ(a.allocate(150), 50u);
}

TEST(RangeAllocator, ShrinkingIsRefused) {
    // It would invalidate live ranges, and every real case for it is compaction wearing
    // the wrong name.
    RangeAllocator a(100);
    EXPECT_FALSE(a.grow(100));
    EXPECT_FALSE(a.grow(50));
    EXPECT_EQ(a.capacity(), 100u);
}

TEST(RangeAllocator, ResetDiscardsEverything) {
    RangeAllocator a(100);
    (void)a.allocate(60);
    a.reset(500);
    EXPECT_EQ(a.capacity(), 500u);
    EXPECT_EQ(a.used(), 0u);
    EXPECT_EQ(a.holeCount(), 1u);
}

TEST(RangeAllocator, AZeroCapacityAllocatorHasNoHoles) {
    RangeAllocator a(0);
    EXPECT_EQ(a.holeCount(), 0u);
    EXPECT_EQ(a.allocate(1), RangeAllocator::kNoRange);
    EXPECT_TRUE(a.grow(10));
    EXPECT_EQ(a.allocate(10), 0u);
}

// ==================================================================== refusals

TEST(RangeAllocator, FreeingOutsideTheBufferIsIgnored) {
    // Inserting it would corrupt the free list into handing out an offset past the end --
    // a GPU fault a very long way from its cause.
    RangeAllocator a(100);
    (void)a.allocate(100);
    a.free(200, 10);
    a.free(95, 50);
    EXPECT_EQ(a.used(), 100u);
    EXPECT_EQ(a.holeCount(), 0u);
}

TEST(RangeAllocator, FreeingNothingIsANoOp) {
    RangeAllocator a(100);
    const uint32_t r = a.allocate(10);
    a.free(r, 0);
    EXPECT_EQ(a.used(), 10u);
}

// ==================================================================== the real property

TEST(RangeAllocator, RandomTrafficNeverHandsOutOverlappingRanges) {
    // Everything above is a hand-picked case. This is the property itself: whatever the
    // sequence, two live ranges must never share an element. A coalescing bug that merges
    // one element too many shows up here and nowhere else.
    std::mt19937 rng(12345);
    std::uniform_int_distribution<uint32_t> sizeDist(1, 60);
    std::uniform_int_distribution<int> actionDist(0, 2);

    RangeAllocator a(2000);
    std::vector<Range> live;

    for (int step = 0; step < 20000; ++step) {
        const bool freeing = !live.empty() && actionDist(rng) == 0;
        if (freeing) {
            std::uniform_int_distribution<size_t> pick(0, live.size() - 1);
            const size_t i = pick(rng);
            a.free(live[i].first, live[i].count);
            live.erase(live.begin() + static_cast<long>(i));
        } else {
            const uint32_t n = sizeDist(rng);
            const uint32_t r = a.allocate(n);
            if (r != RangeAllocator::kNoRange) live.push_back({r, n});
        }

        uint32_t sum = 0;
        for (const Range& g : live) sum += g.count;
        ASSERT_EQ(a.used(), sum) << "step " << step << ": the allocator and the caller disagree";
    }

    expectNoOverlap(live, a.capacity());

    // And it all comes back.
    for (const Range& g : live) a.free(g.first, g.count);
    EXPECT_EQ(a.used(), 0u);
    EXPECT_EQ(a.holeCount(), 1u) << "everything returned should leave exactly one hole";
}
