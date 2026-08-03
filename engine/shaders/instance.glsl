// The scene's instance table, as the shaders see it (4.1).
//
// Must match GpuInstance and GpuInstanceBounds in engine/scene/InstanceTable.h exactly.
// Both live in set 0 beside the frame uniforms: they are per-frame-in-flight buffers
// written by the CPU and read by every scene pass, which is what set 0 already means
// here.
//
// A vertex shader finds its own record with `instances[gl_InstanceIndex]`. That index
// is `firstInstance` from the indirect command plus the instance offset within it, so
// a one-instance draw reads exactly the slot the command named, and an N-instance
// draw reads N consecutive slots (4.5). No per-draw push constants are involved,
// which is the whole point of 0.11: the CPU records a pass, not a draw.

struct Instance {
    mat4 model;
    // Normal matrix rows. Stored rather than derived from model: inverse-transpose is
    // about thirty flops and this is a per-vertex shader.
    vec4 normal0;
    vec4 normal1;
    vec4 normal2;
    uvec4 meta; // x primitive, y material, z flags, w unused
};

layout(set = 0, binding = 3) readonly buffer Instances {
    Instance instances[];
};

// Last frame's model matrix, one per slot, for 3.4's motion correction. Its own buffer
// rather than a second mat4 inside `Instance`: the table splits its arrays by consumer,
// and this one has exactly one -- `velocity.vert`. Folding it in would have grown the
// record every vertex shader in the engine fetches from 128 bytes to 192, to carry a
// value all but one of them ignore, and would have cost the stride its power of two.
layout(set = 0, binding = 4) readonly buffer PrevInstances {
    mat4 prevInstances[];
};

#define INSTANCE_LIVE 1u
#define INSTANCE_BLENDED 2u
#define INSTANCE_DYNAMIC 4u
#define INSTANCE_VISIBLE 8u

mat3 instanceNormalMatrix(Instance i) {
    return mat3(i.normal0.xyz, i.normal1.xyz, i.normal2.xyz);
}
