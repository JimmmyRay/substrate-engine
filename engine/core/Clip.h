#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file engine/core/Clip.h
 * @brief A playhead moving along a timeline, and the named instants it crosses.
 *
 * Not animation. A skeleton and a flipbook both run on this arithmetic, so putting it beside
 * either makes the other name a module it may not.
 */
namespace core {

/// What happens when a playhead reaches the end of its timeline.
enum class LoopMode : uint32_t {
    Loop,
    /// Hold the final value. **Only a ClampToEnd playback ever reports finishing**, so
    /// anything waiting on `advance` to answer true against a looping one waits for ever.
    ClampToEnd,
};

/// One playhead on one timeline.
struct ClipPlayback {
    uint32_t clip = 0;
    /// Seconds into the clip, absolute rather than a phase.
    float time = 0.0f;
    float speed = 1.0f;
    LoopMode loop = LoopMode::Loop;
    bool playing = true;
};

/// A named instant on a timeline.
struct AnimationEvent {
    /// Seconds from the start of the clip.
    float time = 0.0f;
    std::string name;
};

/**
 * @brief Advance `p` by `dt` seconds along a timeline `duration` seconds long.
 *
 * @return true if a `ClampToEnd` playback is sitting on its final key. A looping playback
 *         never reports it, so a transition waiting on one waits forever.
 *
 * A paused playback advances by nothing and still reports whether it has finished.
 */
bool advance(ClipPlayback& p, float duration, float dt);

/**
 * @brief Append to `out` the indices of the `events` a step from `from` to `p.time` crossed.
 *
 * **`events` must be in ascending time order** -- the one thing a caller owes this
 * function, and what lets a crossing be found without sorting per frame.
 *
 * Every crossing in the interval fires, not just the nearest, and the wrap is handled: a
 * looping step past the end reports the events after `from` *and* those before the new
 * time. An event still fires **at most once per call** however many times `dt` lapped the
 * clip, so a dropped frame costs one footstep and not eleven.
 */
void crossedEvents(const ClipPlayback& p, const std::vector<AnimationEvent>& events, float duration, float from,
                   std::vector<uint32_t>& out);

} // namespace core
