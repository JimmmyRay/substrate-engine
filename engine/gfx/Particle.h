#pragma once

#include "core/Slot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

/**
 * @file engine/gfx/Particle.h
 * @brief The particle layouts and arithmetic `engine/shaders/particle.glsl` also declares,
 *        and what one frame of the passes needs.
 *
 * Every `static_assert` below is against that file, and `particleHash`/`particleRandom` are
 * bit-identical to the GLSL of the same names: a particle's position is drawn from its seed
 * on the GPU and its lifetime from the same seed on the CPU, so the two generators
 * disagreeing is a slot freed on a different frame than the one it dies on.
 */
namespace gfx {

/// One emitter as the shaders read it. Must match `Emitter` in particle.glsl.
struct GpuEmitter {
    glm::mat4 transform{1.0f};
    glm::vec4 velocity{0.0f, 1.0f, 0.0f, 0.0f}; ///< xyz emitter-space, w speed jitter
    glm::vec4 boxExtent{0.0f};                  ///< xyz half-extents, w cone half-angle
    glm::vec4 gravity{0.0f, -9.81f, 0.0f, 0.0f};///< xyz, w drag
    glm::vec4 colorStart{1.0f};
    glm::vec4 colorEnd{1.0f, 1.0f, 1.0f, 0.0f};
    /// x sizeStart, y sizeEnd, z restitution, w emissive intensity
    glm::vec4 params{0.1f, 0.3f, 0.3f, 1.0f};
    /// x spin (rad/s), y erosion, z flipbook loops per lifetime, w spare
    glm::vec4 sprite{0.0f, 0.0f, 1.0f, 0.0f};
    /// x texture index, y scene::ParticleEmitterFlags, z flipbook `cols | rows << 16`, w spare
    glm::uvec4 flags{0xFFFFFFFFu, 0u, 0x00010001u, 0u};
};

static_assert(sizeof(GpuEmitter) == 192, "GpuEmitter must match particle.glsl");

/// One particle. Must match `Particle` in particle.glsl; 64 bytes keeps the pool's
/// stride one cache line.
struct GpuParticle {
    glm::vec4 position{0.0f}; ///< xyz world, w birth time on the simulation clock
    glm::vec4 velocity{0.0f}; ///< xyz world, w lifetime in seconds; 0 is a dead slot
    glm::vec4 color{0.0f};    ///< premultiplied lit radiance, a = coverage
    glm::uvec4 meta{0u};      ///< x emitter, y seed, z and w spare
};

static_assert(sizeof(GpuParticle) == 64, "GpuParticle must match particle.glsl");

/// One particle to be born this frame. Must match `Spawn` in particle.glsl.
struct GpuSpawn {
    /// x slot, y emitter, z seed, w spare
    glm::uvec4 meta{0u};
    /// x lifetime, y birth time, z and w spare.
    ///
    /// A birth time and not an age: the CPU frees a slot when `birth + lifetime <= now`
    /// and the shader kills the particle on the same comparison. An age advanced by
    /// `+= dt` in the shader drifts from the CPU's prediction, and the two then disagree
    /// about the frame a particle dies on -- a slot reused while its occupant still draws.
    glm::vec4 params{0.0f};
};

static_assert(sizeof(GpuSpawn) == 32, "GpuSpawn must match particle.glsl");

/// Integer hash, bit-identical to `particleHash` in particle.glsl. The seeds fed to it
/// are consecutive -- an emission counter -- so a substitute must decorrelate adjacent
/// inputs as well as this one (Wang / "lowbias32") does.
inline uint32_t particleHash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// A number in [0, 1) from a particle's seed and a stream index, bit-identical to the
/// GLSL of the same name. 24 bits, because that is what a float can hold exactly.
inline float particleRandom(uint32_t seed, uint32_t stream) {
    const uint32_t h = particleHash(seed * 747796405u + stream * 2891336453u);
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

/// Streams `particleRandom` is called with. Stream 0 is the CPU's and 1-9 are
/// particle.glsl's; reusing one for a second purpose correlates the two draws, since both
/// come from the same particle seed.
enum ParticleRandomStream : uint32_t {
    kRandomLifetime = 0,
    kRandomPosX = 1,
    kRandomPosY = 2,
    kRandomPosZ = 3,
    kRandomConeU = 4,
    kRandomConeV = 5,
    kRandomSpeed = 6,
    kRandomSpinPhase = 7,
    kRandomSpinRate = 8,
    kRandomFrameStart = 9,
};

/**
 * @brief Everything `Renderer::recordParticles` reads for one frame.
 *
 * Pushed once per frame, never pulled: the renderer may not name the simulation that
 * produces this. A field left at its default is a pass running on a frame nobody filled --
 * `alive` of zero draws nothing and `now` of zero kills every particle at once -- so a
 * field added here needs its writer in the same change, and only the golden images say
 * otherwise.
 */
struct ParticleFrame {
    /// This frame's births, in ascending slot order. Valid until the next step.
    std::span<const GpuSpawn> spawns;
    /// Emitters `writeEmitters` fills, and what the emitter buffer was sized for by
    /// `Renderer::setParticleCapacity`.
    uint32_t emitterCount = 0;
    /// Fills `emitterCount` entries of mapped memory.
    core::Slot<void(GpuEmitter*)> writeEmitters;
    /// Live particles, and exactly the instance count the draw submits: the sort puts
    /// every dead slot after every live one.
    uint32_t alive = 0;
    /// Spawns refused since the emitter list was set. Reported when it changes.
    uint32_t dropped = 0;
    /// The simulation clock the shaders run their death comparison against.
    float now = 0.0f;
    /// The fixed step `particle_simulate.comp` integrates by.
    float dt = 0.0f;
};

} // namespace gfx
