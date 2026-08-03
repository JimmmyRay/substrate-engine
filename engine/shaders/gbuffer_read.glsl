/**
 * The deferred G-buffer as it is read back, and the test for whether a pixel's samples
 * came from one fragment.
 *
 * The write side is gbuffer_contract.glsl; this is what the passes downstream of it bind.
 * Two of them include this file -- lighting_body.glsl and shadowmask.frag -- and they have
 * to agree about what "the same fragment" means to the bit: the mask pass traces one
 * shadow ray per distinct fragment in a pixel and the lighting pass indexes the result per
 * sample, so a disagreement would hand a sample a bit that was traced for a different
 * surface.
 *
 * `GBUFFER_MULTISAMPLE` selects the sampler type. A 1x G-buffer creates genuinely
 * single-sampled images, and a single-sample image cannot be bound to a `sampler2DMS`
 * descriptor -- which is why the 1x lighting variant is a separate file rather than the
 * multisample one specialised to N=1.
 */

#ifdef GBUFFER_MULTISAMPLE
layout(set = 1, binding = 0) uniform sampler2DMS gAlbedo;
layout(set = 1, binding = 1) uniform sampler2DMS gNormal;
layout(set = 1, binding = 2) uniform sampler2DMS gOrm;
layout(set = 1, binding = 3) uniform sampler2DMS gDepth;
layout(set = 1, binding = 4) uniform sampler2DMS gEmissive;
#define GFETCH(tex, coord, s) texelFetch(tex, coord, s)
#else
layout(set = 1, binding = 0) uniform sampler2D gAlbedo;
layout(set = 1, binding = 1) uniform sampler2D gNormal;
layout(set = 1, binding = 2) uniform sampler2D gOrm;
layout(set = 1, binding = 3) uniform sampler2D gDepth;
layout(set = 1, binding = 4) uniform sampler2D gEmissive;
#define GFETCH(tex, coord, s) texelFetch(tex, coord, 0)
#endif

/// How far two samples' G-buffer values may differ and still be treated as one
/// fragment. Octahedral units for the normal, which at this magnitude is about a third
/// of a degree; per-channel linear albedo, about 1/255.
///
/// Not zero, and not much above it. An *exact* test flags every internal triangle
/// edge in the mesh rather than just silhouettes, because a smooth-shaded surface
/// interpolates a slightly different normal on each side of a shared edge -- Sponza
/// is dense enough that it lights up as a full wireframe in the `edges` debug view.
/// A tolerance this small erases that without merging anything a viewer could tell
/// apart.
///
/// The value was swept against the per-sample reference image. 0.02 recovered another
/// 8% of the 8x lighting cost and took the worst pixel from 21/255 to 108/255 across
/// five times as many pixels, which is the wrong trade for a pass whose entire purpose
/// is edge quality. 0.005 costs what the exact test costs and differs from it by 113
/// pixels in a 1.44-megapixel frame.
const float kEdgeNormalTolerance = 0.005;
const float kEdgeAlbedoTolerance = 0.005;

/**
 * True when two samples of one pixel carry what shades as the same fragment.
 *
 * ## Why albedo and normal, and not depth
 *
 * Depth is the obvious edge signal and is the wrong one here: it is interpolated at
 * each *sample* position rather than broadcast, so it differs slightly across the
 * samples of a single flat triangle and would classify the entire screen as an edge.
 * The broadcast channels are exactly the ones that identify a fragment --
 * `sampleShadingEnable` is false, so the G-buffer pass runs once per pixel and
 * broadcasts its result to every sample that fragment covers.
 *
 * A relative depth test was tried as a third criterion and removed. The theory was
 * that where the floor meets the far archway a pixel spans a long stretch of a nearly
 * edge-on surface, whose samples agree on normal and albedo while sitting at very
 * different distances. It made no difference -- 9198 differing pixels against 9213
 * without it, the same worst pixel at the same coordinate -- and added 3% to the 8x
 * lighting cost. A criterion that costs and does not pay is not worth keeping on the
 * strength of a plausible story.
 *
 * **This is the whole definition of "the same fragment" in the renderer.** Hybrid MSAA
 * collapses a pixel to one shading evaluation on it, and the shadow mask shares one
 * traced ray on it; both inherit whatever it decides, including that samples of one
 * fragment sit at slightly different depths and so at slightly different world
 * positions.
 */
bool sampleMatches(ivec2 coord, int a, int b) {
#ifdef GBUFFER_MULTISAMPLE
    if (any(greaterThan(abs(GFETCH(gNormal, coord, a).xy - GFETCH(gNormal, coord, b).xy),
                        vec2(kEdgeNormalTolerance)))) {
        return false;
    }

    vec4 ca = GFETCH(gAlbedo, coord, a);
    vec4 cb = GFETCH(gAlbedo, coord, b);
    // Alpha exactly, RGB within tolerance. The G-buffer pass writes alpha 1.0 and
    // the clear leaves 0.0, so this one exact comparison is what stops a pixel on
    // a silhouette against the sky from collapsing into the surface and dropping
    // the sky's contribution -- a dark surface facing +Z encodes to a normal near
    // (0,0), which is exactly what the cleared samples hold.
    if (ca.a != cb.a) return false;
    return !any(greaterThan(abs(ca.rgb - cb.rgb), vec3(kEdgeAlbedoTolerance)));
#else
    return true;
#endif
}

/**
 * Edge classification for hybrid MSAA (3.6).
 *
 * True when every sample in the pixel came from what shades as the same fragment. That
 * is the common case: two samples hold different albedo or normal only where two
 * fragments landed in one pixel, or where part of the pixel was never covered and still
 * holds the clear value.
 */
bool samplesAgree(ivec2 coord) {
#ifdef GBUFFER_MULTISAMPLE
    for (uint s = 1u; s < SAMPLE_COUNT; ++s) {
        if (!sampleMatches(coord, int(s), 0)) return false;
    }
    return true;
#else
    return true;
#endif
}
