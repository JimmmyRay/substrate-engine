#pragma once

#include "core/Handle.h"
#include "gfx/Particle.h"
#include "scene/ParticleEmitter.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace particles {

/**
 * @file engine/particles/ParticleSystem.h
 * @brief The CPU half of the particle simulation.
 *
 * Slots are allocated here, in ascending order, from a free list rebuilt by a scan each
 * frame. A GPU dead list fed by `atomicAdd` is one dispatch cheaper and not
 * deterministic: two particles born on the same frame would swap slots between runs,
 * swap seeds with them, and every golden case in the suite would pay for it. The CPU can
 * predict deaths at all only because **a particle dies of age and nothing else** -- a
 * depth collision that killed rather than bounced would break the free list.
 *
 * `scene::ParticleEmitter` is the description this simulates. It stays in `scene/`, which
 * the loaders and the scene tree can reach and this directory is not.
 */

/// Tag for an emitter. Declared, never defined -- see `core/Handle.h`.
struct EmitterTag;
using EmitterId = core::Handle<EmitterTag>;

class ParticleSystem {
  public:
    /// Hard ceiling on the pool. The sort key packs a quantised depth and a slot index
    /// into one 32-bit word, so every bit added here is a bit taken off the depth
    /// resolution the blend order depends on.
    static constexpr uint32_t kMaxCapacity = 65536;

    /**
     * @brief Adopt `emitters` and size the pool for them. Resets every particle.
     *
     * @param budget Floor on the pool, in particles: allocated up front even where the
     *        emitters need less. The pool still grows past it when they need more.
     */
    void setEmitters(std::vector<scene::ParticleEmitter> emitters, uint32_t budget);

    /**
     * @brief Add one emitter to a list `setEmitters` already sized the pool for.
     *
     * Does not resize the pool. `Engine` grows it from `wantedCapacity()` after the step
     * and resizes the renderer's buffers with it; an emitter created outside that pairing
     * shares whatever particles the existing ones did not claim, and the shortfall is
     * counted by `droppedSpawns()`.
     */
    EmitterId create(scene::ParticleEmitter emitter);

    /**
     * @brief Spawn a one-shot effect at a point.
     *
     * `position` and `normal` replace the emitter's transform, the emitter's local +Y
     * aimed along `normal` -- so an authored upward spray becomes a spray off the surface
     * that was hit. A zero normal leaves the effect unrotated. `effect.burst` is forced
     * non-zero, because a continuous emitter spawned this way would run forever.
     *
     * @return the emitter's handle, which a caller may equally keep to cancel it early or
     *         throw away. It goes stale by itself when the last particle dies.
     */
    EmitterId spawnEffect(scene::ParticleEmitter effect, const glm::vec3& position, const glm::vec3& normal = {});

    /// Retire an emitter. It stops spawning immediately; particles already in flight are
    /// left to expire, since killing them is a visible pop and the pool reclaims their
    /// slots either way.
    void destroy(EmitterId id);

    [[nodiscard]] bool valid(EmitterId id) const {
        return id.valid() && id.index < slots.size() && slots[id.index].generation == id.generation &&
               slots[id.index].live;
    }

    /// The handle occupying a slot, invalid for a retired one. Slot order is the order
    /// `emitters()` and `writeGpuEmitters` use, so a walker pairs the two.
    [[nodiscard]] EmitterId emitterAt(uint32_t slot) const {
        if (slot >= slots.size() || !slots[slot].live) return {};
        return EmitterId{slot, slots[slot].generation};
    }

    /// @brief Retire expired slots, then emit this frame's particles.
    ///
    /// `dt` must be the fixed step: a particle count that depends on the frame rate is a
    /// particle count that differs between runs.
    void update(float dt);

    /// Slots the pool holds. Zero for a scene with no emitters.
    [[nodiscard]] uint32_t capacity() const { return poolCapacity; }

    /// What the emitters currently running need, which is what `capacity()` is grown to.
    /// Live emitters only -- a retired slot keeps its record until something overwrites
    /// it, and counting those would hold the pool at every effect that ever ran.
    [[nodiscard]] uint32_t wantedCapacity() const;

    /**
     * @brief Enlarge the pool to hold at least `atLeast` particles, keeping the ones in
     *        flight. False when it was already big enough, or is at `kMaxCapacity`.
     *
     * **Not a call a game makes.** The renderer sized its buffers from `capacity()`, so
     * growing this alone emits into storage the device does not have.
     * `Engine::growParticles` is the pair, and is the only caller.
     */
    bool grow(uint32_t atLeast);
    /// Live particles after the most recent update(), and exactly the instance count the
    /// draw submits, because the sort puts every dead slot after every live one.
    [[nodiscard]] uint32_t aliveCount() const { return alive; }
    /// Particles an emitter asked for and the pool had no room for, since the last
    /// setEmitters(). Counted rather than logged per birth, and reported by the renderer
    /// when it changes.
    [[nodiscard]] uint32_t droppedSpawns() const { return dropped; }
    [[nodiscard]] bool empty() const { return emitterList.empty(); }

    /// This frame's births, in ascending slot order.
    [[nodiscard]] const std::vector<gfx::GpuSpawn>& spawns() const { return spawnList; }

    [[nodiscard]] const std::vector<scene::ParticleEmitter>& emitters() const { return emitterList; }
    /// Mutable, so an animated node can push a new transform in between frames.
    [[nodiscard]] std::vector<scene::ParticleEmitter>& emitters() { return emitterList; }
    /// Slots `emitters()` holds, live or retired, and what the renderer sizes its emitter
    /// buffer for.
    [[nodiscard]] uint32_t emitterCount() const { return static_cast<uint32_t>(emitterList.size()); }

    /// Place the emitter in `slot`. A slot past the end is ignored: the scene tree carries
    /// attachment indices from a load the emitter list may have been replaced since.
    void setEmitterTransform(uint32_t slot, const glm::mat4& transform) {
        if (slot < emitterList.size()) emitterList[slot].transform = transform;
    }

    /// Write every emitter into `out` in the shaders' layout. `out` must hold
    /// `emitterCount()` entries.
    void writeGpuEmitters(gfx::GpuEmitter* out) const;

    /// Simulated time after the most recent update(), in seconds. The shaders take it as
    /// a push constant and run the same death comparison against it that update() ran.
    [[nodiscard]] float time() const { return now; }
    /// The step update() was last called with, and what the integrator in
    /// `particle_simulate.comp` advances by.
    [[nodiscard]] float step() const { return lastStep; }

    /// @brief Slots the steady state of `emitters` needs, before any budget is applied.
    ///
    /// Rounded up to a power of two because the bitonic sort's domain must be one, and
    /// given one spare particle per emitter because a rate below one per frame still
    /// emits.
    [[nodiscard]] static uint32_t requiredCapacity(const std::vector<scene::ParticleEmitter>& emitters);

  private:
    struct Slot {
        uint32_t generation = 1;
        bool live = true;
        /// One-shots only: the time the last particle this emitter will ever spawn dies,
        /// after which `update` releases the slot. Negative for a continuous emitter.
        float expiresAt = -1.0f;
        bool burstDone = false;
    };
    std::vector<Slot> slots;

    std::vector<scene::ParticleEmitter> emitterList;
    /// A particle in slot `i` is alive while `deathTime[i] > now`.
    std::vector<float> deathTime;
    /// Rebuilt by the scan at the top of every update(), in ascending slot order -- see
    /// the file comment for what depends on that order.
    std::vector<uint32_t> freeSlots;
    std::vector<gfx::GpuSpawn> spawnList;

    uint32_t poolCapacity = 0;
    uint32_t alive = 0;
    uint32_t dropped = 0;
    float now = 0.0f;
    float lastStep = 0.0f;
};

} // namespace particles
