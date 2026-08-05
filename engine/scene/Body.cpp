#include "scene/Body.h"

#include <cstdint>

namespace scene {

void FixedClock::accumulate(float dt) {
    thisFrame = 0;
    // Scaled here rather than at any consumer, so everything downstream of the step inherits
    // the pause without knowing there is one. At the default scale it is a multiplication by
    // exactly 1.0f, so a locked clock stays bit-identical.
    if (dt > 0.0f) accumulator += dt * timeScaleValue;
}

bool FixedClock::consume() {
    if (accumulator < stepSeconds) return false;

    if (thisFrame >= maxSteps) {
        // Discarded in one go: carrying the overrun into the next frame only defers it and
        // hides that it happened.
        const auto skipped = static_cast<uint32_t>(accumulator / stepSeconds);
        dropped += skipped;
        accumulator -= static_cast<float>(skipped) * stepSeconds;
        return false;
    }

    accumulator -= stepSeconds;
    ++thisFrame;
    ++total;
    return true;
}

float FixedClock::alpha() const {
    const float a = accumulator / stepSeconds;
    // 0.99999994f is the float below 1: the contract is [0, 1), and returning 1 would let an
    // interpolation land on the next step's state a step early.
    return a < 0.0f ? 0.0f : (a >= 1.0f ? 0.99999994f : a);
}

} // namespace scene
