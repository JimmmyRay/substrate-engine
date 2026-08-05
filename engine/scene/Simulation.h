#pragma once

#include "scene/Animation.h"
#include "scene/Cloth.h"
#include "scene/Locomotion.h"
#include "scene/Physics.h"
#include "scene/Scene.h"
#include "scene/SpriteTable.h"

#include <vector>

/**
 * @file engine/scene/Simulation.h
 * @brief Everything a step moves, and the one place the order of a step is written down.
 *
 * In `SUBSTRATE_HOSTED_SOURCES`: pulling a Vulkan or window type in here, directly or through
 * a header, breaks the `substrate_tests` and `substrate-sim` links on a driverless box.
 * What belongs in a step and what stays host-side is argued in systems.md, "The simulation
 * half".
 */
namespace scene {

class Simulation {
  public:
    /**
     * @brief Advance every mover by one fixed step, in the order the engine documents.
     *
     * `stepSeconds` is the fixed step and never a frame delta: a solver integrated against a
     * variable delta is non-deterministic by construction.
     */
    void step(float stepSeconds);

    /**
     * @brief The pose array a node's transform should be read out of.
     *
     * Resolved per node, because a `ParticleEmitter` and an `AudioSourceDesc` carry a bare
     * node index and nothing saying which rig owns it -- resolving them all against character
     * 0 puts the second character's torch in the first one's hand. Character 0 is still the
     * answer for a node nothing animates, which every character resolves identically.
     */
    [[nodiscard]] const std::vector<glm::mat4>& poseFor(uint32_t node) const;

    // Declaration order is load-bearing: cloth holds bodies in the physics world, so it must
    // be declared after `physics` to be destroyed before it.
    SpriteTable sprites;
    SceneAnimator animator;
    LocomotionDriver locomotion;
    PhysicsWorld physics;
    ClothSystem cloth;
    FixedClock clock;

    /// The hierarchy: a body drives a node, and whatever hangs off that node follows.
    Scene tree;

  private:
    /// `poseFor` in the shape the audio module's placement slot takes, false for a node the
    /// rig it resolved to does not reach.
    [[nodiscard]] bool poseOf(uint32_t node, glm::mat4* out) const;
};

} // namespace scene
