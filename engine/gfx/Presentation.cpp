#include "gfx/Presentation.h"

#include <algorithm>

namespace gfx {

PresentLayout presentLayout(uint32_t virtualWidth, uint32_t virtualHeight, uint32_t windowWidth,
                            uint32_t windowHeight) {
    PresentLayout layout;

    // A zero on either side leaves every extent zero, the caller's signal to record
    // nothing. A 1x1 rectangle instead would present one texel of a minimised window,
    // which is worse than presenting none: it looks like it works.
    if (virtualWidth == 0 || virtualHeight == 0 || windowWidth == 0 || windowHeight == 0) return layout;

    // Floored and clamped at 1; see the header for what rounding or fitting costs.
    const uint32_t fit = std::min(windowWidth / virtualWidth, windowHeight / virtualHeight);
    layout.scale = std::max(1u, fit);

    layout.srcWidth = std::min(virtualWidth, windowWidth / layout.scale);
    layout.srcHeight = std::min(virtualHeight, windowHeight / layout.scale);
    // Centred on the target, so a window narrowed by a drag loses the same number of
    // columns from each side rather than scrolling the world sideways.
    layout.srcX = (virtualWidth - layout.srcWidth) / 2;
    layout.srcY = (virtualHeight - layout.srcHeight) / 2;

    layout.width = layout.srcWidth * layout.scale;
    layout.height = layout.srcHeight * layout.scale;

    // Truncating division, so an odd leftover leans right and down -- a letterboxed golden
    // image depends on which side takes it.
    layout.x = static_cast<int32_t>((windowWidth - layout.width) / 2);
    layout.y = static_cast<int32_t>((windowHeight - layout.height) / 2);
    return layout;
}

bool identityPresent(const PresentLayout& layout, uint32_t windowWidth, uint32_t windowHeight) {
    return layout.scale == 1 && layout.x == 0 && layout.y == 0 && layout.srcX == 0 && layout.srcY == 0 &&
           layout.width == windowWidth && layout.height == windowHeight;
}

} // namespace gfx
