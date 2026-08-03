#pragma once

#include <cstdint>

namespace gfx {

/**
 * @file engine/gfx/Presentation.h
 * @brief Where the virtual target lands in the window, in whole texels (P2).
 *
 * ## There is one presentation path, and native is its degenerate case
 *
 * The engine renders into a target of size `V` and presents it at the largest integer
 * `S` that fits the window, centred, with whatever is left over as black bars. Native
 * rendering is `V = the window extent`, which makes `S` exactly 1, the source rectangle
 * the whole target and the destination rectangle the whole window -- an identity the
 * renderer elides rather than a second mode it selects. Writing it as two modes would be
 * writing the same arithmetic twice and then discovering the two disagreed about
 * rounding, which is the only interesting thing here.
 *
 * ## Why this holds no Vulkan
 *
 * Same split `ImageTable` draws, for the same reason. What goes wrong in a presentation
 * step is not a Vulkan call: it is an off-by-one in a scale, a bar one pixel wider on the
 * left than the right, or a window one pixel too small silently becoming a 0.996x
 * resample. All three are arithmetic, and arithmetic is testable in a suite that links no
 * device. `Renderer` turns the result into a `VkImageBlit` and nothing else.
 */

/**
 * @brief The scale, the source rectangle and where it lands, all in whole texels.
 *
 * Every field is an integer, and that is the guarantee rather than an implementation
 * detail: a `float` anywhere in this struct would be a fractional scale waiting to
 * happen, and a fractional scale is the artefact the whole P arc exists to remove.
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
    /// blit's two rectangles cannot be derived by two different expressions.
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
 * Four decisions, and each one is a place a plausible implementation goes wrong:
 *
 * **The scale is `min(W/vw, H/vh)` by integer division, floored, and never zero.** Not
 * rounded and not fitted: rounding up overflows the window and fitting exactly is a
 * fractional resample, which is a nearest-neighbour blit at 1.03x -- every 33rd column
 * of texels doubled, a shimmer that moves when the window is dragged, and precisely the
 * artefact this row removes. A window that is not an integer multiple of the virtual
 * resolution therefore presents *smaller* than the window and pays black bars for it.
 *
 * **The leftover goes into bars, and an odd leftover leans to the right and the bottom.**
 * `x = (W - width) / 2` truncates, so a 3-pixel remainder is 1 on the left and 2 on the
 * right. Somebody has to take it; stating which side means a golden image of a
 * letterboxed frame is reproducible rather than a coin toss.
 *
 * **A window too small for even 1x crops rather than shrinks.** There is no half scale:
 * `S` clamps at 1 and the source rectangle narrows to what fits, centred on the virtual
 * target. A game running in a window smaller than its own resolution sees the middle of
 * its world, at the size it was drawn, instead of a smoothly wrong picture of all of it.
 * Downscaling would be the one case in this function where a texel authored is not a
 * texel presented.
 *
 * **A zero anywhere yields a layout with a zero extent**, which callers must elide. A
 * swapchain one pixel wide is a legal thing for a window manager to hand over mid-resize
 * and a zero-height one is what a minimise looks like; `vkCmdBlitImage` rejects both.
 */
[[nodiscard]] PresentLayout presentLayout(uint32_t virtualWidth, uint32_t virtualHeight, uint32_t windowWidth,
                                          uint32_t windowHeight);

/**
 * @brief Whether this layout is the identity -- the whole target, at 1x, filling the window.
 *
 * The one question `Renderer` asks of a layout, and it is asked once per swapchain rather
 * than once per frame: a true answer means the offscreen target and the blit are both
 * unnecessary, so the tonemap draws straight into the swapchain image exactly as it did
 * before this row existed. That elision is what keeps the golden suite byte-identical,
 * and it is why the native path costs nothing to have become a special case of a general
 * one.
 */
[[nodiscard]] bool identityPresent(const PresentLayout& layout, uint32_t windowWidth, uint32_t windowHeight);

} // namespace gfx
