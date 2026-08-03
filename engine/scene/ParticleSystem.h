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
 * @brief Emitters, and the CPU half of the particle simulation (S3.1, S3.2).
 *
 * The GPU owns the *particles*: `particle_simulate.comp` integrates every live slot,
 * lights it and writes its sort key, and nothing is ever read back to shade or to
 * submit. What lives here is the part a compute shader is the wrong place for --
 * what an emitter *is*, how big the pool has to be, and which slot each new particle
 * goes into.
 *
 * ## Why the CPU allocates slots
 *
 * The obvious GPU design is a dead list the simulate pass appends to with `atomicAdd`
 * and the emit pass pops from. It is one dispatch smaller than this one and it is not
 * deterministic: the order two threads win an atomic is not defined, so two particles
 * born on the same frame swap slots between runs, swap seeds with them, and produce a
 * different image. 5.3's whole claim is that frame N is the same image on every run,
 * and a subsystem that quietly ends that would be paid for by every golden case in the
 * suite rather than by this one.
 *
 * So slots are allocated here, in ascending order, from a free list rebuilt by a scan
 * each frame. That is only possible because **a particle dies of age and nothing
 * else**: the CPU can predict every death the moment the particle is born. Depth
 * collision (S3.4) therefore bounces rather than kills -- see the note in
 * `particle_simulate.comp`, which is where that constraint is actually felt.
 *
 * The scan is O(capacity) per frame over a byte-per-slot array. At the sizes this
 * system produces -- a few thousand -- that is microseconds, and it is one loop rather
 * than the heap a "smarter" expiry queue would need.
 *
 * ## Determinism of the jitter
 *
 * Every random number is `particleRandom(seed, stream)`, an integer hash of the
 * particle's own seed. The seed is a per-emitter counter, so it is a function of how
 * many particles that emitter has emitted and of nothing else -- not of wall time, not
 * of the frame rate, not of which slot the particle landed in. `particle.glsl` carries
 * the same two functions, written to produce bit-identical results, because the CPU
 * picks the lifetime and the GPU picks the direction from the same seed.
 */

/// Bits in `GpuEmitter::flags.y`. Shared with the shaders through `particle.glsl`.
enum ParticleEmitterFlags : uint32_t {
    /// The colour is radiance rather than albedo: skip lighting entirely and multiply
    /// by `emissiveIntensity`. What a spark or a flame is, and the whole of S3.5's
    /// fidelity-versus-cost decision expressed as data rather than as a global.
    kEmitterEmissive = 1u << 0,
    /// Test against the depth buffer and bounce (S3.4). Off is not merely cheaper: a
    /// particle that never approaches geometry pays a texture fetch to learn so.
    kEmitterCollides = 1u << 1,
};

/**
 * @brief One emitter, as `nodes[i].extras.substrate_emitter` authored it (S3.1).
 *
 * Placed by its glTF node, exactly as a `KHR_lights_punctual` light is: the same
 * emitter declared under two nodes is two emitters at two transforms, and only the
 * node knows where each copy is. `node` is retained so an animated hierarchy can push
 * a new transform in every frame -- a torch on a walking character is an emitter whose
 * transform is a joint's.
 *
 * Every field has a working default and every key is optional, for the reason `Config`
 * gives: an emitter that names three properties should get three properties and the
 * engine's defaults for the rest.
 */
struct ParticleEmitter {
    std::string name;
    /// World placement. Spawn positions and velocities are in emitter space and this
    /// takes them out of it, so rotating the node aims the jet.
    glm::mat4 transform{1.0f};
    /// glTF node that placed it, or `kNoNode` for an emitter built in code. This was
    /// the copy that spelled the literal out and explained it in a comment, which is
    /// what a missed Rule of Threes looks like (D3).
    uint32_t node = kNoNode;

    /// Particles per second. With `lifetime`, this is what sizes the pool -- see
    /// requiredCapacity().
    float rate = 100.0f;
    /// Seconds a particle lives. The *maximum*, once `lifetimeJitter` is applied, is
    /// what the pool is sized from; a jitter that made particles live longer than the
    /// figure the capacity was computed from would be a silent overflow.
    float lifetime = 2.0f;
    /// Fractional spread on `lifetime`, so 0.25 gives [0.75, 1.25] x lifetime.
    float lifetimeJitter = 0.0f;

    /// Initial velocity in emitter space, before the cone spread.
    glm::vec3 velocity{0.0f, 1.0f, 0.0f};
    /// Fractional spread on the speed, applied after the cone.
    float speedJitter = 0.0f;
    /// Half-angle of the cone the initial direction is scattered into, in radians. Zero
    /// emits a perfectly collimated jet, which is what a spark trail wants and what
    /// smoke never does.
    float coneAngle = 0.0f;
    /// Half-extents of the box particles are born in, in emitter space. Zero is a point
    /// source.
    glm::vec3 boxExtent{0.0f};

    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    /// Velocity lost per second, as a fraction. 0 is vacuum; smoke wants 1 or more.
    float drag = 0.0f;

    /// Colour at birth and at death, interpolated over the particle's own life. Alpha
    /// is coverage; the shader premultiplies, which is what lets one blend state serve
    /// both additive and alpha particles -- see particle.frag.
    glm::vec4 colorStart{1.0f};
    glm::vec4 colorEnd{1.0f, 1.0f, 1.0f, 0.0f};
    float sizeStart = 0.1f;
    float sizeEnd = 0.3f;

    /// Index into the scene's bindless texture array, or UINT32_MAX for the procedural
    /// soft disc. A default that needs no asset: a particle system whose first run
    /// depends on somebody having authored a sprite sheet is one nobody sees working.
    uint32_t texture = 0xFFFFFFFFu;
    /**
     * @brief Grid `texture` is divided into, played over the particle's life.
     *
     * 1x1 -- the default -- is one still frame, which is what every texture that is not a
     * sheet is. Anything larger walks `cols * rows` cells at the rate `flipbookLoops` sets,
     * from a start frame the particle's own seed picks, cross-fading between neighbours.
     *
     * Fire and smoke are a sheet rather than a tinted disc, and why is
     * systems.md, "Particles: the sheet is the effect".
     */
    uint32_t flipbookCols = 1;
    uint32_t flipbookRows = 1;
    /**
     * @brief Times the sheet is played over one lifetime. Wraps, so it need not be whole.
     *
     * **Below 1 is the useful direction.** One loop per lifetime is one loop per *short*
     * lifetime -- sixteen frames across a 0.6 s flame particle is 26 fps of churn, which
     * reads as a fire in a hurry. This is what decouples how fast the sprite boils from how
     * long the particle lives.
     */
    float flipbookLoops = 1.0f;
    /// Maximum billboard spin, radians per second. Each particle draws a start angle and
    /// a rate in [-spin, spin] from its seed. Smoke wants it; a flame does not -- fire
    /// licks upward, and a rotating one reads as a tumbling object.
    float spin = 0.0f;
    /// Fraction of the sprite's alpha eaten away by death, against the sprite's own
    /// detail. 0 is the plain fade `colorEnd.a` gives; higher dissolves the particle into
    /// wisps instead of dimming it uniformly, which is what stops a puff from vanishing
    /// as a whole shape.
    float erosion = 0.0f;
    bool emissive = false;
    float emissiveIntensity = 1.0f;
    bool collides = false;
    /// Fraction of the normal velocity kept on a bounce. 0 makes a particle settle on
    /// the surface it hit, which is what dust does.
    float restitution = 0.3f;

    /// Particles to emit once and then be done (C3). Zero is the continuous emitter a
    /// glTF authors: it spawns at `rate` forever. Non-zero makes this a **one-shot** --
    /// `burst` particles at the next update, then no more, and the system releases the
    /// slot itself once the last of them has died. That self-destruction is what lets a
    /// game spawn an impact without also having to remember to clean it up.
    uint32_t burst = 0;

    // ------------------------------------------------------------------- runtime
    /// Fractional particle carried between frames, so a rate of 0.5/s emits one
    /// particle every two seconds rather than none ever.
    float accumulator = 0.0f;
    /// How many particles this emitter has emitted. *The* seed source: every random
    /// number a particle uses is a hash of this, so the sequence is a function of the
    /// emitter's own history and of nothing else.
    uint32_t emitted = 0;

    /// Longest a particle from this emitter can live.
    [[nodiscard]] float maxLifetime() const { return lifetime * (1.0f + lifetimeJitter); }
};

/**
 * @brief One emitter as the shaders read it. Must match `Emitter` in particle.glsl.
 *
 * A separate struct from `ParticleEmitter` rather than the same one with the CPU
 * fields tacked on. The GPU never reads a name, a node index or an accumulator, and
 * `rate` and `lifetime` are decisions the CPU has already made by the time a spawn
 * record exists -- so uploading them would be uploading the question along with its
 * answer.
 */
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

/**
 * @brief One particle. Must match `Particle` in particle.glsl.
 *
 * 64 bytes, which is one cache line and keeps the pool's stride a power of two. The
 * colour is stored rather than recomputed per vertex because it is the *lit* colour:
 * `particle_simulate.comp` evaluates the lights once per particle, and the six vertices
 * of its billboard read the result. That is the whole of S3.5 -- see the note there.
 */
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
    /// Both are *chosen here* rather than derived in the shader, and the birth time is
    /// the reason a particle stores when it was born instead of how old it is. The CPU
    /// frees a slot when `birth + lifetime <= now` and the GPU kills the particle on the
    /// same comparison; an age the shader advanced by `+= dt` would drift from a death
    /// the CPU predicted by addition, and the two would disagree about which frame a
    /// particle dies on -- which is a slot reused while its occupant is still drawing.
    glm::vec4 params{0.0f};
};

static_assert(sizeof(GpuSpawn) == 32, "GpuSpawn must match particle.glsl");

/// Integer hash, bit-identical to `particleHash` in particle.glsl. Two-round xorshift
/// multiply (Wang / "lowbias32"): cheap, and it decorrelates consecutive seeds, which
/// matters because the seeds *are* consecutive -- they are an emission counter.
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

/// Streams `particleRandom` is called with, so the CPU and the shaders cannot
/// accidentally draw the same number for two different purposes.
enum ParticleRandomStream : uint32_t {
    kRandomLifetime = 0, ///< CPU: how long this particle lives
    kRandomPosX = 1,     ///< shader: where in the emitter box it is born
    kRandomPosY = 2,
    kRandomPosZ = 3,
    kRandomConeU = 4, ///< shader: cone direction
    kRandomConeV = 5,
    kRandomSpeed = 6,      ///< shader: speed jitter
    kRandomSpinPhase = 7,  ///< shader: billboard angle at birth
    kRandomSpinRate = 8,   ///< shader: signed fraction of `spin`
    kRandomFrameStart = 9, ///< shader: which flipbook cell the particle starts on
};

/**
 * @brief The scene's emitters, and the slot bookkeeping behind them.
 *
 * Not a manager and not a component system: it owns one array of emitters, one array
 * of death times, and a free list. Everything it produces per frame is a
 * `std::vector<GpuSpawn>` the renderer uploads and an alive count the renderer draws.
 */
/// Tag for an emitter. Declared, never defined -- see `core/Handle.h`.
struct EmitterTag;
using EmitterId = core::Handle<EmitterTag>;

class ParticleSystem {
  public:
    /// Hard ceiling on the pool, and it is a consequence rather than a preference: the
    /// sort key (S3.3) packs a quantised depth and a slot index into one 32-bit word,
    /// and 16 bits of slot leaves 16 bits of depth. Past this the depth resolution the
    /// blend order depends on would start to go, so the limit is here rather than in
    /// the sort where nobody would find it.
    static constexpr uint32_t kMaxCapacity = 65536;

    /**
     * @brief Adopt `emitters` and size the pool for them.
     *
     * @param budget Ceiling from `render.particleBudget`. A **stated budget**, in 0.9's
     *        sense: what binds first is the figure the emitters actually need, and this
     *        only ever caps it -- reported when it does, never silently.
     *
     * Resets every particle. Emitters are a load-time property of the scene in every
     * caller today, so this is a load-time call; nothing here would break if it were
     * not, beyond the particles in flight.
     */
    void setEmitters(std::vector<ParticleEmitter> emitters, uint32_t budget);

    /**
     * @brief Add one emitter to a list `setEmitters` already sized the pool for (C1).
     *
     * **The pool is not resized.** `setEmitters` derives `capacity()` from the emitters it
     * was given, and that capacity is what the renderer allocated its buffers against, so
     * an emitter added afterwards shares the particles the existing ones did not claim.
     * A `create` that would need more than the pool holds is refused and counted the way
     * an over-budget spawn already is -- 0.9's policy, applied to emitters.
     *
     * Spawning an *effect* -- a pooled one-shot that releases its own slot when its last
     * particle dies -- is C3 and is built on this.
     */
    EmitterId create(ParticleEmitter emitter);

    /**
     * @brief Spawn a one-shot effect at a point (C3).
     *
     * The call the roadmap's Part 1 writes as `scene.spawnEffect(effects::impact, hit.point,
     * hit.normal)`, and the reason `RayHit` carries a normal at all. `effect` is the same
     * `ParticleEmitter` a glTF authors -- there is no `Effect` type and no `Emitter`
     * component, because the data already exists and a second description of it would be
     * two things to keep in step.
     *
     * `position` and `normal` replace the emitter's transform: the emitter's local +Y is
     * aimed along `normal`, so an authored upward spray becomes a spray off the surface
     * that was hit. A zero normal leaves the effect unrotated.
     *
     * `effect.burst` is forced non-zero if the caller left it at zero -- a continuous
     * emitter spawned at a point would run forever, which is the one thing a fire-and-
     * forget call must not do.
     *
     * @return the emitter's handle, which a caller may keep to cancel it early and may
     *         equally throw away. It goes stale by itself when the last particle dies.
     */
    EmitterId spawnEffect(ParticleEmitter effect, const glm::vec3& position, const glm::vec3& normal = {});

    /// Retire an emitter. It stops spawning immediately; particles already in flight live
    /// out their lifetimes, because killing them would be a visible pop and the pool
    /// reclaims them anyway when they expire.
    void destroy(EmitterId id);

    [[nodiscard]] bool valid(EmitterId id) const {
        return id.valid() && id.index < slots.size() && slots[id.index].generation == id.generation &&
               slots[id.index].live;
    }

    /// The handle occupying a slot. Invalid for a retired one. Slot order is the order
    /// `emitters()` and `writeGpuEmitters` use, so a walker pairs the two.
    [[nodiscard]] EmitterId emitterAt(uint32_t slot) const {
        if (slot >= slots.size() || !slots[slot].live) return {};
        return EmitterId{slot, slots[slot].generation};
    }

    /**
     * @brief Retire expired slots, then emit this frame's particles.
     *
     * `dt` is a fixed step in every caller and has to be: a particle count that depends
     * on the frame rate is a particle count that differs between runs, which is the same
     * argument the animation step makes in `Engine`.
     */
    void update(float dt);

    /// Slots the pool holds. Zero for a scene with no emitters, which is what makes the
    /// whole subsystem cost nothing at all in Sponza rather than cost an empty dispatch.
    [[nodiscard]] uint32_t capacity() const { return poolCapacity; }

    /// What the emitters currently running need, which is what `capacity()` is grown to.
    /// Live emitters only: a retired slot keeps its record until something overwrites it,
    /// and counting those would hold the pool at every effect that ever ran.
    [[nodiscard]] uint32_t wantedCapacity() const;

    /**
     * @brief Enlarge the pool to hold at least `atLeast` particles, keeping the ones in
     *        flight. False when it was already big enough, or is at `kMaxCapacity`.
     *
     * **Not a call a game makes.** The GPU half of the pool belongs to the renderer, which
     * sized its buffers from `capacity()`, so growing this alone emits into storage the
     * device does not have. `Engine::growParticles` is the pair, and it is the only caller.
     */
    bool grow(uint32_t atLeast);
    /// Live particles after the most recent update(). Exactly the instance count the
    /// draw submits, because the sort puts every dead slot after every live one.
    [[nodiscard]] uint32_t aliveCount() const { return alive; }
    /// Particles an emitter asked for and the pool had no room for, since the last
    /// setEmitters(). Reported by the renderer when it changes -- a budget that
    /// truncates in silence is the thing 0.9 exists to forbid.
    [[nodiscard]] uint32_t droppedSpawns() const { return dropped; }
    /// True when the emitter list is empty, so every caller has one thing to test.
    [[nodiscard]] bool empty() const { return emitterList.empty(); }

    /// This frame's births, in ascending slot order.
    [[nodiscard]] const std::vector<GpuSpawn>& spawns() const { return spawnList; }

    [[nodiscard]] const std::vector<ParticleEmitter>& emitters() const { return emitterList; }
    /// Mutable, so an animated node can push a new transform in. The only field a caller
    /// is expected to touch between frames -- changing `rate` or `lifetime` after
    /// setEmitters() would invalidate the capacity they were sized from.
    [[nodiscard]] std::vector<ParticleEmitter>& emitters() { return emitterList; }

    /// Write every emitter into `out` in the shaders' layout. `out` must hold
    /// `emitters().size()` entries.
    void writeGpuEmitters(GpuEmitter* out) const;

    /// Simulated time after the most recent update(). The shaders take it as a push
    /// constant and run the same death comparison against it that update() ran.
    [[nodiscard]] float time() const { return now; }
    /// The step update() was last called with, which is what the integrator in
    /// `particle_simulate.comp` advances by. Read off the system rather than passed to
    /// the renderer separately, so there is one place the fixed step is decided.
    [[nodiscard]] float step() const { return lastStep; }

    /**
     * @brief Slots the steady state of `emitters` needs, before any budget is applied.
     *
     * `rate x maxLifetime` summed over the emitters *is* the population an emitter
     * sustains -- births per second times seconds alive -- so this is a derivation
     * rather than a guess, and it is what "sized from data" means for a pool. Rounded up
     * to a power of two because the bitonic sort's domain must be one, and given one
     * spare particle per emitter because a rate below one per frame still emits.
     */
    [[nodiscard]] static uint32_t requiredCapacity(const std::vector<ParticleEmitter>& emitters);

  private:
    /// The lifetime pair per emitter (C1), parallel to `emitterList` rather than inside
    /// `ParticleEmitter`: that struct is what the glTF authors and what
    /// `writeGpuEmitters` packs, and a generation counter is neither authored nor
    /// uploaded. `InstanceTable` keeps its generations in a side array for the same
    /// reason.
    struct Slot {
        uint32_t generation = 1;
        bool live = true;
        /// One-shots only. The time the last particle this emitter will ever spawn dies,
        /// after which `update` releases the slot. Negative for a continuous emitter,
        /// which never self-destructs.
        float expiresAt = -1.0f;
        /// Set once the burst has been emitted, so it is not emitted again next frame.
        bool burstDone = false;
    };
    std::vector<Slot> slots;

    std::vector<ParticleEmitter> emitterList;
    /// Simulated time, advanced by update(). A particle in slot `i` is alive while
    /// `deathTime[i] > now`.
    std::vector<float> deathTime;
    /// Rebuilt by the scan at the top of every update(), in ascending slot order. That
    /// order is the whole reason the pool is allocated here -- see the file comment.
    std::vector<uint32_t> freeSlots;
    std::vector<GpuSpawn> spawnList;

    uint32_t poolCapacity = 0;
    uint32_t alive = 0;
    uint32_t dropped = 0;
    float now = 0.0f;
    float lastStep = 0.0f;
};

/**
 * @brief Read every `nodes[i].extras.substrate_emitter` out of a glTF document.
 *
 * Takes the document's bytes rather than a path or a parsed asset, and that is what
 * makes it testable without a device, a file or fastgltf. `.glb` is handled by
 * unwrapping its JSON chunk, so a caller does not have to know which it has.
 *
 * The transform is left at identity and `node` carries the node index: placing an
 * emitter is the *scene's* job, because the same emitter under two nodes is two
 * emitters and only the node knows where each is. `GltfScene::load` does it in the same
 * walk that places a light.
 *
 * ## Why this parses the document a second time
 *
 * fastgltf reaches extras through a callback handing out a `simdjson::dom::object`,
 * which would put simdjson's amalgamated header -- a file fastgltf downloads into its
 * own `deps/` at configure time -- on this engine's include path. A targeted rapidjson
 * pass costs a few milliseconds at load, depends on nothing that is not already a
 * dependency, and keeps the emitter schema in a hosted translation unit the unit suite
 * can reach. The extras stay in the standard place; only the reader is ours.
 *
 * @return false when the bytes are not a glTF document at all. A document with no
 *         emitter in it is not a failure, it is Sponza.
 */
[[nodiscard]] bool parseSceneEmitters(const rapidjson::Value& nodesArray, std::vector<ParticleEmitter>& out);

} // namespace scene
