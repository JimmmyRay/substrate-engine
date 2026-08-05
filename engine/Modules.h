#pragma once

#include "scene/Collider.h"

#include <span>

/**
 * @file engine/Modules.h
 * @brief What the engine calls on a module, and what happens when none is linked.
 *
 * `engine/` is a static library and the linker pulls an archive member only to resolve an
 * undefined symbol, so an object file every game links -- `Engine.cpp.o` -- that *names* a
 * module drags that module into every game. This header is how the engine calls a module
 * without naming it: one abstract interface per module, and a pointer per module that starts
 * out aimed at a base whose every method does nothing.
 *
 * **The base class is the null implementation.** There is no `NullNav` subclass: every
 * method has a default body right here, so the "no module linked" behaviour is written once,
 * beside the declaration, and a call site reads `modules::nav->rebuild(colliders)` with no
 * `if` in front of it. A method added without a default is a compile error in every real
 * implementation -- which is the reminder that "what does this do when nothing is linked" is
 * a question with an answer rather than an oversight.
 *
 * **These interfaces are engine-facing only.** A game reaches the concrete type through
 * `Engine`'s accessors, which are declared against a forward declaration and defined in the
 * module's own translation unit, so a game keeps the whole public API of `nav::NavMesh`
 * without this file growing a copy of it. What lives here is the handful of calls the engine
 * itself makes -- for `nav`, exactly one.
 *
 * **Order is written, not registered.** These are named pointers called in a written
 * sequence by `Simulation::step` and `Engine`, not a list something iterates: a frame's order
 * has to be readable in one place rather than depend on which module registered first. That
 * is also why there is no common base with a virtual `update()`.
 *
 * `core::Slot` remains the answer where there is no object -- a callback the engine hands a
 * module, like the per-node transform writers in `SceneTargets`. An interface where there is
 * a thing; a slot where there is a function.
 */
namespace modules {

/**
 * @brief Navigation: a walkable surface baked from what a scene says is solid.
 *
 * One method, because one is all the engine does with it. Everything a game asks -- nearest
 * point, path, steering -- it asks `nav::NavMesh` directly through `Engine::navMesh()`.
 */
struct Nav {
    virtual ~Nav() = default;

    /// Rebuild from a loaded scene's colliders. Called at load, after an import that brought
    /// colliders, and on a scene swap. **Takes every collider and filters them itself**: which
    /// of them are walkable is navigation's question, not the caller's.
    virtual void rebuild(std::span<const scene::ColliderDesc>) {}

    /// The null module -- see the file note. A game that never includes `nav/NavModule.h`
    /// runs against this, and bakes nothing.
    static Nav empty;
};

inline Nav Nav::empty;
inline Nav* nav = &Nav::empty;

} // namespace modules
