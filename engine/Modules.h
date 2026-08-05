#pragma once

#include "scene/Collider.h"

#include <span>

/**
 * @file engine/Modules.h
 * @brief What the engine calls on a module, and what happens when none is linked.
 *
 * Naming a module's concrete type from `Engine.cpp` -- an object file every game links --
 * links that module into every game. The engine reaches a module only through the
 * interfaces here.
 *
 * A method declared without a default body breaks every game that links no module for it.
 */
namespace modules {

/// @brief Navigation: a walkable surface baked from what a scene says is solid.
struct Nav {
    virtual ~Nav() = default;

    /// Rebuild from a loaded scene's colliders. **Takes every collider and filters them
    /// itself** -- pre-filtering at the call site drops geometry the bake needs to close
    /// off unwalkable ground.
    virtual void rebuild(std::span<const scene::ColliderDesc>) {}

    /// The do-nothing implementation `modules::nav` aims at until a module is linked.
    static Nav empty;
};

inline Nav Nav::empty;
inline Nav* nav = &Nav::empty;

} // namespace modules
