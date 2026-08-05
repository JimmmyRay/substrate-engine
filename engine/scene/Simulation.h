#pragma once

#include "scene/Body.h"
#include "scene/Scene.h"
#include "scene/SpriteTable.h"

/**
 * @file engine/scene/Simulation.h
 * @brief Everything a step moves, and the one place the order of a step is written down.
 *
 * Every mover a step calls is a module now, reached through `engine/Modules.h`, so what is
 * left here is what the engine itself owns. Naming a module type in this header would link
 * that module into every game, and naming a Vulkan or window type would break the
 * `substrate_tests`, baker and `substrate-sim` links on a driverless box. What belongs in a
 * step and what stays host-side is argued in systems.md, "The simulation half".
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

    SpriteTable sprites;
    FixedClock clock;

    /// The hierarchy: a body drives a node, and whatever hangs off that node follows.
    Scene tree;
};

} // namespace scene
