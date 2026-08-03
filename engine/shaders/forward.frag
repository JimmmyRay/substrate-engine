#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"
#include "features.glsl"
#include "frame.glsl"
#include "pbr.glsl"
#include "ibl.glsl"

/**
 * Alpha-blended surfaces, shaded forward.
 *
 * Blending is the one thing a deferred G-buffer cannot express: it stores exactly one
 * surface per pixel, and a translucent surface needs the one behind it as well. So
 * these primitives skip the G-buffer entirely and are shaded here, after the lighting
 * pass has already filled the HDR target with everything opaque.
 *
 * The light loop below is the same one in lighting_body.glsl -- a deliberate second
 * copy, not an oversight. The falloff maths and the cone ramp live in pbr.glsl; what
 * is left duplicated is the loop itself, which is six lines and differs in where it
 * gets its surface from. shadeRayHit in raytrace.glsl is the third copy, for the same
 * reason.
 *
 * Not carried over: the geometric specular antialiasing in gbuffer.frag. It is baked
 * into the roughness the G-buffer stores, and there is no G-buffer here. A blended
 * surface with a high-frequency normal map will therefore sparkle where an opaque one
 * no longer does.
 */

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 4) flat in uint vMaterial;

layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) readonly buffer Materials {
    Material materials[];
};
layout(set = 1, binding = 1) uniform sampler2D textures[];

vec4 sampleOr(int index, vec2 uv, vec4 fallback) {
    if (index < 0) return fallback;
    return texture(textures[nonuniformEXT(index)], uv);
}

void main() {
    Material m = materials[vMaterial];

    vec4 base = m.baseColorFactor * sampleOr(m.baseColorTexture, vUV, vec4(1.0));

    vec3 N = normalize(vNormal);
    if (m.normalTexture >= 0) {
        vec3 tangent = normalize(vTangent.xyz - N * dot(N, vTangent.xyz));
        vec3 bitangent = cross(N, tangent) * vTangent.w;
        vec3 sampled = sampleOr(m.normalTexture, vUV, vec4(0.5, 0.5, 1.0, 1.0)).xyz * 2.0 - 1.0;
        sampled.xy *= m.normalScale;
        N = normalize(mat3(tangent, bitangent, N) * sampled);
    }

    // A blended surface is drawn from one side but is physically two-sided, and glTF
    // does not require BLEND primitives to be wound consistently. Flip the normal
    // toward the viewer so a back-facing quad is not shaded as if it faced away.
    vec3 V = normalize(frame.cameraPos.xyz - vWorldPos);
    if (dot(N, V) < 0.0) N = -N;

    vec3 orm = sampleOr(m.metallicRoughnessTexture, vUV, vec4(1.0)).rgb;
    float occlusion = m.occlusionTexture >= 0 ? sampleOr(m.occlusionTexture, vUV, vec4(1.0)).r : 1.0;
    float roughness = clamp(orm.g * m.roughnessFactor, 0.04, 1.0);
    float metallic = clamp(orm.b * m.metallicFactor, 0.0, 1.0);

    vec3 diffuseColor = base.rgb * (1.0 - metallic);
    vec3 f0 = mix(vec3(0.04), base.rgb, metallic);

    vec3 color = vec3(0.0);

    int lightCount = int(frame.params.y);

    for (int i = 0; i < lightCount; ++i) {
        Light light = lights[i];

        vec3 L;
        vec3 radiance = lightRadiance(light, vWorldPos, N, L);
        if (dot(radiance, radiance) <= 0.0) continue;

        color += shadeLight(N, V, L, radiance, diffuseColor, f0, roughness);
    }

    // The same flat ambient the deferred path adds, so a blended surface and an opaque
    // one behind it are lifted out of black by the same amount. The environment term both
    // used to share went for being confidently wrong inside a building.
    color += constantAmbient(diffuseColor, occlusion);
    color += m.emissiveFactor.rgb * sampleOr(m.emissiveTexture, vUV, vec4(1.0)).rgb;

    // Straight alpha. The blend state multiplies by SRC_ALPHA, so the colour is
    // written unpremultiplied and the specular highlight fades out with the surface
    // -- which is wrong for real glass, where a reflection stays visible however
    // transparent the substrate is. Fixing that needs premultiplied output and dual
    // blend factors; it is not what "alpha-blended forward pass" means here.
    outColor = vec4(color, base.a);
}
