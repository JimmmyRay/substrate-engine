#pragma once

#include "physics/ClothSystem.h"
#include "physics/PhysicsWorld.h"

/**
 * @file engine/physics/PhysicsModule.h
 * @brief What links physics into a game, and the only header that names both halves.
 *
 * Calling `Engine::physics()` is what pulls `PhysicsModule.cpp` in and runs the registrar that
 * aims `modules::physics` at a real world; a game that never names it creates no body, solves
 * no cloth, and every collider the scene authored is scenery.
 *
 * Folding that registrar into `PhysicsWorld.cpp` gives it `Engine`, which drops the solver out
 * of every link that has no device -- the unit suite, the baker and the sim loop -- and so out
 * of every sanitized run.
 */
namespace physics {

/**
 * @brief The engine's one world, for a host that has no `Engine`.
 *
 * `substrate-sim` and the step-order tests drive `scene::Simulation` directly, and what that
 * steps is this object rather than one of their own -- so this is where they build the scene
 * whose step they are asserting about. A game calls `Engine::physics()`, which is this.
 */
[[nodiscard]] PhysicsWorld& world();

} // namespace physics
