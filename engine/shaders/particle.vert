#version 450
#extension GL_GOOGLE_include_directive : require

/**
 * Camera-facing billboards, six vertices per particle and no vertex buffer.
 *
 * `gl_InstanceIndex` walks the *sorted key array*, not the pool: entry n is the n-th
 * furthest live particle, and the draw submits exactly `aliveCount` instances, so the
 * dead tail the sort pushed to the end is never reached. The slot the key carries is
 * what indexes the pool -- particles never move, only their keys do.
 *
 * The colour arrives already lit and already premultiplied. See particle_light.glsl
 * for why that happens once per particle in a compute shader rather than here.
 */

#include "frame.glsl"
#include "particle.glsl"

/// Cell-local, [0, 1] over one flipbook frame rather than over the whole sheet. The
/// atlas arithmetic finishes in particle.frag because the half-texel inset that keeps a
/// frame from bleeding into its neighbour needs `textureSize`, and the sampler is bound
/// there.
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) flat out uint vTexture;
/// xy the cell's origin in the sheet, zw the cell's size. (0,0,1,1) is a plain texture.
layout(location = 3) flat out vec4 vFrameA;
/// xy the next cell's origin, z how much of it to mix in.
layout(location = 4) flat out vec3 vFrameB;
/// How much of the sprite's alpha is eaten away by now -- see `ParticleEmitter::erosion`.
layout(location = 5) flat out float vErode;

layout(push_constant) uniform Push {
    float now;      ///< simulated time, for the size curve
    uint indexBits; ///< log2(pool capacity)
    uint pad0;
    uint pad1;
} pc;

/// Two triangles, as a list. A strip would be four vertices and would need the pipeline
/// to carry a topology the rest of the engine does not use; two more vertices per
/// particle is cheaper than a second thing for `GraphicsPipelineDesc` to describe.
const vec2 kCorners[6] =
    vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    uint key = sortKeys[gl_InstanceIndex];

    // Belt and braces. The instance count comes from the CPU's own tally of live slots
    // and the dead keys sort past it, so this should never fire -- but "should never"
    // between two clocks is worth two lines, and a dead key's slot field is whatever
    // the all-ones pattern happens to name.
    if (key == PARTICLE_DEAD_KEY) {
        gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
        vUV = vec2(0.0);
        vColor = vec4(0.0);
        vTexture = 0xFFFFFFFFu;
        vFrameA = vec4(0.0, 0.0, 1.0, 1.0);
        vFrameB = vec3(0.0);
        vErode = 0.0;
        return;
    }

    uint slot = key & ((1u << pc.indexBits) - 1u);
    Particle p = particles[slot];
    Emitter e = emitters[p.meta.x];

    float life = particleLife(p, pc.now);
    float size = mix(e.params.x, e.params.y, life);

    // The screen axes in world space, straight out of the inverse view-projection: a
    // displacement of +1 in NDC x is `right`, and of +1 in NDC y is *downward*, because
    // Camera::projection negates Y for Vulkan's clip space. Negating it here is what
    // keeps the sprite the right way up, and it is a property of the projection rather
    // than of the camera, so there is nothing per-frame to keep in step.
    vec3 right = normalize((frame.invViewProj * vec4(1.0, 0.0, 0.0, 0.0)).xyz);
    vec3 up = -normalize((frame.invViewProj * vec4(0.0, 1.0, 0.0, 0.0)).xyz);

    vec2 corner = kCorners[gl_VertexIndex];

    // v runs down the sprite, which is the image convention every texture in this engine
    // is already loaded with. Taken from the *unrotated* corner, so the image is fixed to
    // the quad and the spin below turns the two together.
    vUV = vec2(corner.x, -corner.y) * 0.5 + 0.5;

    // Spin. A start angle and a signed rate, both from the particle's own seed, so no two
    // are in step -- which is the whole point: a field of identically-oriented sprites
    // reads as one repeated stamp however good the sprite is. Age rather than normalised
    // life, so the rate is genuinely radians per second and does not depend on how long
    // this particle happened to draw.
    if (e.sprite.x != 0.0) {
        float rate = (particleRandom(p.meta.y, RANDOM_SPIN_RATE) * 2.0 - 1.0) * e.sprite.x;
        float angle = particleRandom(p.meta.y, RANDOM_SPIN_PHASE) * 6.2831853 + rate * (pc.now - p.position.w);
        float s = sin(angle), c = cos(angle);
        corner = vec2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    }

    vec3 world = p.position.xyz + (right * corner.x + up * corner.y) * (size * 0.5);

    gl_Position = frame.viewProj * vec4(world, 1.0);
    vColor = p.color;
    vTexture = e.flags.x;
    vErode = e.sprite.y * life;

    // The flipbook. `sprite.z` passes over the sheet per lifetime -- usually a fraction of
    // one -- from a start cell the seed picks, wrapping. That wrap is why the sheets are
    // generated to loop seamlessly in time: a particle that starts on frame 11 crosses the
    // seam mid-life, and a sheet with a discontinuity there would pop.
    uint cols = max(e.flags.z & 0xFFFFu, 1u);
    uint rows = max(e.flags.z >> 16, 1u);
    uint frames = cols * rows;
    if (frames > 1u) {
        vec2 cell = vec2(1.0) / vec2(cols, rows);
        float t = fract(particleRandom(p.meta.y, RANDOM_FRAME_START) + life * e.sprite.z) * float(frames);
        uint a = uint(t) % frames;
        uint b = (a + 1u) % frames;
        vFrameA = vec4(vec2(a % cols, a / cols) * cell, cell);
        vFrameB = vec3(vec2(b % cols, b / cols) * cell, fract(t));
    } else {
        vFrameA = vec4(0.0, 0.0, 1.0, 1.0);
        vFrameB = vec3(0.0);
    }
}
