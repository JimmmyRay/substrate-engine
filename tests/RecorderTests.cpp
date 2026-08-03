#include "core/Recorder.h"

#include <gtest/gtest.h>

using namespace core;

/**
 * @file tests/RecorderTests.cpp
 * @brief The pacing rule that keeps a recording the length of the session (S7).
 *
 * What is tested here is the arithmetic, not the encoder. `start()` spawns ffmpeg and
 * writes a file, which makes it an integration test of the machine rather than of this
 * code; the property that can go quietly wrong is the other one. A recording whose clock
 * drifts is not a recording that fails, it is one where the sound slides away from the
 * picture over minutes -- and it does that whether or not ffmpeg is installed.
 */

TEST(RecorderPacing, NothingIsOwedBeforeTheFirstFrameIsDue) {
    // 30 fps owes its first frame a thirty-third of a second in, not immediately.
    EXPECT_EQ(framesOwedAt(0.0, 30, 0), 0u);
    EXPECT_EQ(framesOwedAt(0.02, 30, 0), 0u);
    EXPECT_EQ(framesOwedAt(0.034, 30, 0), 1u);
}

TEST(RecorderPacing, AGameFasterThanTheFileOwesMostFramesNothing) {
    // The case that makes recording cheap: at 250 fps roughly one frame in eight is
    // wanted, and the rest cost no readback, no copy and no encode at all.
    uint64_t delivered = 0;
    uint32_t asked = 0;
    constexpr int kGameFrames = 250;
    for (int i = 1; i <= kGameFrames; ++i) {
        const uint32_t owed = framesOwedAt(static_cast<double>(i) / kGameFrames, 30, delivered);
        if (owed > 0) ++asked;
        delivered += owed;
    }
    EXPECT_EQ(delivered, 30u);
    // Every one of them handed over singly. A game running faster than the file never
    // needs a repeat, and asking for one would mean the pacing had lost count.
    EXPECT_EQ(asked, 30u);
}

TEST(RecorderPacing, AGameSlowerThanTheFileRepeatsFramesToFillIt) {
    // 10 fps into a 30 fps file. Without the repeat the video would play at three times
    // speed, which is the failure this exists to prevent.
    uint64_t delivered = 0;
    for (int i = 1; i <= 10; ++i) {
        delivered += framesOwedAt(static_cast<double>(i) / 10.0, 30, delivered);
    }
    EXPECT_EQ(delivered, 30u);
}

TEST(RecorderPacing, ASecondOfElapsedTimeIsAlwaysASecondOfFile) {
    // The property the whole rule exists for, over an uneven frame rate rather than a
    // steady one: whatever the game does, ten seconds of session is 300 frames of a
    // 30 fps file. Accumulating per-frame deltas instead would drift here.
    uint64_t delivered = 0;
    double now = 0.0;
    // A deliberately irregular cadence -- 4 ms, then 9, then 21, repeating.
    const double cadence[] = {0.004, 0.009, 0.021};
    int i = 0;
    while (now < 10.0) {
        now += cadence[i++ % 3];
        delivered += framesOwedAt(now, 30, delivered);
    }
    EXPECT_NEAR(static_cast<double>(delivered), now * 30.0, 1.0);
}

TEST(RecorderPacing, ALongHitchIsCappedRatherThanFrozenIntoTheFile) {
    // Two seconds of nothing at 30 fps is sixty frames owed. Writing all sixty would put
    // two seconds of one still image in the recording; the cap turns that into a jump.
    EXPECT_EQ(framesOwedAt(2.0, 30, 0), kMaxFrameRepeat);
}

TEST(RecorderPacing, AnUpToDateFileIsOwedNothing) {
    EXPECT_EQ(framesOwedAt(1.0, 30, 30), 0u);
    EXPECT_EQ(framesOwedAt(1.0, 30, 40), 0u);
}

TEST(RecorderPacing, AZeroFrameRateAsksForNothingRatherThanDividingByIt) {
    EXPECT_EQ(framesOwedAt(5.0, 0, 0), 0u);
}

TEST(RecorderTest, AnIdleRecorderIgnoresEverythingItIsHanded) {
    // `Renderer::stopRecording` clears its pointer before the recorder is stopped, and a
    // failed `start()` leaves one that was never running. Neither may do anything.
    Recorder recorder;
    EXPECT_FALSE(recorder.active());
    EXPECT_EQ(recorder.framesOwed(10.0), 0u);

    const std::vector<uint8_t> pixels(64, 0xFF);
    recorder.submitFrame(pixels.data(), pixels.size(), 1);
    EXPECT_EQ(recorder.framesSubmitted(), 0u);
    EXPECT_EQ(recorder.framesDropped(), 0u);
    EXPECT_TRUE(recorder.stop().empty());
}
