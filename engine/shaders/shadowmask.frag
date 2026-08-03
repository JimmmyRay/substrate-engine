// 460 for the same reason lighting_rt.frag is: glslang only recognises GL_EXT_ray_query
// at GLSL 4.60, and the failure is an undeclared-identifier error on `rayQueryEXT`.
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require

/**
 * Traced shadow visibility, once per distinct fragment, for the pixels the deferred
 * resolve shades per sample.
 *
 * ## What it is for
 *
 * `shadeSample` traces a ray per light, and the resolve loop calls it once per *sample* on
 * any pixel where `samplesAgree` is false -- so a silhouette pixel at 4x traced the same
 * lights four times, and at 8x eight times, for however many of those samples came from
 * one fragment. This pass traces each distinct fragment once and leaves a bit per light
 * per sample behind; the lighting pass reads the bit.
 *
 * ## Why it skips the collapsed pixels
 *
 * Where every sample in a pixel carries one fragment the lighting pass already shades once
 * and already traces once, so there is nothing to save and a mask entry would cost a store
 * and a load to say what one ray says. Skipping them is also what keeps this pass's
 * arithmetic -- a second reconstruction of P and N, a second light loop -- off four fifths
 * of the screen, and what keeps those pixels' shadow answers coming from the same
 * instructions they always came from.
 *
 * ## Fragment shader, not compute
 *
 * `vUV` is what `worldFromDepth` reconstructs from, and the ray origin is that
 * reconstruction plus a bias. Interpolating it exactly as the lighting pass does is the
 * only way to be sure the two agree on a grazing ray; a compute pass would recompute
 * `(coord + 0.5) / extent` in floating point and could land an ulp away, which for a hard
 * shadow is not an ulp of error but a flipped bit. There is no colour attachment -- the
 * output is the storage image at set 3.
 */

#define GBUFFER_MULTISAMPLE 1

#include "features.glsl"
#include "frame.glsl"
#include "octahedral.glsl"
#include "pbr.glsl"
#include "gbuffer_read.glsl"
#include "rayshadow.glsl"

/// The scene TLAS, at the number it holds for every consumer of this set. Declared here
/// rather than in rayshadow.glsl for the reason written there: the set index differs
/// between the passes that trace, so the handle is an argument and the binding is local.
layout(set = 2, binding = 2) uniform accelerationStructureEXT shadowTlas;

/// One `r32ui` per sample: layer `s` holds the mask for sample `s`. Set 3 here and set 4
/// in the lighting pass, which binds the IBL chain this pass has no use for.
layout(set = 3, binding = 0, r32ui) uniform writeonly uimage2DArray shadowMask;

/// The tile light bits (C35), at set 4 here and set 5 there for the same reason. Read
/// rather than ignored because the loop below is the lighting pass's loop: a light this
/// pixel's tile cannot reach is a ray whose answer would be multiplied by zero, and the
/// whole point of this pass is the ray count.
layout(set = 4, binding = 0) readonly buffer LightTiles {
    uint lightTiles[];
};
#include "light_tile.glsl"

layout(location = 0) in vec2 vUV;

/**
 * The shadow bits for one sample: 1 where the light reaches it.
 *
 * **The three early-outs are the light loop's, in its order, and they are not an
 * optimisation copied for speed.** What this pass exists to reduce is the ray count, and
 * the loop it feeds only ever traced the lights that passed these; tracing the rest here
 * would add rays to a pass whose whole purpose is to remove them. A light that fails one
 * of them leaves its bit clear and the loop never reads it, because it stops at the same
 * test.
 */
uint traceMask(ivec2 coord, int s) {
    float depth = GFETCH(gDepth, coord, s).r;
    // Nothing was drawn here: the lighting pass returns the skybox for this sample and
    // reaches no light loop, so there is no bit to be wrong about.
    if (depth == FAR_DEPTH) return 0u;

    vec3 N = octDecode(GFETCH(gNormal, coord, s).xy);
    vec3 P = worldFromDepth(vUV, depth);

    int lightCount = min(int(frame.params.y), kShadowMaskLights);
    uint mask = 0u;

    // The same tile bits the lighting pass reads, walked the same way. A bit left clear
    // here for a light the tile cannot reach is never looked at: the loop that would read
    // it stops at the identical test.
    uint tileBase = lightTileBase(coord);
    int maskWords = lightMaskWords(lightCount);

    for (int w = 0; w < maskWords; ++w) {
        uint bits = LIGHT_TILE_WORD(tileBase, w);
        while (bits != 0u) {
            int i = w * 32 + findLSB(bits);
            bits &= bits - 1u;
            if (i >= lightCount) break;

            Light light = lights[i];

            vec3 L;
            vec3 radiance = lightRadiance(light, P, N, L);
            if (dot(radiance, radiance) <= 0.0) continue;
            if (dot(radiance, radiance) < frame.lightParams.x) continue;
            if (dot(N, L) <= 0.0) continue;

            if (lightShadow(shadowTlas, light, P, N, L) > 0.0) mask |= 1u << uint(i);
        }
    }

    return mask;
}

void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);

    // Exactly the pixels the resolve loop shades per sample, and the test has to be the
    // same one it uses -- both call `samplesAgree`, which is why that function moved into
    // gbuffer_read.glsl. A pixel this returns on is left holding whatever the last frame
    // wrote and is never read.
    if (ENABLE_EDGE_MSAA && samplesAgree(coord)) return;

    // Sample `s` reuses the first earlier sample that carries its fragment, and that
    // sharing *is* the saving: a silhouette pixel is usually two fragments across four
    // samples, so this is two traversals where the resolve loop paid four.
    //
    // **The shared ray is the first sample's, and the samples of one fragment do not sit
    // at one depth** -- depth is interpolated per sample where albedo and normal are
    // broadcast, so this answers all of them from the position of one of them. Sub-pixel,
    // and visible only where a shadow boundary crosses a silhouette pixel between two
    // samples of the same surface.
    //
    // **Carrying the masks forward is what makes the grouping well defined**, rather than
    // taking each sample's first match as an index and tracing there. That would need the
    // index to be a fixed point, and `sampleMatches` is a tolerance test rather than an
    // equivalence -- sample 2 can match sample 1 while sample 1 matches sample 0 and
    // sample 2 does not -- so a sample's answer has to be inherited, not re-derived.
    //
    // 8, not SAMPLE_COUNT: `render.msaaSamples` tops out there, and a fixed bound keeps
    // this an ordinary array rather than one sized by a specialisation constant.
    uint masks[8];

    for (uint s = 0u; s < SAMPLE_COUNT; ++s) {
        uint mask = 0u;
        bool shares = false;
        for (uint t = 0u; t < s; ++t) {
            if (sampleMatches(coord, int(s), int(t))) {
                mask = masks[t];
                shares = true;
                break;
            }
        }
        if (!shares) mask = traceMask(coord, int(s));

        masks[s] = mask;
        // Written for every sample, including the shared ones: the lighting pass indexes
        // this by sample and does not repeat the classification.
        imageStore(shadowMask, ivec3(coord, int(s)), uvec4(mask, 0u, 0u, 0u));
    }
}
