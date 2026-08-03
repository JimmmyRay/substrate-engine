#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

// The engine's own G-buffer shader, and variant 0 -- so it is the first consumer of the
// contract rather than an exception to it. Everything it used to declare for itself, a
// game shader now includes from the same file.
#include "gbuffer_contract.glsl"

void main() {
    Material m = gbufferMaterial();

    vec4 base = m.baseColorFactor * sampleOr(m.baseColorTexture, vUV, vec4(1.0));

    // Sponza merges foliage alpha into baseColor, so the cutout happens here.
    if (m.alphaMask != 0u && base.a < m.alphaCutoff) discard;

    Surface s;
    s.normal = normalize(vNormal);
    if (m.normalTexture >= 0) {
        s.normal = gbufferNormalMap(s.normal, sampleOr(m.normalTexture, vUV, vec4(0.5, 0.5, 1.0, 1.0)).xyz,
                                    m.normalScale);
    }

    // glTF packs occlusion/roughness/metallic into R/G/B of one texture.
    vec3 orm = sampleOr(m.metallicRoughnessTexture, vUV, vec4(1.0)).rgb;

    s.albedo = base.rgb;
    s.roughness = orm.g * m.roughnessFactor;
    s.metallic = orm.b * m.metallicFactor;
    s.occlusion = m.occlusionTexture >= 0 ? sampleOr(m.occlusionTexture, vUV, vec4(1.0)).r : 1.0;
    s.emissive = m.emissiveFactor.rgb * sampleOr(m.emissiveTexture, vUV, vec4(1.0)).rgb;

    gbufferWrite(s);
}
