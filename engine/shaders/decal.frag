#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

/**
 * Deferred decals (3.3).
 *
 * A fullscreen pass per decal, not a projected box. Reads the depth buffer, reconstructs
 * the world position of whatever is already there, transforms it into the decal's unit
 * cube and discards outside it -- so the decal lands on the geometry rather than in
 * front of it, which is the whole point of projecting into the G-buffer instead of
 * drawing a quad.
 *
 * ## Fullscreen rather than a box
 *
 * The usual implementation rasterises the decal volume so only the pixels it could
 * possibly cover run the shader. That is the right optimisation at hundreds of decals
 * and pure cost at the handful a config file places: it needs a cube mesh, front/back
 * face selection depending on whether the camera is inside the volume, and depth-test
 * state that differs between those two cases. The rejection below is four comparisons.
 * When something places decals in bulk, this becomes a box; until then it is a `discard`.
 *
 * ## The MSAA limitation
 *
 * The pass writes a multisampled attachment without sample shading, so every sample in a
 * pixel receives the same decal colour. Along a silhouette that crosses a decal, both
 * surfaces get it. Decals are large and low-frequency, so this is invisible in practice
 * -- but it is a real difference from a per-sample projection, and it is why this reads
 * the *resolved* depth: doing it per-sample would need the same 1x/MSAA shader split the
 * lighting pass carries, for an artefact nobody has seen yet.
 */

#include "common.glsl"
#include "frame.glsl"

layout(set = 1, binding = 0) readonly buffer Materials {
    Material materials[];
};
layout(set = 1, binding = 1) uniform sampler2D textures[];

/// Single-sample scene depth: the resolve at MSAA, gDepth itself at 1x.
layout(set = 2, binding = 0) uniform sampler2D sceneDepth;

layout(push_constant) uniform Push {
    /// World -> decal space. The decal occupies [-0.5, 0.5] on every axis and projects
    /// along its local Y, so the texture is addressed by local XZ.
    mat4 worldToDecal;
    vec4 tint;
    uint textureIndex;
    /// Fraction of the half-extent over which alpha ramps to zero at the boundary.
    /// Without it a decal ends on a straight line that no surface explains.
    float edgeFade;
    /// Non-zero projects the disc inscribed in the footprint. See `gfx::Decal::round`.
    uint round;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outAlbedo;

void main() {
    float depth = texture(sceneDepth, vUV).r;
    // Reverse-Z: 0 is the far plane, so this pixel is sky and there is nothing to
    // project onto.
    if (depth <= 0.0) discard;

    vec3 local = (pc.worldToDecal * vec4(worldFromDepth(vUV, depth), 1.0)).xyz;

    vec3 outside = abs(local) - vec3(0.5);
    if (any(greaterThan(outside, vec3(0.0)))) discard;

    vec4 texel = texture(textures[nonuniformEXT(pc.textureIndex)], local.xz + 0.5) * pc.tint;

    // Fade toward every face of the volume, not just the projection axis: a decal
    // clipped flat at its side walls reads as a decal, and one that fades reads as a
    // stain.
    vec3 fade = clamp(-outside / max(pc.edgeFade * 0.5, 1e-4), 0.0, 1.0);
    float alpha = fade.x * fade.y * fade.z;

    if (pc.round != 0u) {
        // The two lateral terms replaced by one radial one, and the projection term kept:
        // this narrows the footprint and not the depth, so a disc on a thin wall still
        // stops at the far side. `r` is 0 at the centre and 1 on the inscribed circle,
        // faded over the same fraction of the half-extent the square edges use.
        float r = length(local.xz) * 2.0;
        alpha = fade.y * clamp((1.0 - r) / max(pc.edgeFade, 1e-4), 0.0, 1.0);
    }

    texel.a *= alpha;
    if (texel.a <= 0.0) discard;

    outAlbedo = texel;
}
