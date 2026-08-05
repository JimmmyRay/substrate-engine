#pragma once

#include <cstdint>

namespace gfx {

/**
 * @file engine/gfx/Presentation.h
 * @brief Where the virtual target lands in the window, in whole texels.
 *
 * The engine renders into a target of size `V` and presents it at the largest integer `S`
 * that fits the window, centred, with the leftover as black bars. Native rendering is
 * `V = the window extent`, an identity `identityPresent` detects rather than a second mode
 * -- writing it as two paths is writing the same arithmetic twice and discovering later
 * that the two round differently.
 */

/**
 * @brief The scale, the source rectangle and where it lands, all in whole texels.
 *
 * Every field is an integer, and that is the guarantee: a `float` here is a fractional
 * scale, which is a shimmer under a nearest-neighbour blit.
 */
struct PresentLayout {
    /// Integer magnification, never zero. One means the source is copied texel for texel.
    uint32_t scale = 1;

    /// Top-left of the presented rectangle in window pixels. Signed to match
    /// `VkOffset3D`, never negative: the rectangle is clipped to the window rather than
    /// hung off its edge.
    int32_t x = 0;
    int32_t y = 0;
    /// `scale * srcWidth` and `scale * srcHeight`. Held rather than recomputed so the
    /// blit's two rectangles cannot come from two different expressions.
    uint32_t width = 0;
    uint32_t height = 0;

    /// The region of the virtual target that is presented. The whole of it in every case
    /// except one -- see `presentLayout`.
    uint32_t srcX = 0;
    uint32_t srcY = 0;
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
};

/**
 * @brief Fit a `virtual` target into a `window`, at an integer scale, centred.
 *
 * Four contracts a caller can break:
 *
 * - **The scale is floored, never rounded and never zero.** Rounding up overflows the
 *   window; fitting exactly is a fractional resample, which under a nearest-neighbour blit
 *   doubles every 33rd column at 1.03x and shimmers as the window is dragged.
 * - **An odd leftover leans right and down**, because `(W - width) / 2` truncates. A
 *   golden image of a letterboxed frame depends on which side takes it.
 * - **A window too small for 1x crops rather than shrinks**, so a texel authored is still
 *   a texel presented.
 * - **A zero anywhere yields a zero extent**, which callers must elide: a one-pixel or
 *   zero-height swapchain is legal mid-resize and `vkCmdBlitImage` rejects both.
 */
[[nodiscard]] PresentLayout presentLayout(uint32_t virtualWidth, uint32_t virtualHeight, uint32_t windowWidth,
                                          uint32_t windowHeight);

/**
 * @brief Whether this layout is the identity -- the whole target, at 1x, filling the window.
 *
 * True means `Renderer` drops the offscreen target and the blit and tonemaps straight into
 * the swapchain image. That elision is what keeps the golden suite byte-identical, so a
 * layout this wrongly rejects costs a resample on the native path.
 */
[[nodiscard]] bool identityPresent(const PresentLayout& layout, uint32_t windowWidth, uint32_t windowHeight);

} // namespace gfx
