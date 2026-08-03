#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "instance.glsl"

// Depth-only pass for both shadow maps. The sun has one scene-fitted projection and needs
// no index; an atlas layer names one of `shadowMatrices`. One field decides which, and
// the index is meaningful only in the second case.
//
// This is the one push constant block left in the scene passes, and it survives for the
// reason 0.11 removed the others -- it is per *pass*, not per draw. One
// vkCmdPushConstants before one vkCmdDrawIndexedIndirect covers a whole layer.

// Position and UV only. The normal and tangent at locations 1 and 2 are in the vertex
// buffer and are not read here -- a depth pass has no lighting to do, and the UV survives
// solely so shadow.frag can discard alpha-masked texels. Declaring the two it does not
// use would make the pipeline provide them for nothing, and validation says so.
layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inUV;

layout(push_constant) uniform Push {
    uint matrixIndex;
    uint usePunctual;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) flat out uint vMaterial;

void main() {
    Instance inst = instances[gl_InstanceIndex];

    vUV = inUV;
    vMaterial = inst.meta.y;

    mat4 viewProj = pc.usePunctual != 0u ? shadowMatrices[pc.matrixIndex] : frame.sunViewProj;
    gl_Position = viewProj * inst.model * vec4(inPosition, 1.0);
}
