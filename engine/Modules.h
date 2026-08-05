#pragma once

#include "core/Slot.h"
#include "gfx/Particle.h"
#include "scene/Collider.h"
#include "scene/ParticleEmitter.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

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

/// @brief Particles: the pool a scene's emitters spawn into, stepped and drawn.
struct Particles {
    virtual ~Particles() = default;

    /// What the pool holds. The renderer's buffers are sized from both figures, so the
    /// two travel together.
    struct Pool {
        uint32_t capacity = 0;
        uint32_t emitters = 0;
    };

    /// Adopt a scene's emitters, size the pool for them and reset every particle. The
    /// second argument is a floor in particles, never a ceiling.
    virtual void setEmitters(std::vector<scene::ParticleEmitter>, uint32_t) {}

    /// Add one emitter to a pool already sized. It shares whatever the emitters already
    /// running did not claim until `growToWanted` runs.
    virtual void addEmitter(const scene::ParticleEmitter&) {}

    /// Advance by the fixed step. A frame delta here is a particle count that differs
    /// between runs.
    virtual void update(float) {}

    /// Grow the pool to what the live emitters need, answering whether it moved. **Only
    /// then may the renderer's buffers be resized, and they must be**: the CPU pool and
    /// the device storage the shaders write into are one allocation in two halves.
    virtual bool growToWanted() { return false; }

    [[nodiscard]] virtual Pool pool() const { return {}; }

    /// Re-place every emitter an animated node placed. The slot writes that node's world
    /// transform and answers whether it had one.
    virtual void placeEmitters(const core::Slot<bool(uint32_t, glm::mat4*)>&) {}

    /// Where the scene tree writes the transform of an emitter attached to a node.
    [[nodiscard]] virtual core::Slot<void(uint32_t, const glm::mat4&)> emitterTransforms() { return {}; }

    /// What the particle passes read this frame. The spans in it are valid until the
    /// next `update`, so it is taken per frame rather than held.
    [[nodiscard]] virtual gfx::ParticleFrame frame() const { return {}; }

    /// The do-nothing implementation `modules::particles` aims at until a module is linked.
    static Particles empty;
};

inline Particles Particles::empty;
inline Particles* particles = &Particles::empty;

} // namespace modules
