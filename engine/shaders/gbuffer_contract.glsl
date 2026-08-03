/**
 * The contract a G-buffer fragment shader honours, engine-authored or game-authored (G5).
 *
 * A shader variant is a fragment shader the renderer binds instead of `gbuffer.frag` for
 * the draws whose material names it. It is bound into a pipeline the *engine* built, with
 * a layout the engine wrote, into attachments the engine created -- so the freedom is in
 * what a surface looks like, and everything around that is fixed. This file is that
 * fixture, made includable: before G5 it existed only as the top forty lines of
 * `gbuffer.frag`, which a game shader could copy but not honour.
 *
 * Include it, write a `main` that fills a `Surface`, and hand it to `gbufferWrite`:
 *
 *     #version 450
 *     #extension GL_EXT_nonuniform_qualifier : require
 *     #extension GL_GOOGLE_include_directive : require
 *     #include "gbuffer_contract.glsl"
 *
 *     void main() {
 *         Material m = gbufferMaterial();
 *         Surface s;
 *         s.albedo    = m.baseColorFactor.rgb;
 *         s.normal    = normalize(vNormal);
 *         s.occlusion = 1.0;
 *         s.roughness = m.roughnessFactor;
 *         s.metallic  = m.metallicFactor;
 *         s.emissive  = vec3(0.0);
 *         gbufferWrite(s);
 *     }
 *
 * ## What is fixed, and why each one is
 *
 * - **The four attachments.** Albedo, an octahedrally packed normal, occlusion /
 *   roughness / metallic, and emissive. The deferred lighting pass reads exactly these
 *   and nothing else; a variant writing a fifth channel would have nothing read it.
 *   `gbufferWrite` is what encodes them, so the octahedral packing and the specular
 *   antialiasing are not four lines a game shader has to copy correctly.
 * - **The varyings.** World position, normal, tangent, UV and the material index, in
 *   these locations. A variant supplying its own vertex shader must write the same five;
 *   the default `gbuffer.vert` already does.
 * - **Set 1.** The material table at binding 0 and the bindless texture array at
 *   binding 1, as `GltfScene` builds them. `gbufferMaterial()` is this shader's own row
 *   of the first and `sampleOr` indexes the second, including the -1 that means "no
 *   texture, use the factor alone".
 * - **Set 0.** The frame uniforms, through `frame.glsl`, and the instance table through
 *   `instance.glsl` for a variant that brings its own vertex shader.
 *
 * ## Specialisation constants
 *
 * `constant_id` 0..7 belong to the engine; a variant's own constants start at 8, which is
 * where `ShaderVariant::constants` is supplied from. The engine uses one of its eight
 * here -- `ENABLE_GSAA` -- and `Renderer::verifyShaderBindings` aborts in Debug on a
 * variant that declares an id nothing supplies a value for, which is the failure a game
 * developer would otherwise meet as a surface shaded with the wrong constant's default.
 *
 * ## What a variant does *not* get
 *
 * The shadow and velocity passes keep the engine's vertex stage. A variant that only
 * shades differently is unaffected; one that *moves* vertices in its own `gbuffer.vert`
 * will cast an undisplaced shadow and write an undisplaced motion vector, because those
 * two passes are not part of this contract.
 */

#include "common.glsl"
#include "frame.glsl"
#include "octahedral.glsl"

/// Geometric specular antialiasing. The engine's own constant, applied by `gbufferWrite`
/// so every variant gets it without knowing it exists. `frame.params.z` remains the
/// strength; this decides whether the derivatives are taken at all.
layout(constant_id = 0) const bool ENABLE_GSAA = true;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) centroid in vec3 vNormal;
layout(location = 2) centroid in vec4 vTangent;
layout(location = 3) centroid in vec2 vUV;
layout(location = 4) flat in uint vMaterial;

layout(location = 0) out vec4 outAlbedo;
/// Two components, not four: the normal is octahedrally encoded into an RG16F target.
layout(location = 1) out vec2 outNormal;
layout(location = 2) out vec4 outOrm;
layout(location = 3) out vec4 outEmissive;

layout(set = 1, binding = 0) readonly buffer Materials {
    Material materials[];
};
layout(set = 1, binding = 1) uniform sampler2D textures[];

/// This draw's material row. `vMaterial` is `flat`, so the index is uniform across the
/// primitive and the fetch is a scalar load.
Material gbufferMaterial() {
    return materials[vMaterial];
}

/// A bindless texture, or `fallback` where glTF left the slot empty. -1 is the loader's
/// "no texture" and every material field that names one may carry it.
vec4 sampleOr(int index, vec2 uv, vec4 fallback) {
    if (index < 0) return fallback;
    return texture(textures[nonuniformEXT(index)], uv);
}

/**
 * What a G-buffer shader produces, before it is packed.
 *
 * Plain, unencoded, world-space values -- the normal is a unit vector rather than two
 * octahedral components, and roughness is the material's rather than the antialiased
 * one. Both of those conversions are `gbufferWrite`'s job.
 */
struct Surface {
    vec3 albedo;
    vec3 normal; ///< world space, unit length
    float occlusion;
    float roughness;
    float metallic;
    vec3 emissive;
};

/// Perturb `normal` by a tangent-space normal map sample, given the interpolated tangent
/// frame. Here rather than in each variant because getting the Gram-Schmidt
/// reorthogonalisation and the handedness in `vTangent.w` wrong is a bug that looks like
/// bad art rather than like broken code.
vec3 gbufferNormalMap(vec3 normal, vec3 sampled, float scale) {
    vec3 tangent = normalize(vTangent.xyz - normal * dot(normal, vTangent.xyz));
    vec3 bitangent = cross(normal, tangent) * vTangent.w;
    sampled = sampled * 2.0 - 1.0;
    sampled.xy *= scale;
    return normalize(mat3(tangent, bitangent, normal) * sampled);
}

/**
 * Encode a surface into the four attachments.
 *
 * Geometric specular antialiasing (Kaplanyan) happens here. MSAA fixes geometric edges
 * only: a high-frequency normal map still produces a specular lobe that changes faster
 * than one sample per pixel can track, and that shows up as crawling sparkle on Sponza's
 * stonework whenever the camera moves. The fix is to widen the lobe to cover the normal
 * variation inside the pixel, and doing it here rather than in the lighting pass is what
 * makes it free: the true shaded normal is in hand with usable derivatives, and the
 * result is baked into the roughness the G-buffer already stores.
 *
 * Must be called from uniform control flow -- the derivatives are undefined otherwise,
 * which is the usual rule for `dFdx` and the reason a variant discards *before* it
 * builds its surface rather than around this call.
 */
void gbufferWrite(Surface s) {
    float roughness = s.roughness;
    if (ENABLE_GSAA) {
        float gsaa = frame.params.z;
        vec3 dndx = dFdx(s.normal);
        vec3 dndy = dFdy(s.normal);
        float variance = gsaa * (dot(dndx, dndx) + dot(dndy, dndy));
        // Squared roughness is the GGX alpha; variance adds in that space, not in
        // roughness space, which is why this squares and then takes the root back.
        float alpha = roughness * roughness;
        roughness = sqrt(clamp(alpha + min(2.0 * variance, 0.18), 0.0, 1.0));
    }

    outAlbedo = vec4(s.albedo, 1.0);
    outNormal = octEncode(s.normal);
    outOrm = vec4(s.occlusion, roughness, s.metallic, 1.0);
    outEmissive = vec4(s.emissive, 1.0);
}
