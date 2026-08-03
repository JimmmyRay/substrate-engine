#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "instance.glsl"

/**
 * Vertex stage for the alpha-blended forward pass.
 *
 * Identical to gbuffer.vert, and deliberately not shared with it: the two will
 * diverge the moment either grows a stage the other does not want (skinning here,
 * velocity output there), and a `#define` switch between them would be harder to
 * read than two files that each do one thing.
 */

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;
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
