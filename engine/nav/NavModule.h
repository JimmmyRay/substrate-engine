#pragma once

#include "nav/NavMesh.h"

/**
 * @file engine/nav/NavModule.h
 * @brief What links navigation into a game, and the only header that names both halves.
 *
 * Calling `Engine::navMesh()` or `Engine::bakeNavMesh()` is what pulls `NavModule.cpp` in and
 * runs the registrar that aims `modules::nav` at a real navmesh; a game that names neither
 * bakes nothing.
 *
 * Folding that registrar into `NavMesh.cpp` gives it `Engine`, which drops it out of
 * `SUBSTRATE_HOSTED_SOURCES` and out of every sanitized unit run.
 */

#include "scene/Collider.h"

#include <span>

namespace nav {

/// Bake `colliders` into `out`. **Takes every collider and filters them itself** --
/// pre-filtering at the call site drops geometry the bake needs to close off unwalkable
/// ground.
void bakeNavMesh(std::span<const scene::ColliderDesc> colliders, NavMesh& out, const NavBuildParams& params);

} // namespace nav
