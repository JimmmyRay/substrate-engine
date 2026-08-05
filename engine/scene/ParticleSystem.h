#pragma once

#include "scene/Node.h"
#include <rapidjson/fwd.h>
#include "core/Handle.h"
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scene {

/**
 * @file ParticleSystem.h
 * @brief Emitters, and the CPU half of the particle simulation.
 *
 * Slots are allocated here, in ascending order, from a free list rebuilt by a scan each
 * frame. A GPU dead list fed by `atomicAdd` is one dispatch cheaper and not
 * deterministic: two particles born on the same frame would swap slots between runs,
 * swap seeds with them, and every golden case in the suite would pay for it. The CPU can
 * predict deaths at all only because **a particle dies of age and nothing else** -- a
 * depth collision that killed rather than bounced would break the free list.
 */

/// Bits in `GpuEmitter::flags.y`. Shared with the shaders through `particle.glsl`.
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
    /// requiredCapacity().
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
    /// x texture index, y ParticleEmitterFlags, z flipbook `cols | rows << 16`, w spare
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
    void setEmitters(std::vector<ParticleEmitter> emitters, uint32_t budget);

    /**
     * @brief Add one emitter to a list `setEmitters` already sized the pool for.
     *
     * Does not resize the pool. `Engine` grows it from `wantedCapacity()` after the step
     * and resizes the renderer's buffers with it; an emitter created outside that pairing
     * shares whatever particles the existing ones did not claim, and the shortfall is
     * counted by `droppedSpawns()`.
     */
    EmitterId create(ParticleEmitter emitter);

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
    EmitterId spawnEffect(ParticleEmitter effect, const glm::vec3& position, const glm::vec3& normal = {});

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
    [[nodiscard]] const std::vector<GpuSpawn>& spawns() const { return spawnList; }

    [[nodiscard]] const std::vector<ParticleEmitter>& emitters() const { return emitterList; }
    /// Mutable, so an animated node can push a new transform in between frames.
    [[nodiscard]] std::vector<ParticleEmitter>& emitters() { return emitterList; }

    /// Write every emitter into `out` in the shaders' layout. `out` must hold
    /// `emitters().size()` entries.
    void writeGpuEmitters(GpuEmitter* out) const;

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
    [[nodiscard]] static uint32_t requiredCapacity(const std::vector<ParticleEmitter>& emitters);

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

    std::vector<ParticleEmitter> emitterList;
    /// A particle in slot `i` is alive while `deathTime[i] > now`.
    std::vector<float> deathTime;
    /// Rebuilt by the scan at the top of every update(), in ascending slot order -- see
    /// the file comment for what depends on that order.
    std::vector<uint32_t> freeSlots;
    std::vector<GpuSpawn> spawnList;

    uint32_t poolCapacity = 0;
    uint32_t alive = 0;
    uint32_t dropped = 0;
    float now = 0.0f;
    float lastStep = 0.0f;
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
