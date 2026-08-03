#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// A depth-only pass could run with no fragment shader at all, and this one exists
// for exactly one reason: Sponza's foliage is alpha-masked. Without the cutout the
// plants cast solid rectangular shadows, which is more obviously wrong than having
// no shadows at all.
//
// Set 1 is the scene's material set, bound exactly as the G-buffer pass binds it, so
// the two pipelines share a layout shape and there is no second descriptor set to
// build or keep in step.

layout(location = 0) in vec2 vUV;
layout(location = 1) flat in uint vMaterial;

layout(set = 1, binding = 0) readonly buffer Materials {
    Material materials[];
};
layout(set = 1, binding = 1) uniform sampler2D textures[];

// No push constant block here. The vertex stage declares one, but this stage reads
// nothing from it, and a fragment stage that declares a block it does not use still
// forces the range into its stage flags for no reason.

void main() {
    Material m = materials[vMaterial];

    // Emissive geometry casts no shadow, which is the raster half of the rule
    // AccelStruct applies to the traced path by marking the same geometry non-opaque.
    // Both exist for one case: a point light at the centre of the emissive sphere that
    // represents it. The mesh encloses the light, every cube face of its shadow map
    // records that mesh at near-zero distance, and the light illuminates nothing at all
    // while its own shell throws a blob across the room.
    //
    // Deciding it by material rather than by a per-light key means no scene has to
    // author anything, and the two paths cannot disagree about which meshes occlude.
    if (any(greaterThan(m.emissiveFactor.rgb, vec3(0.0)))) discard;

    if (m.alphaMask == 0u) return;

    float alpha = m.baseColorFactor.a;
    if (m.baseColorTexture >= 0) alpha *= texture(textures[nonuniformEXT(m.baseColorTexture)], vUV).a;
    if (alpha < m.alphaCutoff) discard;
}
