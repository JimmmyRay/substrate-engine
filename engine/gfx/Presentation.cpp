#include "gfx/Presentation.h"

#include <algorithm>

namespace gfx {

PresentLayout presentLayout(uint32_t virtualWidth, uint32_t virtualHeight, uint32_t windowWidth,
                            uint32_t windowHeight) {
    PresentLayout layout;

    // A zero on either side leaves every extent zero, which is the caller's signal to
    // record nothing at all. Returning a 1x1 rectangle instead would present one texel
    // of a minimised window, which is worse than presenting none: it looks like it works.
    if (virtualWidth == 0 || virtualHeight == 0 || windowWidth == 0 || windowHeight == 0) return layout;

    // Floored, and clamped at 1 rather than allowed to reach 0. See the header for why
    // neither rounding nor fitting is available here.
    const uint32_t fit = std::min(windowWidth / virtualWidth, windowHeight / virtualHeight);
    layout.scale = std::max(1u, fit);

    // What fits at that scale. Equal to the virtual extent in every case except a window
    // too small to hold it at 1x, which crops.
    layout.srcWidth = std::min(virtualWidth, windowWidth / layout.scale);
    layout.srcHeight = std::min(virtualHeight, windowHeight / layout.scale);
    // Centred on the target, so a window narrowed by a drag loses the same number of
    // columns from each side rather than scrolling the world sideways.
    layout.srcX = (virtualWidth - layout.srcWidth) / 2;
    layout.srcY = (virtualHeight - layout.srcHeight) / 2;

    layout.width = layout.srcWidth * layout.scale;
    layout.height = layout.srcHeight * layout.scale;

    // Truncating division, so an odd leftover leans right and down. Stated in the header
    // because a letterboxed golden image depends on it and no implementation is obviously
    // more correct than the other.
    layout.x = static_cast<int32_t>((windowWidth - layout.width) / 2);
    layout.y = static_cast<int32_t>((windowHeight - layout.height) / 2);
    return layout;
}

bool identityPresent(const PresentLayout& layout, uint32_t windowWidth, uint32_t windowHeight) {
    return layout.scale == 1 && layout.x == 0 && layout.y == 0 && layout.srcX == 0 && layout.srcY == 0 &&
           layout.width == windowWidth && layout.height == windowHeight;
}

} // namespace gfx
