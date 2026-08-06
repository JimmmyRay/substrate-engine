#include "core/AudioTap.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace core;

/**
 * @file tests/AudioTapTests.cpp
 * @brief The mix copy handed from the audio thread to the recorder.
 *
 * Two properties are worth defending and only one of them is about correctness of a
 * single call. The first is ordering: what comes out is exactly what went in, in order,
 * with nothing duplicated. The second is that the *producer never blocks* -- it runs on
 * miniaudio's audio thread, which is filling a device period, and a stall there is
 * audible. That second one is what `scripts/test.sh tsan` is for; this file is in the hosted
 * sources precisely so it can run there.
 */

namespace {

/// A frame whose every sample encodes its own index, so a reader can tell not merely
/// that it got data but that it got the *right* data in the right order.
void fill(std::vector<float>& block, uint64_t firstFrame, uint32_t channels) {
    for (size_t f = 0; f * channels < block.size(); ++f) {
        for (uint32_t c = 0; c < channels; ++c) {
            block[f * channels + c] = static_cast<float>(firstFrame + f);
        }
    }
}

} // namespace

TEST(AudioTapTest, AnInactiveTapAcceptsAndReturnsNothing) {
    // The device runs whether or not anything is recording, so `write` is called on every
    // mix regardless. Doing nothing cheaply is the common case, not an edge case.
    AudioTap tap;
    EXPECT_FALSE(tap.active());

    const std::vector<float> block(64, 1.0f);
    EXPECT_EQ(tap.write(block.data(), 32), 0u);
    EXPECT_EQ(tap.pending(), 0u);

    std::vector<float> out(64, 0.0f);
    EXPECT_EQ(tap.read(out.data(), 32), 0u);
}

TEST(AudioTapTest, FramesComeOutInTheOrderTheyWentIn) {
    AudioTap tap;
    tap.start(2, 48000);
    ASSERT_TRUE(tap.active());
    EXPECT_EQ(tap.channels(), 2u);

    std::vector<float> in(256 * 2);
    fill(in, 0, 2);
    tap.write(in.data(), 256);
    EXPECT_EQ(tap.pending(), 256u);

    std::vector<float> out(256 * 2, -1.0f);
    ASSERT_EQ(tap.read(out.data(), 256), 256u);
    for (size_t f = 0; f < 256; ++f) {
        ASSERT_FLOAT_EQ(out[f * 2], static_cast<float>(f)) << "frame " << f;
        ASSERT_FLOAT_EQ(out[f * 2 + 1], static_cast<float>(f));
    }
    EXPECT_EQ(tap.pending(), 0u);
    EXPECT_EQ(tap.dropped(), 0u);
}

TEST(AudioTapTest, ReadsAndWritesWrapWithoutTearingAFrame) {
    // The storage is a ring, so a block that straddles the end is two memcpys. Getting
    // that split wrong interleaves the channels of one frame with another's, which is a
    // click rather than an error.
    AudioTap tap;
    tap.start(2, 1000);

    uint64_t written = 0;
    uint64_t verified = 0;
    std::vector<float> in(300 * 2);
    std::vector<float> out(300 * 2);

    // Ten rounds of 300 frames through a 1000-frame ring: every round but the first
    // starts at a different offset, so the split is exercised at several alignments.
    for (int round = 0; round < 10; ++round) {
        fill(in, written, 2);
        tap.write(in.data(), 300);
        written += 300;

        const uint64_t got = tap.read(out.data(), 300);
        ASSERT_EQ(got, 300u) << "round " << round;
        for (uint64_t f = 0; f < got; ++f) {
            ASSERT_FLOAT_EQ(out[f * 2], static_cast<float>(verified + f)) << "round " << round << " frame " << f;
            ASSERT_FLOAT_EQ(out[f * 2 + 1], static_cast<float>(verified + f));
        }
        verified += got;
    }
    EXPECT_EQ(tap.dropped(), 0u);
}

TEST(AudioTapTest, AStalledReaderLosesTheNewestFramesAndIsToldHowMany) {
    // The stated policy, and it is the opposite of what a live monitor would do. This
    // feeds a file: the audio has to stay in order and any gap has to be *countable*, so
    // whoever writes the stream can insert exactly that much silence and keep the sound
    // aligned with the picture.
    AudioTap tap;
    tap.start(1, 1000);

    std::vector<float> in(2000, 1.0f);
    fill(in, 0, 1);

    tap.write(in.data(), 2000);
    // One slot is deliberately never used, so a full ring and an empty one are
    // distinguishable states rather than one ambiguous pair of indices.
    EXPECT_EQ(tap.pending(), 999u);
    EXPECT_EQ(tap.dropped(), 1001u);

    // What survived is the *oldest* run, unbroken -- not a scattering of what fitted.
    std::vector<float> out(999, -1.0f);
    ASSERT_EQ(tap.read(out.data(), 999), 999u);
    for (size_t f = 0; f < out.size(); ++f) ASSERT_FLOAT_EQ(out[f], static_cast<float>(f));
}

TEST(AudioTapTest, RestartingEmptiesItAndForgetsTheDrops) {
    AudioTap tap;
    tap.start(1, 100);
    const std::vector<float> in(500, 1.0f);
    tap.write(in.data(), 500);
    ASSERT_GT(tap.dropped(), 0u);

    tap.start(2, 1000);
    EXPECT_EQ(tap.pending(), 0u);
    EXPECT_EQ(tap.dropped(), 0u);
    EXPECT_EQ(tap.channels(), 2u);
}

TEST(AudioTapTest, StoppingLeavesItInertRatherThanDangling) {
    AudioTap tap;
    tap.start(2, 24000);
    const std::vector<float> in(128, 0.5f);
    tap.write(in.data(), 64);

    tap.stop();
    EXPECT_FALSE(tap.active());

    // Every operation still has to be safe afterwards: `AudioEngine::shutdown` stops the
    // tap, and anything still holding a reference must not fall over.
    EXPECT_EQ(tap.write(in.data(), 64), 0u);
    std::vector<float> out(128, 0.0f);
    EXPECT_EQ(tap.read(out.data(), 64), 0u);
}

TEST(AudioTapTest, OneWriterAndOneReaderAgreeOnEveryFrame) {
    // The property the whole design exists for, and the one only a sanitizer can really
    // check. Run under `scripts/test.sh tsan` this is the test that says the two atomics are
    // enough and that no lock is hiding in the audio thread's path.
    AudioTap tap;
    tap.start(2, 4000);

    constexpr uint64_t kBlock = 128;
    // A whole number of blocks, so the writer stops on exactly the total rather than one
    // block past it and the reader's count can be compared for equality.
    constexpr uint64_t kTotalFrames = kBlock * 1560;
    std::atomic<bool> writerDone{false};

    std::thread writer([&] {
        std::vector<float> block(kBlock * 2);
        uint64_t frame = 0;
        while (frame < kTotalFrames) {
            fill(block, frame, 2);
            // Only ever offered once, and the next offer starts from what was accepted --
            // re-presenting a partially accepted block is what would duplicate frames.
            // This test is about *ordering*, so it waits for room rather than tolerating
            // a drop; the drop policy has its own test above.
            uint64_t sent = 0;
            while (sent < kBlock) {
                const uint64_t took = tap.write(block.data() + sent * 2, kBlock - sent);
                if (took == 0) {
                    std::this_thread::yield();
                    continue;
                }
                sent += took;
            }
            frame += kBlock;
        }
        writerDone.store(true, std::memory_order_release);
    });

    std::vector<float> out(kBlock * 2);
    uint64_t expected = 0;
    while (expected < kTotalFrames) {
        const uint64_t got = tap.read(out.data(), kBlock);
        if (got == 0) {
            if (writerDone.load(std::memory_order_acquire) && tap.pending() == 0) break;
            std::this_thread::yield();
            continue;
        }
        for (uint64_t f = 0; f < got; ++f) {
            ASSERT_FLOAT_EQ(out[f * 2], static_cast<float>(expected + f)) << "at frame " << expected + f;
        }
        expected += got;
    }

    writer.join();
    EXPECT_EQ(expected, kTotalFrames);
}
