#include "core/Clip.h"

#include <algorithm>
#include <cmath>

namespace core {

bool advance(ClipPlayback& p, float duration, float dt) {
    if (p.playing) p.time += dt * p.speed;

    if (duration <= 0.0f) {
        p.time = 0.0f;
        return p.loop == LoopMode::ClampToEnd;
    }

    if (p.loop == LoopMode::Loop) {
        // fmod of a negative time is negative, so a clip played backwards walks off the
        // front. One add is enough: |fmod| is strictly less than the duration.
        p.time = std::fmod(p.time, duration);
        if (p.time < 0.0f) p.time += duration;
        return false;
    }

    p.time = std::clamp(p.time, 0.0f, duration);
    // Reverse playback finishes at the *start*; testing against the duration leaves a
    // clamped clip run backwards never finishing.
    return p.speed < 0.0f ? p.time <= 0.0f : p.time >= duration;
}

void crossedEvents(const ClipPlayback& p, const std::vector<AnimationEvent>& events, float duration, float from,
                   std::vector<uint32_t>& out) {
    if (events.empty() || !p.playing) return;

    const float to = p.time;
    const bool reverse = p.speed < 0.0f;

    // Half-open on the side the playhead came *from*, so an event exactly under the
    // playhead fires once as it is reached and not again on the next frame.
    const auto fireBetween = [&](float lo, float hi) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(events.size()); ++i) {
            const float t = events[i].time;
            if (t > lo && t <= hi) out.push_back(i);
        }
    };

    if (duration > 0.0f && p.loop == LoopMode::Loop) {
        // `advance` has already wrapped `p.time`, so a step that crossed the end shows up
        // as the new time being *behind* the old one. The two intervals must not overlap,
        // or an event fires twice for one step.
        const bool wrapped = reverse ? to > from : to < from;
        if (!wrapped) {
            reverse ? fireBetween(to, from) : fireBetween(from, to);
            return;
        }
        if (reverse) {
            fireBetween(-1.0f, from);
            fireBetween(to, duration);
        } else {
            fireBetween(from, duration);
            fireBetween(-1.0f, to);
        }
        return;
    }

    reverse ? fireBetween(to, from) : fireBetween(from, to);
}

} // namespace core
