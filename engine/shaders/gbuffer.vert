#version 450
#extension GL_GOOGLE_include_directive : require

// The shared declaration, not a local copy of the first two fields. An abridged
// version silently aliases whatever follows -- the previous one named its second
// member cameraPos while the buffer actually holds invViewProj there, and it was
// only harmless because nothing read it.
#include "frame.glsl"
#include "instance.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

// centroid: on a partially covered pixel the centre may lie outside the triangle,
// and extrapolated UVs and normals show up as garbage along every MSAA edge.
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) centroid out vec3 vNormal;
layout(location = 2) centroid out vec4 vTangent;
layout(location = 3) centroid out vec2 vUV;
// Flat, and passed down rather than re-read in the fragment stage: an indirect draw
// has no push constants to carry it, and reading the instance record twice would put
// a dependent 128-byte fetch in front of every fragment.
layout(location = 4) flat out uint vMaterial;

void main() {
    Instance inst = instances[gl_InstanceIndex];

    vec4 world = inst.model * vec4(inPosition, 1.0);
    vWorldPos = world.xyz;

    mat3 normalMatrix = instanceNormalMatrix(inst);
    vNormal = normalize(normalMatrix * inNormal);
    vTangent = vec4(normalize(normalMatrix * inTangent.xyz), inTangent.w);
    vUV = inUV;
    vMaterial = inst.meta.y;

    gl_Position = frame.viewProj * world;
}
