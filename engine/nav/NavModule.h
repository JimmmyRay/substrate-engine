#pragma once

#include "nav/NavMesh.h"

/**
 * @file engine/nav/NavModule.h
 * @brief What links navigation into a game, and the only header that names both halves.
 *
 * **Including this is what links navigation.** `Engine::navMesh()` and `Engine::bakeNavMesh()`
 * are declared in `Engine.h` against a forward declaration and *defined* in `NavModule.cpp`;
 * naming either is the undefined symbol that makes the linker pull that object file out of
 * the archive, and pulling it in runs the file-static registrar that points `modules::nav` at
 * a real navmesh. A game that never includes this header links neither, and `Engine` bakes
 * nothing because the module it calls is still the do-nothing base it was born as.
 *
 * `nav/NavMesh.h` is the other half -- the walkable surface and nothing else: no `Engine`, no
 * Vulkan -- so `NavMesh.cpp` stays in `SUBSTRATE_HOSTED_SOURCES` and runs under every
 * sanitizer. **The two are separate translation units for exactly that reason.** Fold the
 * registrar into `NavMesh.cpp` and it acquires `Engine`, leaves the hosted set, and the unit
 * suite can no longer link the navmesh at all.
 *
 * Beyond the link, the types a game uses are all in `nav/NavMesh.h`, which this includes.
 */

#include "scene/Collider.h"

#include <span>

namespace nav {

/// Bake `colliders` into `out`. **Takes every collider and filters them itself** -- which of
/// them are walkable is navigation's question, not the caller's; see the note on the loop.
///
/// A game wanting a second navmesh for a second body size calls `Engine::bakeNavMesh`, which
/// is this with the loaded scene's colliders already supplied.
void bakeNavMesh(std::span<const scene::ColliderDesc> colliders, NavMesh& out, const NavBuildParams& params);

} // namespace nav
