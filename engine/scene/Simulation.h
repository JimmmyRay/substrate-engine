#pragma once

#include "scene/Animation.h"
#include "scene/Audio.h"
#include "scene/Cloth.h"
#include "scene/Locomotion.h"
#include "scene/ParticleSystem.h"
#include "scene/Physics.h"
#include "scene/Scene.h"
#include "scene/SpriteTable.h"

#include <vector>

/**
 * @file engine/scene/Simulation.h
 * @brief Everything a step moves, and the one place the order of a step is written down.
 *
 * This holds no Vulkan type and no window. It is in `SUBSTRATE_HOSTED_SOURCES`, so it links
 * into `substrate_tests` and `substrate-sim` with no driver present -- which is the whole of
 * C27: a dedicated server, a CI run on a GPU-less box or a batch tuning job steps a scene
 * through the same code the renderer's engine does, rather than through a second copy of it
 * that drifts.
 *
 * **`Engine` owns one of these and delegates to it.** `Engine::simulate` is one line now. The
 * point is not the encapsulation, it is that `step` exists exactly once: a call order
 * reimplemented against a headless loop is a call order that will disagree with the drawn one
 * on the frame it matters.
 *
 * What is deliberately *not* here: the camera, the input map, the scene loader, the spatial
 * index and the instance table. Each is host-side and could live here; none of them is moved
 * by a step, and a simulation that owned the camera would be inviting a headless loop to
 * decide what is on screen. See systems.md, "The simulation half".
 */
namespace scene {

class Simulation {
  public:
    /**
     * @brief Advance every mover by one fixed step, in the order the engine documents.
     *
     * `stepSeconds` is the fixed step and never a frame delta -- a solver integrated against
     * a variable delta is non-deterministic by construction, which is the property the whole
     * fixed-step arrangement exists to keep.
     */
    void step(float stepSeconds);

    /**
     * @brief The pose array a node's transform should be read out of.
     *
     * **Per node, not per scene, and that is the whole of the fix it came from.** A
     * `ParticleEmitter` and an `AudioSourceDesc` carry a bare node index with nowhere to say
     * which rig it belongs to; resolving every one of them against character 0 put a torch on
     * the second character in the first one's hand. Character 0 stays the answer for a node
     * nothing animates -- a rigid node belongs to the scene rather than to any one copy of a
     * character, and every character resolves it identically.
     */
    [[nodiscard]] const std::vector<glm::mat4>& poseFor(uint32_t node) const;

    // Public members rather than an accessor apiece. This is a bag of subsystems that a
    // caller drives directly; wrapping each in a getter would be thirty lines of nothing.
    // **Declaration order is load-bearing**: cloth holds bodies in the physics world and has
    // to be destroyed before it.
    SpriteTable sprites;
    SceneAnimator animator;
    LocomotionDriver locomotion;
    ParticleSystem particles;
    PhysicsWorld physics;
    ClothSystem cloth;
    AudioEngine audio;
    FixedClock clock;

    /// The hierarchy: a body drives a node, and whatever hangs off that node follows.
    Scene tree;

    /// The body each source is attached to, or an invalid handle. Read by the occlusion ray,
    /// which must not count the object a sound is bolted to as the thing occluding it.
    /// **Includes static bodies** -- a hum on a bolted-down generator has no transform to
    /// drive and still occludes itself.
    std::vector<BodyId> sourceBody;

    /// Whether the occlusion sweep runs at all, and how far short of each end its ray stops.
    /// Set from `GameSetup` once, where the audio config was built, so the step reads two
    /// values rather than reaching back into a game's setup every frame.
    bool occlusion = false;
    float occlusionMargin = 0.0f;
};

} // namespace scene
