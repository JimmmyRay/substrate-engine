#include "core/AveragingBuffer.h"

#include <gtest/gtest.h>

using namespace core;

/**
 * @file tests/AveragingBufferTests.cpp
 * @brief The profiler's rolling window (5.1).
 *
 * Small enough to look obviously right, which is exactly why it is worth testing: the
 * running total is an optimisation, and a running total that drifts from the true mean
 * makes every averaged number in `Profiler::dump()` quietly wrong rather than visibly
 * broken.
 */

TEST(AveragingBuffer, AveragesBeforeItWraps) {
    AveragingBuffer<double> buf(4);

    EXPECT_DOUBLE_EQ(buf.nextValue(10.0), 10.0);
    EXPECT_DOUBLE_EQ(buf.nextValue(20.0), 15.0);
    EXPECT_DOUBLE_EQ(buf.nextValue(30.0), 20.0);
    EXPECT_EQ(buf.size(), 3u);
}

TEST(AveragingBuffer, DropsTheOldestSampleOnWrap) {
    AveragingBuffer<double> buf(3);
    buf.nextValue(1.0);
    buf.nextValue(2.0);
    buf.nextValue(3.0);

    // The 1.0 falls out: mean of {2, 3, 4}.
    EXPECT_DOUBLE_EQ(buf.nextValue(4.0), 3.0);
    // And the 2.0: mean of {3, 4, 5}.
    EXPECT_DOUBLE_EQ(buf.nextValue(5.0), 4.0);
    EXPECT_EQ(buf.size(), 3u);
}

TEST(AveragingBuffer, RunningTotalDoesNotDriftOverManyWraps) {
    // The whole point of the running total is that it is O(1) rather than O(capacity).
    // The risk it carries is accumulated floating-point error, so drive it far past the
    // window and compare against the exact answer.
    AveragingBuffer<double> buf(64);
    double last = 0.0;
    for (int i = 0; i < 10000; ++i) last = buf.nextValue(i % 2 == 0 ? 1.0 : 3.0);

    EXPECT_NEAR(last, 2.0, 1e-9);
}

TEST(AveragingBuffer, ZeroCapacityIsClampedRatherThanDividingByZero) {
    AveragingBuffer<double> buf(0);
    EXPECT_DOUBLE_EQ(buf.nextValue(7.0), 7.0);
    EXPECT_DOUBLE_EQ(buf.nextValue(9.0), 9.0);
    EXPECT_EQ(buf.size(), 1u);
}
