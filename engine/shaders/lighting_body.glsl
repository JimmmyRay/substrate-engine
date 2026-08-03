/**
 * Shared deferred-lighting body.
 *
 * Included by lighting.frag (multisampled G-buffer) and lighting1x.frag
 * (single-sampled). A 1x G-buffer creates genuinely single-sample images, and a
 * single-sample image cannot be bound to a sampler2DMS descriptor — so a real 1x
 * baseline needs its own variant rather than a multisample shader with N=1.
 *
 * The alternative, always allocating a multisampled G-buffer and shading one sample,
 * would measure shading cost but hide the memory and bandwidth difference, which is
 * exactly what the MSAA baseline is meant to expose.
 */

// SAMPLE_COUNT and the ENABLE_* toggles come from features.glsl, which every consumer
// of this file includes before it.

// The five G-buffer attachments, GFETCH, and samplesAgree. Shared with shadowmask.frag,
// which has to classify a pixel's fragments exactly the way the resolve loop below does.
#include "gbuffer_read.glsl"

/// The tile light bits light_tile.comp wrote for this frame (C35). **Set 5 here and
/// set 4 in shadowmask.frag**, for the reason the TLAS is declared per file rather than
/// in rayshadow.glsl: a set is addressed by the index its own pipeline layout gives it,
/// and the two pipelines bind different things around this one.
///
/// Bound and declared whether or not `render.lightTiles` is on -- a descriptor cannot
/// be specialised away, only the read can, which is the arrangement the AO buffer and the
/// shadow mask already have.
layout(set = 5, binding = 0) readonly buffer LightTiles {
    uint lightTiles[];
};
#include "light_tile.glsl"

// Screen-space occlusion, always single-sampled: it is one value per pixel regardless
// of how many samples the G-buffer holds, so it sits outside that file's sampler split.
layout(set = 1, binding = 5) uniform sampler2D ssaoMap;

#ifdef ENABLE_RAY_QUERY
/// The scene TLAS. Binding 2 is the number the acceleration structure has held since it
/// shared this set with the shadow maps at bindings 0 and 1. Declared here rather than in
/// rayshadow.glsl because the tracing compute passes bind the same structure at a
/// different set index and include the same header; see the note there.
layout(set = 2, binding = 2) uniform accelerationStructureEXT shadowTlas;

/// Per-sample traced shadow visibility, one bit per light, written by shadowmask.frag
/// before this pass runs. **Set 4 here and set 3 there**: a storage image is addressed by
/// the index its own pipeline layout gives the set, and the two pipelines bind different
/// things around it.
///
/// A storage image rather than a sampled one so it needs no sampler and no layout
/// transition -- it is written and read in GENERAL, and the only thing between the two is
/// a memory barrier.
layout(set = 4, binding = 0, r32ui) uniform readonly uimage2DArray shadowMask;
#endif

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Reverse-Z: the far plane is 0.

/// Direction from the camera through this pixel, for the skybox lookup.
vec3 viewRay(vec2 uv) {
    // **A parallel projection has no eye for rays to fan out from** (P3). Every ray is
    // the view axis, and the subtraction below would measure from `cameraPos` -- a
    // position the projection's rays do not pass through -- which spreads a hemisphere of
    // sky across a frame that should show one direction of it. `flags.w` is set from
    // `Camera::projectionMode`, and this is the only place in the renderer that asks:
    // culling tests clip-space inequalities, `worldFromDepth` goes through `invViewProj`,
    // the TAA jitter is a clip-space post-multiply, and the sun's box never took a camera.
    if (frame.flags.w != 0u) return frame.cameraForward.xyz;

    // Reverse-Z: the far plane is 0, so unprojecting there gives a point on the ray.
    vec4 world = frame.invViewProj * vec4(uv * 2.0 - 1.0, FAR_DEPTH, 1.0);
    return normalize(world.xyz / world.w - frame.cameraPos.xyz);
}

/**
 * One sample's radiance.
 *
 * `useMask` selects where traced shadow visibility comes from: false traces a ray per
 * light here, true reads the bit shadowmask.frag already traced for this sample. It is
 * a parameter rather than the specialisation constant itself because both are live in
 * one compiled shader -- the mask covers only the pixels the resolve loop shades per
 * sample, and the collapsed pixels below still trace inline.
 */
vec3 shadeSample(ivec2 coord, int sampleIndex, vec2 uv, float ssao, bool useMask) {
    float depth = GFETCH(gDepth, coord, sampleIndex).r;
    if (depth == FAR_DEPTH) return skyboxRadiance(viewRay(uv));

    vec4 albedo = GFETCH(gAlbedo, coord, sampleIndex);
    vec4 orm = GFETCH(gOrm, coord, sampleIndex);

    // Octahedrally encoded (3.5), and octDecode already normalises. There is no
    // zero-normal sentinel any more and there cannot be one: (0,0) is a legal
    // encoding, of +Z. The depth test above is what identifies an unwritten pixel,
    // which is what it should always have been -- the normal check was a second
    // answer to a question depth had already answered.
    vec3 N = octDecode(GFETCH(gNormal, coord, sampleIndex).xy);

    // The glTF occlusion texture and SSAO multiply rather than max(): they describe
    // different things -- one is baked small-scale detail the artist painted, the other
    // is the geometry actually in front of this pixel right now -- and a surface subject
    // to both is occluded by both. Their only consumer is the constant ambient below,
    // because occlusion is an ambient quantity and must never touch the analytic lights,
    // which carry their own visibility in a shadow ray. At ambient 0 both are computed and
    // multiplied into nothing.
    //
    // ## The asymmetry, and why it is affordable again
    //
    // Ambient is the one term a ray hit cannot compute the way the world does. SSAO is a
    // screen-space buffer and a hit has no screen position, so shadeRayHit reaches for the
    // baked occlusion texture instead -- which Sponza does not ship, making it exactly 1.0.
    // A reflected crease is therefore fractionally brighter than the world crease beside
    // it, and that is the one place the two ambients differ.
    //
    // This term was removed once, because that difference was not fractional: the world
    // darkened 3.5x where its reflection did not, and a mirror in a black room showed a
    // well-lit room. The diagnosis at the time was that a screen-space term cannot be made
    // symmetric with a traced one, and the supporting measurement was that a real
    // half-metre hemisphere trace returns ~1.0 where SSAO returned 0.3.
    //
    // **That measurement was the bug, not the justification.** ssao.comp compared depths
    // with the inequality reversed, so it returned ~0 on open surfaces and only stayed
    // bright at silhouettes -- an edge detector, mean 0.294 over the frame with an open
    // floor reading 0.021. Corrected, the same buffer means 0.958 with that floor at 1.000,
    // which is the ~1.0 the hemisphere trace had been reporting all along. The two numbers
    // never disagreed about the scene; one of them was inverted.
    //
    // Re-measured on reflect.gltf at an ambient high enough for this to matter, worst
    // region against the no-SSAO reference: world 0.955, mirror 1.000. A 4.5% divergence
    // where it was 63%, which is what "fractional" was supposed to mean.
    float occlusion = orm.r;
    float roughness = clamp(orm.g, 0.04, 1.0);
    float metallic = clamp(orm.b, 0.0, 1.0);

    vec3 P = worldFromDepth(uv, depth);
    vec3 V = normalize(frame.cameraPos.xyz - P);

    vec3 diffuseColor = albedo.rgb * (1.0 - metallic);
    vec3 f0 = mix(vec3(0.04), albedo.rgb, metallic);

    vec3 color = vec3(0.0);

    // Every light in one loop, sun included -- the sun is lights[0], so there is no
    // separate directional term to keep in step with this one.
    //
    // Which shadow answers depends on which file this was compiled into, and only that.
    // With ray query the loop traces a ray per light and covers every one of them; without
    // it, the map below covers the sun and the punctual lights go unshadowed. Ray
    // tracing is one switch, so a build can never be half of each.
    int lightCount = int(frame.params.y);

#ifdef ENABLE_RAY_QUERY
    // One load for every light this pixel sees, hoisted out of the loop below -- the
    // whole mask for this sample arrives in one 32-bit fetch, which is the shape that
    // makes reading it cheaper than tracing again.
    uint shadowBits = useMask ? imageLoad(shadowMask, ivec3(coord, sampleIndex)).r : 0u;
#endif

    // Tiled light assignment (C35). The bits of one tile, low bit first, which is
    // ascending light index -- the order the flat loop summed in, and the order this has
    // to keep summing in for the same set of lights to produce the same float. With
    // `render.lightTiles` off every bit is set and this walks 0..lightCount-1.
    uint tileBase = lightTileBase(coord);
    int maskWords = lightMaskWords(lightCount);

    for (int w = 0; w < maskWords; ++w) {
        uint bits = LIGHT_TILE_WORD(tileBase, w);
        while (bits != 0u) {
            int i = w * 32 + findLSB(bits);
            bits &= bits - 1u;
            // The all-ones fallback sets bits past the last light in the final word.
            // Nothing writes them when tiling is on.
            if (i >= lightCount) break;

            Light light = lights[i];

            vec3 L;
            vec3 radiance = lightRadiance(light, P, N, L);
            // Before the shadow ray rather than after: a light contributing nothing here is
            // out of range or below the horizon of this surface, and tracing to it would be
            // a ray whose answer gets multiplied by zero. This is what keeps the ray count
            // proportional to lights that actually reach the pixel rather than to lights in
            // the scene.
            if (dot(radiance, radiance) <= 0.0) continue;

            // `render.lightCutoff`, and unlike every other early-out in this loop it is an
            // **approximation**: it drops a light that does contribute. Kept separate from the
            // exact test above rather than folded into it, so the bit-exactness that line
            // claims stays a property of that line. At the row's default the comparand is 0.0
            // and `dot` is strictly positive by the line above, so nothing is dropped.
            //
            // **The unit is post-exposure radiance** -- `lightParams.x` is the row divided by
            // `frame.params.x` and squared -- so 0.004 means one 8-bit code value at the
            // tonemap and keeps that meaning under any `GameSetup::exposure`.
            //
            // **It does not bound the error it causes.** What is tested is arriving radiance;
            // what is dropped is `shadeLight`'s product, whose GGX peak runs to ~1e5 at the
            // roughness floor. Measured: 0.1 on Sponza moves a pixel by 143/255.
            if (dot(radiance, radiance) < frame.lightParams.x) continue;

            // The test `shadeLight` already applies at its first line, hoisted above the
            // shadow lookup. `lightRadiance` deliberately carries no cosine term -- it answers
            // "how much light arrives here", which is a property of the light and not of the
            // surface it lands on -- so a surface facing *away* from a light passes the
            // radiance test above, pays a full shadow lookup, and has the answer multiplied
            // by zero. That is roughly half of every light/surface pair in the scene.
            //
            // What it saves depends on which branch below was going to run: with ray query a
            // whole BVH traversal, without it nine PCF taps and a matrix multiply.
            //
            // Bit-exact, and checkably so rather than by argument: `shadeLight` returns
            // exactly vec3(0.0) when NoL <= 0, and adding +0.0 leaves every component of the
            // accumulator as it was. The golden set is what proves it -- if an image moves,
            // this is not the equivalence it claims to be.
            if (dot(N, L) <= 0.0) continue;

#ifdef ENABLE_RAY_QUERY
            // Hard shadows are exactly 0 or 1, so a fully occluded light skips the BRDF
            // rather than multiplying its result by zero. That stops being true the day these
            // grow a penumbra, at which point this becomes a multiply again.
            //
            // The same `lightShadow` call runs at reflection hits in shadeRayHit, which is
            // what keeps a surface and its own reflection from answering the same question
            // two different ways.
            //
            // A light past `kShadowMaskLights` has no bit and traces here however the mask is
            // set: the mask is one 32-bit word and `Renderer::lightBudget` is a number a game
            // states, so the two are not the same fact and the wider one degrades rather than
            // silently unshadowing everything above the 32nd light.
            if (ENABLE_RT_SHADOWS) {
                bool masked = useMask && i < kShadowMaskLights;
                bool lit = masked ? (shadowBits & (1u << uint(i))) != 0u
                                  : lightShadow(shadowTlas, light, P, N, L) > 0.0;
                if (!lit) continue;
            }
#else
            // A multiply, not a `continue`: PCF returns a fraction, unlike the traced path
            // above where a hard shadow is exactly 0 or 1.
            //
            // The sun reads its scene-fitted map; a point or spot reads the layers the atlas
            // assigned it. An interior gets all of its shadows from the second branch -- the
            // sun does not reach inside, so every light that lights the room is punctual.
            int lightType = int(light.params.z);
            if (lightType == LIGHT_DIRECTIONAL) {
                radiance *= shadowFactor(P, N, L);
            } else {
                radiance *= punctualShadow(P, N, L, distance(light.position.xyz, P),
                                           int(light.params.w), lightType);
            }
            if (dot(radiance, radiance) <= 0.0) continue;
#endif

            color += shadeLight(N, V, L, radiance, diffuseColor, f0, roughness);
        }
    }

    // A flat ambient, occluded. Where no light reaches a surface and the author asked for
    // no ambient, the surface is black.
    //
    // There was an environment term here -- split-sum IBL over a prefiltered sky cube --
    // and it went for being confidently wrong indoors. The cubes are built once from the
    // sky and are scene-blind: asked for radiance in any direction from inside an
    // enclosed arcade they return sky, at full strength, weighted by a Fresnel term that
    // peaks at grazing incidence. Measured in a shadowed bay it was 63% of every pixel
    // and its blue channel was double its red, drawn as a bright outline around every
    // silhouette in the room. Occluding it was not available: SSAO reaches about half a
    // metre and the vault it needed to know about is twenty units up.
    //
    // What replaces it claims nothing by comparison: one number an author picks, with no
    // direction and no opinion about the room, zero unless asked for. It is not baked
    // irradiance and not traced sky visibility, which is what indirect light actually
    // wants; it is the knob to reach for until one of those exists. A metal has no
    // diffuse, so this does not touch it -- rougher than the traced reflections reach, a
    // metal still renders on direct light alone.
    //
    // Occluded by both terms, per the note above `occlusion`. SSAO's half-metre reach is
    // the right scale for *this* ambient in a way it never was for the IBL cube: a flat
    // constant has no direction to be wrong about, so attenuating it by local geometry is
    // the only thing that gives a crease any shape at all.
    color += constantAmbient(diffuseColor, occlusion * ssao);

    // Emissive is radiance the surface produces, so it is added after shading and is
    // unaffected by lights, shadowing or occlusion.
    color += GFETCH(gEmissive, coord, sampleIndex).rgb;

    return color;
}

void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);
    uint debugView = frame.flags.x;

    if (debugView != 0u) {
        // Debug views show sample 0 only — the point is to inspect what the geometry
        // pass wrote, not what the resolve produces.
        vec4 albedo = GFETCH(gAlbedo, coord, 0);
        vec3 normal = octDecode(GFETCH(gNormal, coord, 0).xy);
        vec4 orm = GFETCH(gOrm, coord, 0);
        float depth = GFETCH(gDepth, coord, 0).r;

        if (debugView == 1u) outColor = vec4(albedo.rgb, 1.0);
        else if (debugView == 2u) outColor = vec4(normal * 0.5 + 0.5, 1.0);
        else if (debugView == 3u) outColor = vec4(orm.rgb, 1.0);
        else if (debugView == 4u) outColor = vec4(vec3(pow(depth, 0.15)), 1.0);
        else if (debugView == 5u) outColor = vec4(GFETCH(gEmissive, coord, 0).rgb, 1.0);
        else if (debugView == 6u) {
            // White where SSAO is compiled out, so "no occlusion" and "the pass is not
            // running" read the same way they do in the lit image rather than as black.
            outColor = vec4(vec3(ENABLE_SSAO ? texture(ssaoMap, vUV).r : 1.0), 1.0);
        }
        else if (debugView == 7u) {
            // Which pixels 3.6 has to shade per-sample. Red should trace silhouettes
            // and nothing else: red across a flat wall means the classifier is reading
            // a channel that is not broadcast, and the hybrid path is then costing
            // more than the loop it replaced.
            outColor = samplesAgree(coord) ? vec4(albedo.rgb * 0.25, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
        }
        else outColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    // A constant rather than a uniform test, so with SSAO off the fetch and the
    // descriptor read are gone rather than branched around. Gated at all because with
    // the pass off the buffer is never written, and sampling it would fold undefined
    // data into the ambient term.
    float ssao = ENABLE_SSAO ? texture(ssaoMap, vUV).r : 1.0;

    // Edge-detect hybrid MSAA (3.6). The per-sample loop below is what makes MSAA
    // scale almost linearly -- 1.99x/3.84x/8.18x measured -- and almost all of that
    // is spent re-shading samples that hold the same fragment. Shading one of them
    // where they agree is not an approximation of the resolve; it is the resolve,
    // arrived at without evaluating the same integrand N times.
    //
    // A branch rather than the stencil pass the roadmap sketched. Stencil would need
    // the G-buffer depth to become D32_SFLOAT_S8_UINT -- eight bytes per sample where
    // it is now four, which spends most of what 3.5 just saved -- plus a
    // classification pass and a second lighting pipeline. This costs one
    // specialisation constant, and edges are spatially coherent enough that most
    // waves take the fast path together. What it measures is in plan/11-tier3.md.
    if (ENABLE_EDGE_MSAA && samplesAgree(coord)) {
        // **No mask here, whatever ENABLE_SHADOW_MASK says.** This pixel is one fragment
        // and one ray per light already answers it, so a mask entry would be a store and a
        // load carrying what the trace below produces anyway -- and writing one would put
        // shadowmask.frag's arithmetic in front of the four fifths of the screen that take
        // this branch. shadowmask.frag skips exactly these pixels for the same reason.
        outColor = vec4(shadeSample(coord, 0, vUV, ssao, false), 1.0);
        return;
    }

    // The pixels the mask covers, and the only ones: several fragments in one pixel, each
    // needing its own shadow answer, and previously one ray per *sample* rather than one
    // per fragment.
    vec3 radiance = vec3(0.0);
    for (uint s = 0u; s < SAMPLE_COUNT; ++s) {
        radiance += shadeSample(coord, int(s), vUV, ssao, ENABLE_SHADOW_MASK);
    }

    outColor = vec4(radiance / float(SAMPLE_COUNT), 1.0);
}
