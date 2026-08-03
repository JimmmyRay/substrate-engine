#include "gfx/Presentation.h"

#include <gtest/gtest.h>

using namespace gfx;

/**
 * @file tests/PresentationTests.cpp
 * @brief P2's scale and letterbox arithmetic, which is the whole of what P2 can get wrong.
 *
 * The Vulkan half of the presentation step is one `vkCmdBlitImage` with `VK_FILTER_NEAREST`
 * and a clear behind it. Everything that decides whether a texel authored is the texel
 * presented happens before that call, in integers: which scale fits, which way an odd
 * leftover pixel leans, what a window too small for even 1x does, and whether the native
 * case reduces to something the renderer can skip. None of that needs a device, which is
 * why `Presentation.cpp` is in `SUBSTRATE_HOSTED_SOURCES` and these run under ASan.
 */

// ------------------------------------------------------------------- the exact fits

TEST(Presentation, NativeIsTheIdentity) {
    // The degenerate case the whole design turns on: virtual == window is scale 1, the
    // whole target, the whole window. `Renderer` elides the blit on exactly this answer,
    // and it is what keeps the eleven golden cases byte-identical across this row.
    const PresentLayout p = presentLayout(1920, 1080, 1920, 1080);
    EXPECT_EQ(p.scale, 1u);
    EXPECT_EQ(p.x, 0);
    EXPECT_EQ(p.y, 0);
    EXPECT_EQ(p.width, 1920u);
    EXPECT_EQ(p.height, 1080u);
    EXPECT_EQ(p.srcX, 0u);
    EXPECT_EQ(p.srcY, 0u);
    EXPECT_EQ(p.srcWidth, 1920u);
    EXPECT_EQ(p.srcHeight, 1080u);
    EXPECT_TRUE(identityPresent(p, 1920, 1080));
}

TEST(Presentation, ExactMultipleFillsTheWindow) {
    // 320x180 at 3x is 960x540. The readback case, and the one the arc's Phase 1
    // milestone is written in terms of.
    const PresentLayout p = presentLayout(320, 180, 960, 540);
    EXPECT_EQ(p.scale, 3u);
    EXPECT_EQ(p.x, 0);
    EXPECT_EQ(p.y, 0);
    EXPECT_EQ(p.width, 960u);
    EXPECT_EQ(p.height, 540u);
    EXPECT_EQ(p.srcWidth, 320u);
    EXPECT_EQ(p.srcHeight, 180u);
    // Not the identity: the scale is 3, so the blit is real.
    EXPECT_FALSE(identityPresent(p, 960, 540));
}

// ------------------------------------------------------- the window that does not fit

TEST(Presentation, NonIntegerMultipleFloorsAndLetterboxes) {
    // 1920x1080 holds 320x180 six times over, and 1000x600 holds it three -- 1000/320 is
    // 3.125 and 600/180 is 3.33, and the smaller floor wins. What is left over is bars,
    // never a 3.125x resample: that is the decision this row exists to make deliberate.
    const PresentLayout p = presentLayout(320, 180, 1000, 600);
    EXPECT_EQ(p.scale, 3u);
    EXPECT_EQ(p.width, 960u);
    EXPECT_EQ(p.height, 540u);
    EXPECT_EQ(p.x, 20);
    EXPECT_EQ(p.y, 30);
    EXPECT_EQ(p.srcWidth, 320u);
    EXPECT_EQ(p.srcHeight, 180u);
}

TEST(Presentation, TheTighterAxisPicksTheScale) {
    // Wide window, short one. Height allows 2x, width allows 6x; 2x is what is presented,
    // and the bars are vertical rather than horizontal.
    const PresentLayout p = presentLayout(320, 180, 1920, 400);
    EXPECT_EQ(p.scale, 2u);
    EXPECT_EQ(p.width, 640u);
    EXPECT_EQ(p.height, 360u);
    EXPECT_EQ(p.x, 640);
    EXPECT_EQ(p.y, 20);
}

TEST(Presentation, AnOddLeftoverLeansRightAndDown) {
    // 320x180 at 3x in a 963x543 window leaves 3 pixels on each axis. Truncating division
    // puts 1 on the left and 2 on the right. Somebody has to take it and the answer has to
    // be reproducible, or a letterboxed capture is a coin toss.
    const PresentLayout p = presentLayout(320, 180, 963, 543);
    EXPECT_EQ(p.scale, 3u);
    EXPECT_EQ(p.x, 1);
    EXPECT_EQ(p.y, 1);
    EXPECT_EQ(963u - p.width - static_cast<uint32_t>(p.x), 2u);
    EXPECT_EQ(543u - p.height - static_cast<uint32_t>(p.y), 2u);
}

// --------------------------------------------------------- the window that is too small

TEST(Presentation, TooSmallCropsRatherThanShrinks) {
    // A 640x360 window cannot hold 1280x720 at any integer scale. There is no half scale
    // available -- downscaling is the one thing in this function that would stop a texel
    // authored being a texel presented -- so the scale clamps at 1 and the source
    // rectangle narrows to what fits.
    const PresentLayout p = presentLayout(1280, 720, 640, 360);
    EXPECT_EQ(p.scale, 1u);
    EXPECT_EQ(p.srcWidth, 640u);
    EXPECT_EQ(p.srcHeight, 360u);
    EXPECT_EQ(p.width, 640u);
    EXPECT_EQ(p.height, 360u);
    // Centred on the virtual target: the middle of the world, at the size it was drawn.
    EXPECT_EQ(p.srcX, 320u);
    EXPECT_EQ(p.srcY, 180u);
    EXPECT_EQ(p.x, 0);
    EXPECT_EQ(p.y, 0);
    // Fills the window, but is not the identity -- srcX and srcY are not zero, so the
    // renderer must still record the blit.
    EXPECT_FALSE(identityPresent(p, 640, 360));
}

TEST(Presentation, CropOnOneAxisOnly) {
    // Tall enough, not wide enough. Only the horizontal crop happens, and the vertical
    // one does not appear out of sympathy.
    const PresentLayout p = presentLayout(400, 200, 300, 400);
    EXPECT_EQ(p.scale, 1u);
    EXPECT_EQ(p.srcWidth, 300u);
    EXPECT_EQ(p.srcHeight, 200u);
    EXPECT_EQ(p.srcX, 50u);
    EXPECT_EQ(p.srcY, 0u);
    EXPECT_EQ(p.y, 100);
}

// -------------------------------------------------------------------------- degenerate

TEST(Presentation, ZeroExtentYieldsNothingToRecord) {
    // A minimised window and a swapchain one pixel wide are both things a window manager
    // hands over during a resize, and vkCmdBlitImage rejects a zero extent. Callers read
    // a zero width as "record nothing", so returning a 1x1 rectangle here would present
    // one texel and look like it worked.
    for (const PresentLayout p : {presentLayout(320, 180, 0, 540), presentLayout(320, 180, 960, 0),
                                  presentLayout(0, 180, 960, 540), presentLayout(320, 0, 960, 540)}) {
        EXPECT_EQ(p.width, 0u);
        EXPECT_EQ(p.height, 0u);
    }
}

TEST(Presentation, OnePixelWindowStillProducesAValidBlit) {
    // Not zero, so there is something to record: one texel of the middle of the target.
    const PresentLayout p = presentLayout(320, 180, 1, 1);
    EXPECT_EQ(p.scale, 1u);
    EXPECT_EQ(p.width, 1u);
    EXPECT_EQ(p.height, 1u);
    EXPECT_EQ(p.srcWidth, 1u);
    EXPECT_EQ(p.srcHeight, 1u);
}

// ------------------------------------------------------------------------- invariants

TEST(Presentation, ScaleTimesSourceIsAlwaysTheDestination) {
    // The property that makes the blit exact: the destination rectangle is the source
    // rectangle times a whole number, so every destination texel maps to exactly one
    // source texel and VK_FILTER_NEAREST has nothing to interpolate. If this ever fails
    // the blit has become a resample, whatever the filter says.
    for (uint32_t w = 1; w <= 2000; w += 37) {
        for (uint32_t h = 1; h <= 1200; h += 29) {
            const PresentLayout p = presentLayout(320, 180, w, h);
            EXPECT_EQ(p.width, p.srcWidth * p.scale) << w << "x" << h;
            EXPECT_EQ(p.height, p.srcHeight * p.scale) << w << "x" << h;
            // And it never hangs off the edge of the window, which is what would make
            // vkCmdBlitImage a validation error rather than a wrong picture.
            EXPECT_LE(static_cast<uint32_t>(p.x) + p.width, w) << w << "x" << h;
            EXPECT_LE(static_cast<uint32_t>(p.y) + p.height, h) << w << "x" << h;
            EXPECT_GE(p.x, 0);
            EXPECT_GE(p.y, 0);
            // Nor off the edge of the source.
            EXPECT_LE(p.srcX + p.srcWidth, 320u);
            EXPECT_LE(p.srcY + p.srcHeight, 180u);
        }
    }
}

TEST(Presentation, IdentityOnlyWhereTheBlitWouldCopyEveryTexelUnchanged) {
    // identityPresent is the elision test, and eliding wrongly is the one way this file
    // can produce a black screen. Anything but an exact 1:1 fill must answer false.
    EXPECT_TRUE(identityPresent(presentLayout(800, 600, 800, 600), 800, 600));
    EXPECT_FALSE(identityPresent(presentLayout(400, 300, 800, 600), 800, 600)); // 2x
    EXPECT_FALSE(identityPresent(presentLayout(800, 600, 900, 600), 900, 600)); // bars
    EXPECT_FALSE(identityPresent(presentLayout(900, 600, 800, 600), 800, 600)); // crop
}
