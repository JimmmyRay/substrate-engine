#pragma once

#include "core/Handle.h"

#include <glm/glm.hpp>

#include <cstdint>

/**
 * @file engine/scene/Body.h
 * @brief What a scene calls a body, how the world it goes into is configured, and the fixed
 *        step everything is moved on.
 *
 * Description, so it stays where the loaders, the tree and the mixer can reach it; the solver
 * that acts on it is `physics/PhysicsWorld.h`. Naming a Jolt or a Vulkan type here drops every
 * translation unit that touches a scene out of the driverless link.
 */
namespace scene {

/// Everything the world needs to exist. Defaults are the engine's, not Jolt's, where the two
/// disagree.
struct PhysicsConfig {
    /// The simulation step, in seconds. Shared with animation and particles: three subsystems
    /// stepping at three rates make "frame 60" mean three different times.
    float step = 1.0f / 60.0f;
    /// Steps a single frame may run before the remaining accumulated time is dropped, counted
    /// and reported rather than chased into a frame that never ends.
    uint32_t maxStepsPerFrame = 4;
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    /// Bodies the world can hold; zero sizes it from the scene -- see `PhysicsWorld::init`.
    uint32_t bodyBudget = 0;
    /// Zero selects Jolt's single-threaded job system; any other value is a pool of that size.
    /// Determinism needs the count fixed, not one -- see `physics/PhysicsWorld.h`.
    uint32_t workerThreads = 0;
    /// Collision sub-steps inside one `step`. Jolt's own default is 1.
    uint32_t collisionSteps = 1;
};

/**
 * @brief The accumulator that turns a variable frame rate into a fixed step.
 *
 * Animation and particles step on it too, so a scene with no colliders still has one.
 *
 * A locked clock is this same path fed exactly `step`, which lands the accumulator on exactly
 * zero in float: one step, `alpha()` zero. Adding a second path for it is what would let the
 * locked and realtime clocks drift apart.
 */
class FixedClock {
  public:
    explicit FixedClock(float step = 1.0f / 60.0f, uint32_t maxStepsPerFrame = 4)
        : stepSeconds(step > 0.0f ? step : 1.0f / 60.0f), maxSteps(maxStepsPerFrame) {}

    /// Add a frame's worth of time and begin a new frame's budget of steps. Negative time is
    /// ignored: a clock stepped across a system time change is handed one.
    void accumulate(float dt);

    /// True when another step should run this frame. Consumes it.
    [[nodiscard]] bool consume();

    /// How far into the next step the render time sits, in [0, 1). Exactly zero under a
    /// locked clock.
    [[nodiscard]] float alpha() const;

    /**
     * @brief Scale time before it is accumulated. 0 is paused, 1 is normal, negatives clamp.
     *
     * Applied to what the accumulator *receives*, so animation, particles, physics and audio
     * occlusion inherit it wherever they inherit their step; rendering, input and the UI sit
     * outside the step and keep running. Audio playback is miniaudio's clock and this cannot
     * reach it -- silence during a pause is `AudioEngine::setMuted`.
     */
    void setTimeScale(float scale) { timeScaleValue = scale > 0.0f ? scale : 0.0f; }
    [[nodiscard]] float timeScale() const { return timeScaleValue; }
    /// True when the scale has stopped time.
    [[nodiscard]] bool paused() const { return timeScaleValue == 0.0f; }

    [[nodiscard]] float step() const { return stepSeconds; }
    /// Steps taken since construction.
    [[nodiscard]] uint64_t stepCount() const { return total; }
    /// Whole steps discarded by the per-frame cap, cumulative. Time the simulation did not
    /// run is a fact a game needs, so this must never be dropped silently.
    [[nodiscard]] uint32_t droppedSteps() const { return dropped; }
    [[nodiscard]] uint32_t stepsThisFrame() const { return thisFrame; }

  private:
    float stepSeconds;
    uint32_t maxSteps;
    float timeScaleValue = 1.0f;
    float accumulator = 0.0f;
    uint32_t thisFrame = 0;
    uint32_t dropped = 0;
    uint64_t total = 0;
};

/// Tags that make a body handle and a character handle unrelated types. Declared, never
/// defined -- see `core/Handle.h`.
struct BodyTag;
struct PhysicsCharacterTag;

/// A rigid body in the physics world.
using BodyId = core::Handle<BodyTag>;
/// A `CharacterVirtual`: not solved with the world but reading it, which is why it is its own
/// handle type. `SceneAnimator`'s characters are unrelated things.
using PhysicsCharacterId = core::Handle<PhysicsCharacterTag>;

/**
 * @brief What a character is standing on, or not.
 *
 * The third value is why this type exists: a face steeper than the collider's `maxSlopeAngle`
 * is neither ground nor mid-air. Folding it into `OnGround` walks a character up a cliff;
 * folding it into `InAir` plays a fall clip for something the solver is sliding down a slope.
 */
enum class CharacterGround : uint8_t {
    InAir,    ///< Touching nothing.
    OnGround, ///< Standing. The only state a jump launches from, coyote window aside.
    Sliding,  ///< Too steep to stand on, or Jolt's `NotSupported`: a body that cannot hold it.
};

} // namespace scene
