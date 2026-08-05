#include "core/DebugView.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace core;

/**
 * @file tests/DebugViewTests.cpp
 * @brief The debug view list, and the step that walks it (G8).
 *
 * `Renderer::cycleDebugView` is the door G8 opened, and the arithmetic behind it is what
 * every caller used to write out by hand: a cast to `uint32_t`, a modulo over
 * `DebugView::Count`, and a cast back. Each of those is a place to be off by one, and the
 * symptom of being off by one is a key that skips a view or lands on `Count` -- which
 * `lighting_body.glsl` has no branch for, so the picture is whatever the last `else`
 * happened to be. That is a wrong image rather than a crash, which is the kind this
 * project asserts rather than eyeballs.
 *
 * The order is not an internal detail either. `--debug-view albedo`, `--debug-view normal`,
 * `--debug-view depth` and `--debug-view ssao` are four golden cases, so the positions
 * asserted below are positions the reference images already photograph.
 */

namespace {

constexpr int kCount = static_cast<int>(DebugView::Count);

std::string keyAt(DebugView from, int step) {
    const char* key = debugViewKey(advanceDebugView(from, step));
    return key != nullptr ? std::string(key) : std::string("<count>");
}

} // namespace

TEST(DebugViewTest, OneStepWalksTheListInEnumOrder) {
    const std::vector<std::string> expected{"lit", "albedo", "normal", "orm", "depth", "emissive", "ssao", "edges"};
    ASSERT_EQ(expected.size(), static_cast<size_t>(kCount));

    DebugView view = DebugView::Lit;
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], keyAt(view, 0)) << "at position " << i;
        view = advanceDebugView(view, +1);
    }
    // The wrap: eight steps from Lit is Lit again, not a ninth view and not `Count`.
    EXPECT_EQ(DebugView::Lit, view);
}

TEST(DebugViewTest, TheFourViewsTheGoldenSetPinsAreWhereTheCycleLandsOnThem) {
    // A cycle key is only worth having if it reaches the views somebody named. These four
    // are the ones `scripts/golden.sh` drives through `--debug-view`.
    EXPECT_EQ("albedo", keyAt(DebugView::Lit, 1));
    EXPECT_EQ("normal", keyAt(DebugView::Lit, 2));
    EXPECT_EQ("depth", keyAt(DebugView::Lit, 4));
    EXPECT_EQ("ssao", keyAt(DebugView::Lit, 6));
}

TEST(DebugViewTest, SteppingBackwardsWrapsThroughTheFrontOfTheList) {
    EXPECT_EQ(DebugView::Edges, advanceDebugView(DebugView::Lit, -1));
    EXPECT_EQ(DebugView::Lit, advanceDebugView(DebugView::Albedo, -1));
    EXPECT_EQ(DebugView::Ssao, advanceDebugView(DebugView::Lit, -2));
}

TEST(DebugViewTest, AStepOfZeroOrAWholeListIsIdentity) {
    for (int i = 0; i < kCount; ++i) {
        const auto view = static_cast<DebugView>(i);
        EXPECT_EQ(view, advanceDebugView(view, 0));
        EXPECT_EQ(view, advanceDebugView(view, kCount));
        EXPECT_EQ(view, advanceDebugView(view, -kCount));
    }
}

TEST(DebugViewTest, NoStepHoweverLargeOrNegativeLeavesTheList) {
    // The reduction is what this is defending. A version that reduced only one operand
    // still passed every case above and fell off the front here.
    for (int step = -5 * kCount - 3; step <= 5 * kCount + 3; ++step) {
        for (int i = 0; i < kCount; ++i) {
            const DebugView result = advanceDebugView(static_cast<DebugView>(i), step);
            EXPECT_LT(static_cast<int>(result), kCount) << "step " << step << " from " << i;
            EXPECT_NE(nullptr, debugViewKey(result)) << "step " << step << " from " << i;
        }
    }
}

TEST(DebugViewTest, ASequenceOfSingleStepsAgreesWithOneBigStep) {
    for (int step = 0; step < 3 * kCount; ++step) {
        DebugView walked = DebugView::Normal;
        for (int i = 0; i < step; ++i) walked = advanceDebugView(walked, +1);
        EXPECT_EQ(walked, advanceDebugView(DebugView::Normal, step)) << "step " << step;
    }
}
