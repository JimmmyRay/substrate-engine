// The particle pool as every stage sees it (S3.2, S3.3).
//
// Must match GpuParticle, GpuEmitter and GpuSpawn in engine/scene/ParticleSystem.h
// exactly. Set 1 in both the compute and the graphics pipeline layouts, so a shader
// that only reads (particle.vert) and one that writes (particle_simulate.comp) name
// the same bindings.
//
// The pool is *persistent*: unlike every other buffer in set 1's neighbourhood it is
// not per-frame-in-flight, because a particle's state at frame N is its state at frame
// N-1 integrated once. Two frames in flight write it in submission order on one queue,
// which is exactly the order the simulation wants.

struct Particle {
    vec4 position; // xyz world, w birth time on the simulation clock
    vec4 velocity; // xyz world, w lifetime in seconds; 0 marks a dead slot
    /// Lit radiance, premultiplied and ready to blend -- see particle_light.glsl for
    /// why the lighting happens once per particle here rather than once per fragment.
    vec4 color;
    uvec4 meta; // x emitter, y seed, z and w spare
};

struct Emitter {
    mat4 transform;
    vec4 velocity;   // xyz emitter-space initial velocity, w speed jitter
    vec4 boxExtent;  // xyz spawn-box half-extents, w cone half-angle in radians
    vec4 gravity;    // xyz world, w drag per second
    vec4 colorStart;
    vec4 colorEnd;
    vec4 params;     // x sizeStart, y sizeEnd, z restitution, w emissive intensity
    vec4 sprite;     // x spin in rad/s, y erosion, z flipbook loops per lifetime, w spare
    uvec4 flags;     // x texture index, y EMITTER_* bits, z flipbook cols | rows << 16, w spare
};

struct Spawn {
    uvec4 meta;  // x slot, y emitter, z seed, w spare
    vec4 params; // x lifetime, y birth time on the simulation clock
};

layout(std430, set = 1, binding = 0) buffer Particles {
    Particle particles[];
};
/// One key per pool slot, dense. `particle_sort.comp` permutes this array and nothing
/// else: the pool itself never moves, which is what keeps a slot index meaningful to
/// the CPU that allocated it.
layout(std430, set = 1, binding = 1) buffer SortKeys {
    uint sortKeys[];
};
layout(std430, set = 1, binding = 2) readonly buffer Emitters {
    Emitter emitters[];
};
layout(std430, set = 1, binding = 3) readonly buffer Spawns {
    Spawn spawns[];
};

#define EMITTER_EMISSIVE 1u
#define EMITTER_COLLIDES 2u

/// Sorts after every live particle, whatever its depth. All ones rather than a flag
/// bit, so the comparison the sort already does is the whole test.
#define PARTICLE_DEAD_KEY 0xFFFFFFFFu

/// Bit-identical to `particleHash` in ParticleSystem.h. The CPU picks a particle's
/// lifetime from its seed and the GPU picks the direction from the same seed, so the
/// two must agree exactly -- integer ops only, no floats anywhere in the mix.
uint particleHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// [0, 1) from a seed and a stream index. 24 bits, which is what a float holds exactly.
float particleRandom(uint seed, uint stream) {
    uint h = particleHash(seed * 747796405u + stream * 2891336453u);
    return float(h >> 8) * (1.0 / 16777216.0);
}

// Streams, matching ParticleRandomStream in ParticleSystem.h. Stream 0 belongs to the
// CPU's lifetime draw and is deliberately absent here: two consumers drawing the same
// number for different purposes is the failure mode a stream index exists to prevent.
#define RANDOM_POS_X 1u
#define RANDOM_POS_Y 2u
#define RANDOM_POS_Z 3u
#define RANDOM_CONE_U 4u
#define RANDOM_CONE_V 5u
#define RANDOM_SPEED 6u
#define RANDOM_SPIN_PHASE 7u
#define RANDOM_SPIN_RATE 8u
#define RANDOM_FRAME_START 9u

/**
 * Pack a particle into one sortable 32-bit word: quantised distance in the high bits,
 * slot in the low ones.
 *
 * One word rather than a key array beside an index array, because the compare-exchange
 * is then a single load, a single compare and a single store per element -- and the tie
 * between two particles at the same quantised distance breaks on the slot, which is
 * deterministic. `indexBits` is log2(capacity), so the depth keeps every bit the pool
 * does not need: 20 bits at 4096 particles, 16 at the 65536 ceiling. That ceiling is
 * where it is *because* of this packing, and ParticleSystem::kMaxCapacity says so.
 *
 * Far-to-near, because that is the order premultiplied blending needs. The quantised
 * value is inverted rather than the comparison, so the sort is a plain ascending one
 * and the dead key is simply the largest value there is.
 */
uint particleSortKey(float distance, float range, uint slot, uint indexBits) {
    uint depthBits = 32u - indexBits;
    // Minus two rather than minus one: the all-ones key means "dead", and a particle
    // sitting exactly on the camera must not be able to spell it.
    float maxDepth = float((1u << depthBits) - 2u);
    float t = clamp(distance / max(range, 1e-4), 0.0, 1.0);
    uint quantised = uint((1.0 - t) * maxDepth);
    return (quantised << indexBits) | (slot & ((1u << indexBits) - 1u));
}

/// Normalised age, 0 at birth and 1 at death. Every curve in an emitter is a function
/// of it. `now` is passed rather than stored on the particle for the reason
/// ParticleSystem.h gives: an age the GPU accumulated would drift from the death the
/// CPU predicted.
float particleLife(Particle p, float now) {
    return clamp((now - p.position.w) / max(p.velocity.w, 1e-6), 0.0, 1.0);
}
