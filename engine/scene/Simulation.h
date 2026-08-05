#pragma once

#include "scene/Cloth.h"
#include "scene/Physics.h"
#include "scene/Scene.h"
#include "scene/SpriteTable.h"

/**
 * @file engine/scene/Simulation.h
 * @brief Everything a step moves, and the one place the order of a step is written down.
 *
 * Links without a device: pulling a Vulkan or window type in here, directly or through a
 * header, breaks the `substrate_tests`, baker and `substrate-sim` links on a driverless box.
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

    // Declaration order is load-bearing: cloth holds bodies in the physics world, so it must
    // be declared after `physics` to be destroyed before it.
    SpriteTable sprites;
    PhysicsWorld physics;
    ClothSystem cloth;
    FixedClock clock;

    /// The hierarchy: a body drives a node, and whatever hangs off that node follows.
    Scene tree;
};

} // namespace scene
