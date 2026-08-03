/**
 * Traced shadow queries for the deferred lighting pass.
 *
 * ## What this replaces
 *
 * The cascades, and only the cascades. What survives is one scene-fitted map for the sun
 * (`recordShadows`) and the 24-layer atlas for points and spots (`recordPunctualShadows`),
 * both still built from `shadow.vert`/`shadow.frag` and still selected by `shadowsEnabled`
 * in the variant key -- so there *is* a second shadow calculation in the tree, it runs
 * whenever ray query is off, and `--no-rt` is the arm that draws it. A ray either reaches
 * the light or it does not; a map still has a bias to tune.
 *
 * ## Both paths, one calculation
 *
 * The deferred lighting pass shades the primary image with these and `shadeRayHit`
 * shades reflection hits with them, and the two must agree: a surface and its own
 * reflection asking two slightly different shadow questions is exactly the "reflected
 * shadows don't match the world" bug, and agreement by construction is the only kind
 * that survives editing.
 *
 * A shadow ray from a reflection hit *is* a second bounce, which inline ray query has no
 * recursion for -- but it does not need any. Nothing here calls back into the traversal
 * that found the hit; it opens its own query from the hit point, which is an ordinary
 * ray like any other. What it costs is a ray per light per reflection ray, on top of a
 * pass already tracing one per pixel, and that is the price of the two agreeing.
 *
 * ## The tlas is a parameter, not a binding
 *
 * The two callers bind the acceleration structure at different set indices -- set 2 for
 * the deferred pass, set 3 for the tracing compute passes, which spend 2 on their image
 * pair. A binding declared here would be wrong for one of them, and a descriptor set
 * index cannot be specialised away, so the handle arrives as an argument and each caller
 * declares its own.
 *
 * ## Ray query, not a ray-tracing pipeline
 *
 * Inline in a shader that was already running: no rgen, no miss shader, no shader
 * binding table, and so no second pipeline type threaded through Pipeline.cpp.
 *
 * TerminateOnFirstHit because a shadow ray only asks *whether* something is in the way
 * and never which thing or how far.
 *
 * **Opaque is deliberately absent**, and that is the one flag decision here worth
 * stating. `gl_RayFlagsOpaqueEXT` forces every triangle opaque *for that ray*, which
 * would override the per-geometry `VK_GEOMETRY_OPAQUE_BIT_KHR` that AccelStruct.cpp
 * withholds from emissive geometry -- and withholding it is exactly how a light escapes
 * the mesh that represents it. Leaving the flag off costs nothing on the geometry that
 * has the bit: the implementation still confirms those in hardware and terminates. Only
 * emissive geometry comes back as a candidate, and the loop below is what walks past it.
 *
 * Measured, because the opposite reading is the intuitive one: adding
 * `gl_RayFlagsOpaqueEXT` here moved the Lighting zone 2.353 -> 2.320 ms on showcase at 4x
 * MSAA, inside the 0.05 ms noise floor, while breaking the `emissive` golden outright.
 * There is no traversal cost being paid for this and no reason to restructure the BLAS to
 * reclaim one.
 *
 * ## Bias
 *
 * One nudge along the normal, and that is the whole apparatus. There is no depth-map
 * quantisation to bias against here -- the only error is in the position the caller
 * reconstructed from the depth buffer, which is texel-independent, so a constant covers
 * it where a shadow map needed a slope-scaled term.
 */

/// How many lights the per-sample shadow mask carries, one bit each. It is the width of
/// one `r32ui` texel and nothing else, so it is not `Renderer::lightBudget` and must not
/// be made to track it: a scene that states a wider budget keeps its extra lights and
/// traces them in the lighting pass, which is what shadowmask.frag and lighting_body.glsl
/// both clamp their loops to.
const int kShadowMaskLights = 32;

/// World units along the normal that a shadow ray starts from the surface. Large enough
/// to clear the reconstruction error at the far plane, small enough not to show as light
/// leaking under a contact.
const float kShadowNormalBias = 0.02;

/// How far short of a punctual light its shadow ray stops, so geometry at or just beyond
/// the light -- a fixture mesh centred on it, the wall immediately behind -- is not
/// counted as an occluder of that light.
const float kShadowLightMargin = 0.04;

/// 1.0 if nothing sits within `tMax` of `P` along `L`, else 0.0. Hard: one ray, one
/// answer, no penumbra. A soft edge means several rays across the light's solid angle
/// and something to denoise the result, and neither exists yet.
float tracedShadow(accelerationStructureEXT tlas, vec3 P, vec3 N, vec3 L, float tMax) {
    // A degenerate range traces nothing and must read as lit, not as shadowed: this is
    // the case where the shading point is closer to a punctual light than the margin.
    if (tMax <= 0.0) return 1.0;

    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF,
                          P + N * kShadowNormalBias, 0.01, L, tMax);
    // A *loop*, not one step, and the difference only became load-bearing when emissive
    // geometry stopped being opaque. An opaque triangle is confirmed by the
    // implementation and, with terminate-on-first-hit, ends the query -- so one proceed
    // was enough while everything was opaque. A non-opaque one is handed back as a
    // candidate that this shader declines to confirm, and the traversal has to be driven
    // past it. Stopping at the first candidate would report a wall behind a glowing orb
    // as unoccluded.
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

/// A directional light. The range outreaches any scene this engine has been pointed at,
/// which is what "directional" means here in practice -- there is no light position to
/// stop at, so the ray has to run until it leaves the world.
float tracedSunShadow(accelerationStructureEXT tlas, vec3 P, vec3 N, vec3 L) {
    return tracedShadow(tlas, P, N, L, 1e4);
}

/// A point or spot light, whose ray stops just short of the light itself.
float tracedLightShadow(accelerationStructureEXT tlas, vec3 P, vec3 N, vec3 L, float distToLight) {
    return tracedShadow(tlas, P, N, L, distToLight - kShadowLightMargin);
}

/**
 * The shadow term for one light, from its `params.z` type and the point being shaded.
 *
 * Both callers reach this rather than the two above, so the type branch and the distance
 * are written once. That branch is the ray's *range* and is the whole difference between
 * the cases: a directional light has no position to stop at, a punctual one does.
 *
 * `distance()` recomputes what `lightRadiance` already found on its way to the falloff.
 * Left duplicated rather than plumbed out through a second out parameter, because one
 * square root against a ray traversal is not a cost and the alternative changes a
 * signature both paths share.
 */
float lightShadow(accelerationStructureEXT tlas, Light light, vec3 P, vec3 N, vec3 L) {
    if (int(light.params.z) == LIGHT_DIRECTIONAL) return tracedSunShadow(tlas, P, N, L);
    return tracedLightShadow(tlas, P, N, L, distance(light.position.xyz, P));
}
