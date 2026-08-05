#pragma once

#include "scene/Node.h"

#include <rapidjson/fwd.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file engine/scene/ParticleEmitter.h
 * @brief What a glTF document authors, and what reads it back out.
 *
 * The description half, beside `Collider.h` and `AudioSource.h` for the same reason: a
 * loader, a `.scene` sidecar and the scene tree all reach these, and none of them may name
 * the module that simulates them. `particles::ParticleSystem` is the other half.
 */
namespace scene {

/// Bits in `gfx::GpuEmitter::flags.y`. Shared with the shaders through `particle.glsl`.
enum ParticleEmitterFlags : uint32_t {
    /// The colour is radiance rather than albedo: lighting is skipped and the colour
    /// multiplied by `emissiveIntensity`.
    kEmitterEmissive = 1u << 0,
    /// Test against the depth buffer and bounce. Costs a texture fetch per particle per
    /// step even where nothing is near enough to hit.
    kEmitterCollides = 1u << 1,
};

/**
 * @brief One emitter, as `nodes[i].extras.substrate_emitter` authored it.
 *
 * The same emitter declared under two nodes is two emitters at two transforms, and only
 * the node knows where each copy is, so placing them is the scene's job.
 */
struct ParticleEmitter {
    std::string name;
    /// World placement. Spawn positions and velocities are in emitter space, so rotating
    /// the node aims the jet.
    glm::mat4 transform{1.0f};
    /// glTF node that placed it, or `kNoNode` for an emitter built in code.
    uint32_t node = kNoNode;

    /// Particles per second. With `lifetime`, what sizes the pool -- see
    /// `particles::ParticleSystem::requiredCapacity`.
    float rate = 100.0f;
    /// Seconds a particle lives. The pool is sized from the *maximum* once
    /// `lifetimeJitter` is applied; a jitter applied anywhere but here would let
    /// particles outlive the figure the capacity was computed from.
    float lifetime = 2.0f;
    /// Fractional spread on `lifetime`, so 0.25 gives [0.75, 1.25] x lifetime.
    float lifetimeJitter = 0.0f;

    /// Initial velocity in emitter space, before the cone spread.
    glm::vec3 velocity{0.0f, 1.0f, 0.0f};
    /// Fractional spread on the speed, applied after the cone.
    float speedJitter = 0.0f;
    /// Half-angle of the cone the initial direction is scattered into, in radians.
    float coneAngle = 0.0f;
    /// Half-extents of the box particles are born in, in emitter space. Zero is a point
    /// source.
    glm::vec3 boxExtent{0.0f};

    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    /// Velocity lost per second, as a fraction. 0 is vacuum.
    float drag = 0.0f;

    /// Colour at birth and at death, interpolated over the particle's own life. Alpha is
    /// coverage: particle.frag premultiplies, so one blend state serves both additive and
    /// alpha particles.
    glm::vec4 colorStart{1.0f};
    glm::vec4 colorEnd{1.0f, 1.0f, 1.0f, 0.0f};
    float sizeStart = 0.1f;
    float sizeEnd = 0.3f;

    /// Index into the scene's bindless texture array, or UINT32_MAX for the procedural
    /// soft disc.
    uint32_t texture = 0xFFFFFFFFu;
    /// @brief Grid `texture` is divided into, played over the particle's life. 1x1 is one
    ///        still frame. Packed into 16 bits each by `writeGpuEmitters`.
    uint32_t flipbookCols = 1;
    uint32_t flipbookRows = 1;
    /// Times the sheet is played over one lifetime. Wraps, so it need not be whole.
    float flipbookLoops = 1.0f;
    /// Maximum billboard spin, radians per second; each particle draws a rate in
    /// [-spin, spin] from its seed.
    float spin = 0.0f;
    /// Fraction of the sprite's alpha eaten away by death, in [0, 1], against the
    /// sprite's own detail rather than uniformly.
    float erosion = 0.0f;
    bool emissive = false;
    float emissiveIntensity = 1.0f;
    bool collides = false;
    /// Fraction of the normal velocity kept on a bounce. 0 settles a particle on the
    /// surface it hit.
    float restitution = 0.3f;

    /// Particles to emit once and then be done. Zero spawns at `rate` forever; non-zero
    /// makes this a one-shot whose slot the system releases itself once the last of them
    /// has died, so a caller need not hold the handle to clean it up.
    uint32_t burst = 0;

    /// Fractional particle carried between frames, so a rate of 0.5/s emits one
    /// particle every two seconds rather than none ever.
    float accumulator = 0.0f;
    /// How many particles this emitter has emitted, and *the* seed source: every random
    /// number a particle draws is a hash of it. Reseeding from anything that varies
    /// between runs -- wall time, frame rate, slot index -- ends reproducibility.
    uint32_t emitted = 0;

    /// Longest a particle from this emitter can live.
    [[nodiscard]] float maxLifetime() const { return lifetime * (1.0f + lifetimeJitter); }
};

/**
 * @brief Read every `nodes[i].extras.substrate_emitter` out of a glTF `nodes` array.
 *
 * The transform is left at identity and `node` carries the node index, so the caller
 * still has to place each emitter -- `GltfScene::load` does it in the same walk that
 * places a light.
 */
[[nodiscard]] bool parseSceneEmitters(const rapidjson::Value& nodesArray, std::vector<ParticleEmitter>& out);

} // namespace scene
