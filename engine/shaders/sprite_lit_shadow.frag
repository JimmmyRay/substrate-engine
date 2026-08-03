#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

/**
 * The cutout a lit sprite casts into the shadow map (P6).
 *
 * `shadow.frag`'s job for a different texture array. The engine's own cuts its silhouette
 * out of set 1, which holds what a glTF brought; a sprite's image is in set 2, the array a
 * game loaded through `e.images()`, and its rect is in texels on the material. Without this
 * every lit sprite would cast a solid rectangle -- the same failure `shadow.frag`'s own
 * header records for Sponza's foliage, one subsystem along.
 *
 * The shadow pass keeps the engine's vertex stage, which writes the UV and the material
 * index and nothing else, so this reads exactly what the engine's does.
 */

#include "common.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 1) flat in uint vMaterial;

layout(set = 1, binding = 0) readonly buffer Materials {
    Material materials[];
};
layout(set = 2, binding = 0) uniform sampler2D gameImages[];

void main() {
    Material m = materials[vMaterial];

    // Emissive geometry casts no shadow, exactly as `shadow.frag` decides it and for the
    // same reason: the two raster paths must not disagree about which meshes occlude.
    if (any(greaterThan(m.emissiveFactor.rgb, vec3(0.0)))) discard;

    vec2 size = vec2(textureSize(gameImages[nonuniformEXT(m.gameImage)], 0));
    vec2 rectMin = m.params.xy;
    vec2 rectSize = m.params.zw;
    if (rectSize.x <= 0.0) {
        rectMin.x = 0.0;
        rectSize.x = size.x;
    }
    if (rectSize.y <= 0.0) {
        rectMin.y = 0.0;
        rectSize.y = size.y;
    }

    float alpha = m.baseColorFactor.a * texture(gameImages[nonuniformEXT(m.gameImage)],
                                                (rectMin + vUV * rectSize) / size).a;
    if (alpha < m.alphaCutoff) discard;
}
