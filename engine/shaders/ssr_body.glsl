/**
 * Screen-space reflections (3.1).
 *
 * Included by ssr.comp (multisampled G-buffer) and ssr1x.comp (single-sampled), which
 * differ only in whether GBUFFER_MULTISAMPLE is defined -- the same split
 * lighting.frag and lighting1x.frag already carry, and for the same reason: a genuinely
 * single-sample image cannot bind to a sampler2DMS descriptor.
 *
 * **New work.** Tethered has no SSR; the `reflections.comp` the roadmap once cited is
 * its ray-traced one, which is 3.11.
 *
 * ## What it is and is not
 *
 * This traces the *depth buffer*, so it can only reflect what is already on screen.
 * A reflection of something behind the camera, or occluded, or off the edge of the
 * frame, has no data to find and fades out. That is the deal SSR makes, and it is why
 * 3.11 exists rather than this superseding it: SSR is the portable path that runs on
 * any GPU at a fraction of the cost, and it is wrong in a way that is cheap to hide
 * (fade at the screen edge) rather than cheap to fix.
 *
 * Only sample 0 of the G-buffer is read. A reflection is a low-frequency quantity
 * composited into an already-resolved image, so per-sample reflection would cost N
 * marches to produce a value the edge pixel is about to blend away anyway.
 */

/// 8x8, matching every other compute pass here and the dispatch in recordSsr.
///
/// Declared in this shared body rather than in ssr.comp and ssr1x.comp, because it was
/// omitted entirely the first time and glslang silently defaulted to one invocation per
/// group. The dispatch is sized in groups, so the pass ran over the top-left 200x113
/// pixels of a 1600x900 frame and produced a plausible-looking reflection there and
/// black everywhere else -- a failure that reads as "SSR found nothing on this scene".
layout(local_size_x = 8, local_size_y = 8) in;

#ifdef GBUFFER_MULTISAMPLE
layout(set = 1, binding = 0) uniform sampler2DMS gAlbedo;
layout(set = 1, binding = 1) uniform sampler2DMS gNormal;
layout(set = 1, binding = 2) uniform sampler2DMS gOrm;
layout(set = 1, binding = 3) uniform sampler2DMS gDepth;
layout(set = 1, binding = 4) uniform sampler2DMS gEmissive;
#define GFETCH0(tex, coord) texelFetch(tex, coord, 0)
#else
layout(set = 1, binding = 0) uniform sampler2D gAlbedo;
layout(set = 1, binding = 1) uniform sampler2D gNormal;
layout(set = 1, binding = 2) uniform sampler2D gOrm;
layout(set = 1, binding = 3) uniform sampler2D gDepth;
layout(set = 1, binding = 4) uniform sampler2D gEmissive;
#define GFETCH0(tex, coord) texelFetch(tex, coord, 0)
#endif
layout(set = 1, binding = 5) uniform sampler2D ssaoMap;

/// The lit scene, after lighting and the forward pass. This is what a reflection ray
/// samples when it hits something, which is also why this pass runs where it does:
/// earlier and there would be nothing lit to reflect.
layout(set = 2, binding = 0) uniform sampler2D litColor;
layout(set = 2, binding = 1, rgba16f) uniform writeonly image2DArray outReflection;

#ifdef ENABLE_RAY_QUERY
/// The scene TLAS (3.9). The environment cube, and the rest of the IBL layout the hit
/// shading needs, come from ibl.glsl at IBL_SET; the scene's materials and textures come
/// from raytrace.glsl at RT_SCENE_SET. Present only in the ray-traced variants; see
/// ssr_rt.comp.
layout(set = 3, binding = 2) uniform accelerationStructureEXT sceneTlas;
#endif

layout(push_constant) uniform Push {
    vec2 texel;
    float maxDistance;      ///< world units a ray may travel
    float thickness;        ///< world-space depth window counted as a hit
    float intensity;
    float roughnessCutoff;  ///< surfaces rougher than this reflect nothing
    uint stepCount;
    uint refineSteps;
#ifdef ENABLE_RAY_QUERY
    /// Non-zero to trace a shadow ray per light at each reflection hit, matching what the
    /// lighting pass does for the primary image.
    ///
    /// A push constant rather than the specialisation constant the lighting pass uses for
    /// the same toggle, because `createComputePipeline` supplies no constants and adding
    /// that machinery for one flag is more surface than a uniform branch in front of a
    /// ray traversal costs. `SsrPush` names it identically; a struct that agrees only by
    /// accident of two compilers padding the same way is one compiler away from silent
    /// garbage -- which is also why `pad` below is written out rather than left to the
    /// two of them to insert: the buffer references need 8-byte alignment and the fields
    /// above come to 36. It used to be free, in the word `nearPlane` left behind; P3
    /// moved that coefficient into the frame uniforms and the padding is real again.
    uint shadowLights;
    uint pad;
    /// Geometry for hit shading, by address rather than by descriptor. Declared last so
    /// the non-tracing variants read a prefix of the same push range and need no second
    /// struct. See raytrace.glsl.
    RtHitRecords hitRecords;
    RtVertices sceneVertices;
    RtIndices sceneIndices;
    RtVertices deformedVertices;
    RtIndices deformedIndices;
#endif
} pc;


/// Project a world point back to screen. `uv` in 0..1, `depth` in reverse-Z clip.
bool projectToScreen(vec3 world, out vec2 uv, out float depth) {
    vec4 clip = frame.viewProj * vec4(world, 1.0);
    if (clip.w <= 0.0) return false;
    uv = (clip.xy / clip.w) * 0.5 + 0.5;
    depth = clip.z / clip.w;
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}

// `viewDistance` is in frame.glsl (P3). Comparing *distances* rather than depths is what
// makes `thickness` a world-space number meaning the same thing at every range, instead
// of a depth epsilon that does not -- and that argument is what this march wants from it,
// whichever projection produced the depth.

/// Depth at a screen uv. texelFetch rather than texture(): at MSAA the depth target is
/// a sampler2DMS, which has no filtered lookup, and nearest is what a ray march wants
/// in any case -- an interpolated depth between two surfaces is a value no surface has.
float depthAt(vec2 uv, ivec2 size) {
    ivec2 c = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
    return GFETCH0(gDepth, c).r;
}

void main() {
    // `size` is the full-resolution frame -- the depth buffer the march samples and the
    // lit image it reads on a hit. `outSize` is this dispatch's own grid, which is
    // `render.ssrScale` of it. The two were one value until the scale existed, and the
    // trap in merging them again is that the march would then walk the depth buffer in
    // reduced coordinates and reflect the top-left corner of the frame.
    ivec2 size = textureSize(litColor, 0);
    // Recovered from `texel` rather than passed as its own field: a float32 reciprocal of
    // an integer this size round-trips to within 2e-4, so the `round` is exact, and the
    // alternative is a uvec2 inserted into a push block whose 8-byte alignment is already
    // hand-balanced by `pad`.
    ivec2 outSize = ivec2(round(vec2(1.0) / pc.texel));
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= outSize.x || coord.y >= outSize.y) return;

    imageStore(outReflection, ivec3(coord, 0), vec4(0.0));

    // Where in the full-resolution G-buffer this reflection texel sits. At scale 1.0 the
    // half-texel offset lands this exactly back on `coord`, so the unscaled path fetches
    // what it always did.
    vec2 uv = (vec2(coord) + 0.5) * pc.texel;
    ivec2 gcoord = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);

    float depth = GFETCH0(gDepth, gcoord).r;
    if (depth == FAR_DEPTH) return; // sky: the skybox is already the reflection

    vec4 orm = GFETCH0(gOrm, gcoord);
    float roughness = clamp(orm.g, 0.04, 1.0);
    if (roughness > pc.roughnessCutoff) return;

    vec3 P = worldFromDepth(uv, depth);
    vec3 N = octDecode(GFETCH0(gNormal, gcoord).xy);
    vec3 V = normalize(frame.cameraPos.xyz - P);
    if (dot(N, V) <= 0.0) return;

    vec3 R = reflect(-V, N);

    // Fresnel decides how much of the reflection is visible at all. Without it a
    // head-on view of rough stone reflects as strongly as a grazing view of polished
    // marble, which is the single most common way SSR reads as "wrong" rather than
    // "missing".
    vec3 albedo = GFETCH0(gAlbedo, gcoord).rgb;
    float metallic = clamp(orm.b, 0.0, 1.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float ndv = max(dot(N, V), 0.0);
    vec3 fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - ndv, 5.0);

    // Fade out as roughness approaches the cutoff, so a material gradient does not
    // produce a hard line where reflections stop.
    float roughFade = 1.0 - smoothstep(pc.roughnessCutoff * 0.5, pc.roughnessCutoff, roughness);
    if (roughFade <= 0.0) return;

#ifdef ENABLE_RAY_QUERY
    // ---------------------------------------------------------------- ray traced
    // Ray query against the scene, in place of the screen-space march below. Two of
    // SSR's three failure modes go away outright: a ray that leaves the frame still
    // finds geometry, and the intersection is exact rather than the first depth-buffer
    // texel a fixed stride happened to land behind.
    //
    // The third failure mode goes away here rather than in the trace. An earlier version
    // reprojected the hit into screen space and sampled the lit image, falling back to
    // the environment cube where the depth buffer disagreed -- which meant a ray that
    // found geometry the camera could not see still came back as sky, and the exact
    // intersection was spent asking the depth buffer a question. Now the hit is shaded
    // from its own vertex data and material: see shadeRayHit in raytrace.glsl.
    //
    // A miss still samples the environment cube, and that is the correct answer rather
    // than a fallback -- a ray that leaves the scene *should* return sky radiance.
    vec3 origin = P + N * 0.02;
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, sceneTlas, gl_RayFlagsOpaqueEXT, 0xFF, origin, 0.01, R, pc.maxDistance);
    while (rayQueryProceedEXT(rq)) {}

    vec3 reflectedRt;
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        reflectedRt = texture(environmentMap, R).rgb;
    } else {
        RayScene rtScene =
            RayScene(pc.hitRecords, pc.sceneVertices, pc.sceneIndices, pc.deformedVertices, pc.deformedIndices);
        reflectedRt = shadeRayHit(rq, sceneTlas, rtScene, origin, R, pc.shadowLights != 0u);
    }

    // Weighted by the same split-sum BRDF factor the lighting pass removes from its
    // prefiltered specular (times the same roughFade), so the additive composite is a
    // *replacement* by construction: what lighting subtracted, this puts back, computed
    // from the traced scene instead of the sky cube. The Schlick-with-roughness
    // `fresnel` above belongs to the screen-space march below, which samples an image
    // that still contains its own full specular and needs the softer weight.
    imageStore(outReflection, ivec3(coord, 0),
               vec4(reflectedRt * envBRDF(N, V, f0, roughness) * roughFade * pc.intensity, 1.0));
    return;
#endif

    // Start a little along the ray. Beginning at P makes the first sample compare the
    // surface against itself, which self-intersects on every pixel.
    float stride = pc.maxDistance / float(pc.stepCount);
    vec3 rayPos = P + N * 0.02 + R * stride;

    bool hit = false;
    vec2 hitUv = vec2(0.0);
    vec3 prevPos = rayPos;

    for (uint i = 0u; i < pc.stepCount; ++i) {
        vec2 sUv;
        float sDepth;
        if (!projectToScreen(rayPos, sUv, sDepth)) break;

        float sceneDepth = depthAt(sUv, size);
        float rayDist = viewDistance(sDepth);
        float sceneDist = viewDistance(sceneDepth);

        // The ray is behind the surface at this pixel, and not so far behind that it
        // must have passed through empty space on the far side of a thin object.
        if (rayDist > sceneDist && rayDist - sceneDist < pc.thickness) {
            hit = true;
            hitUv = sUv;
            break;
        }
        prevPos = rayPos;
        rayPos += R * stride;
    }

    if (!hit) return;

    // Binary refinement between the last miss and the hit. Linear marching lands the
    // intersection anywhere inside one stride, and at these strides that is visibly
    // the wrong pixel on a floor reflection.
    vec3 lo = prevPos;
    vec3 hi = rayPos;
    for (uint i = 0u; i < pc.refineSteps; ++i) {
        vec3 mid = (lo + hi) * 0.5;
        vec2 sUv;
        float sDepth;
        if (!projectToScreen(mid, sUv, sDepth)) break;
        float sceneDepth = depthAt(sUv, size);
        if (viewDistance(sDepth) > viewDistance(sceneDepth)) {
            hi = mid;
            hitUv = sUv;
        } else {
            lo = mid;
        }
    }

    // Fade at the screen edge. A reflection that ends in a hard line where the ray ran
    // out of screen is more distracting than no reflection at all, and this is the one
    // artefact SSR can always be made to hide.
    vec2 edge = smoothstep(vec2(0.0), vec2(0.12), hitUv) * (1.0 - smoothstep(vec2(0.88), vec2(1.0), hitUv));
    float fade = edge.x * edge.y * roughFade;

    vec3 reflected = texture(litColor, hitUv).rgb;
    imageStore(outReflection, ivec3(coord, 0), vec4(reflected * fresnel * fade * pc.intensity, 1.0));
}
