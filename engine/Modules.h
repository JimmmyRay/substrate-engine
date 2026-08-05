#pragma once

#include "core/Slot.h"
#include "gfx/Particle.h"
#include "scene/AudioSource.h"
#include "scene/Collider.h"
#include "scene/ParticleEmitter.h"
#include "scene/Physics.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace core {
class AudioTap;
} // namespace core

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

/// @brief Audio: the device, the voices, the buses and the occlusion filters.
struct Audio {
    virtual ~Audio() = default;

    /// Every readout the startup summary, the recorder, the overlay and the profiler
    /// counters take, in one call. Twelve accessors instead makes twelve virtual calls of a
    /// question asked once a frame, and gives the inspector twelve places to drift from.
    struct Stats {
        /// Slots, not live sources: what a walker pairs with `sourceAt`.
        uint32_t sources = 0;
        uint32_t streamed = 0;
        uint32_t decoded = 0;
        uint64_t decodedBytes = 0;
        uint32_t buses = 0;
        uint32_t refused = 0;
        /// The mix format, whether or not a device came up -- the recorder muxes at this
        /// rate even for a silent track.
        uint32_t sampleRate = 0;
        uint32_t channels = 0;
        uint32_t listeners = 0;
        bool active = false;
    };

    /// One slot, as the engine reads it back.
    struct Source {
        scene::SoundId id;
        /// What the document or the config authored. Valid only until the next `create` or
        /// `update`, so it is read through and never held.
        const scene::AudioSourceDesc* desc = nullptr;
        glm::vec3 position{0.0f};
        float seconds = 0.0f;
        /// How far into occlusion this source is, 0 to 1, after the slew.
        float occlusion = 0.0f;
        bool streamed = false;
    };

    /// Open the device (or the device-less mix) and create the buses. False leaves every
    /// call below a no-op rather than a crash -- a game with no sound card still runs.
    virtual bool init(const scene::AudioConfig&) { return false; }
    virtual void shutdown() {}

    /// Load one source and start it if it says to. The engine never keeps the handle; a
    /// game that wants one calls `Engine::audio()` and gets it back.
    virtual void create(const scene::AudioSourceDesc&) {}

    [[nodiscard]] virtual Stats stats() const { return {}; }

    /// Read slot `slot` back, false for an empty one.
    [[nodiscard]] virtual bool sourceAt(uint32_t, Source*) const { return false; }

    /// Listener 0 only, which is the one the camera drives; the rest are a game's.
    virtual void setListener(const glm::vec3&, const glm::vec3&, const glm::vec3&) {}
    [[nodiscard]] virtual glm::vec3 listenerPosition(uint32_t) const { return glm::vec3(0.0f); }

    /// Where the scene tree writes the transform of a sound attached to a node.
    [[nodiscard]] virtual core::Slot<void(scene::SoundId, const glm::mat4&)> sourceTransforms() { return {}; }

    /// Re-place every source an animated node placed. The slot writes that node's world
    /// transform and answers whether it had one.
    virtual void placeSources(const core::Slot<bool(uint32_t, glm::mat4*)>&) {}

    /// Sweep one ray per occludable source per listener. The slot answers whether the
    /// segment is blocked, ignoring the body given -- see `audio::AudioEngine` for the rule
    /// about which listeners have to agree before a source is muffled.
    virtual void updateOcclusion(const core::Slot<bool(const glm::vec3&, const glm::vec3&, scene::BodyId)>&) {}

    /// The body a source rides, so the sweep does not report it occluded by what it is
    /// bolted to.
    virtual void setSourceBody(uint32_t, scene::BodyId) {}
    virtual void clearSourceBodies() {}

    /// Advance the ducking and the occlusion slews by one step, and mix. `dt` must be the
    /// fixed step, or the mixer is fed at the frame rate.
    virtual void update(float) {}

    /// Start teeing the mix into a ring for the recorder, and hand it over. Null when
    /// nothing is running, which is a recording with a silent track.
    [[nodiscard]] virtual core::AudioTap* startCapture(float) { return nullptr; }
    virtual void stopCapture() {}

    /// The do-nothing implementation `modules::audio` aims at until a module is linked.
    static Audio empty;
};

inline Audio Audio::empty;
inline Audio* audio = &Audio::empty;

} // namespace modules
