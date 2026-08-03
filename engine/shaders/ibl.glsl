/**
 * What is left of image-based lighting: the skybox, and the BRDF weight the traced
 * reflections are composited with.
 *
 * ## Why the environment term went
 *
 * There was a full split-sum chain here (Karis 2013) -- a cosine-convolved irradiance
 * cube for the diffuse half, a roughness-mipped prefiltered cube for the specular, and
 * this BRDF table joining them. It supplied every surface's indirect light, and it was
 * confidently wrong indoors.
 *
 * Both cubes are built once, from the sky. They have no way to represent a room. Asked
 * for radiance in any direction from inside an enclosed arcade they return daylight, at
 * full strength, weighted by a Fresnel term that peaks at grazing incidence -- so the
 * strongest ambient in the scene landed on exactly the silhouettes, as a blue outline
 * around everything in a bay the sun could not reach. Measured there it was 63% of every
 * pixel, with its blue channel at double its red.
 *
 * Nothing available could occlude it. SSAO reaches about half a metre; the vault that
 * made the bay dark is twenty units overhead. A traced hemisphere gather could, was
 * built, and was removed for its Monte Carlo grain.
 *
 * So the ambient term is gone rather than approximated, and unlit surfaces are black.
 * That is a floor, not a finish: indirect light belongs to something that can measure
 * the room -- baked irradiance, or traced sky visibility with a real denoising plan --
 * and black is honest until one exists in a way sky-blue never was.
 *
 * ## What survives, and why each earns its place
 *
 * - `environmentMap` draws the sky, which is a real thing the camera can see.
 * - `brdfLut`/`envBRDF` weight the *traced* reflections. That is not ambient: it is the
 *   BRDF factor applied to radiance a ray actually went and fetched, and ssr_body.glsl
 *   needs exactly the same factor the removed specular used, or the composite loses
 *   energy at every reflective pixel.
 */

/// Which set this layout is bound at. The deferred and forward paths bind it at 3; the
/// tracing compute passes already spend 3 on the TLAS set and bind it at 4. The *layout*
/// is the same `iblSetLayout` in both cases -- only the index differs -- so this is one
/// declaration parameterised, not two to keep in step.
#ifndef IBL_SET
#define IBL_SET 3
#endif

/// Bindings 0 and 1 held the irradiance and prefiltered cubes and are vacant. The two
/// survivors keep the numbers they have always had, so no shader renumbered when the
/// environment term was removed -- the same choice the TLAS binding got when the shadow
/// maps beside it went.
layout(set = IBL_SET, binding = 2) uniform sampler2D brdfLut;
layout(set = IBL_SET, binding = 3) uniform samplerCube environmentMap;

/**
 * The skybox.
 *
 * No geometry, no separate pass, no depth trick: the deferred lighting pass already
 * branches on "nothing was written here", and the environment it would sample is this
 * cube. A fullscreen pass that already runs is strictly cheaper than a cube drawn at the
 * far plane.
 */
vec3 skyboxRadiance(vec3 viewRay) { return textureLod(environmentMap, viewRay, 0.0).rgb; }

/// The split-sum BRDF factor: the tabulated scale and bias applied to F0. This is the
/// weight traced reflection radiance is multiplied by. It was separated out when the
/// prefiltered specular still existed, so the reflection pass could apply exactly the
/// term the lighting pass removed; the lighting pass has no such term any more, and this
/// is now simply how a reflection is weighted.
vec3 envBRDF(vec3 N, vec3 V, vec3 f0, float roughness) {
    float ndotv = clamp(dot(N, V), 0.0, 1.0);
    vec2 ab = texture(brdfLut, vec2(ndotv, roughness)).rg;
    return f0 * ab.x + ab.y;
}
