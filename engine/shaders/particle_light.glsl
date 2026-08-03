// Per-particle lighting (S3.5). Included by particle_emit.comp and
// particle_simulate.comp, which are the two shaders that can write a particle's colour.
//
// ## Once per particle, not once per fragment
//
// This is the whole of S3.5's "fidelity-versus-cost decision, not a detail". A blended
// billboard is the most overdrawn thing a renderer draws: a plume of smoke covers the
// same pixels twenty times over, so shading it per fragment would multiply the light
// loop by the overdraw *and* by the pixel count. Shading it per particle costs
// O(particles x lights) and the result is constant across a sprite that is a few dozen
// pixels wide -- which for a diffuse puff is not an approximation anybody can see.
//
// It also settles the interaction 0.9 flagged. The deferred loop the light budget
// exists to bound is O(pixels x lights); this one is O(particles x lights), and at a
// few thousand particles against 1600x900 pixels it is three orders of magnitude
// smaller. Particles therefore read the *same* light list, in full, rather than needing
// a budget of their own -- and if the budget ever drops a light, particles and surfaces
// lose the same one, which is the only way the two can agree.
//
// ## What a particle is, for lighting purposes
//
// A sphere, and an isotropic one. It has no normal -- a billboard's is the view
// direction, which would make a particle brighten as you walked around it -- so what is
// evaluated is the irradiance arriving at a point from every direction, with no cosine
// term and no BRDF: the flat ambient plus the punctual falloff each light already
// carries. Unshadowed, unlike the surfaces around it -- a shadow ray per light per
// particle is affordable, but a hard 0-or-1 answer on an isotropic puff would pop as it
// drifted across an edge, and softening it is the penumbra problem nothing here solves
// yet.
//
// The irradiance cube this used to sample was declared here at set 3 binding 0 and went
// with the rest of the environment term.

/**
 * The colour to store in the pool: radiance, already premultiplied by coverage.
 *
 * ## One blend state for additive and alpha particles
 *
 * The pipeline blends `src + dst * (1 - src.a)`, which is ordinary premultiplied-over
 * -- the same mode volumetric fog composites with. An emissive particle writes a
 * *zero* alpha with a non-zero rgb, and premultiplied-over with `src.a == 0` reduces
 * exactly to `src + dst`: additive, for free, with no second pipeline and no second
 * blend state. That matters more than it saves, because two blend states would mean two
 * draws, and two draws would mean the global sort could not be global.
 *
 * So `emissive` says two things at once and they belong together: a flame is radiance
 * rather than albedo, and a flame adds light rather than hiding what is behind it.
 */
vec4 particleColor(Emitter e, vec3 worldPos, float life) {
    vec4 authored = mix(e.colorStart, e.colorEnd, life);
    float coverage = clamp(authored.a, 0.0, 1.0);

    if ((e.flags.y & EMITTER_EMISSIVE) != 0u) {
        // Alpha 0: additive. Scaled by the authored alpha anyway, so a flame still
        // fades out over its life rather than vanishing at the last frame.
        return vec4(authored.rgb * e.params.w * coverage, 0.0);
    }

    // Sun. No cosine: an isotropic sphere receives a quarter of a directional light's
    // irradiance over its surface, and that constant is folded in here rather than
    // left for an artist to discover in the colour.
    vec3 radiance = frame.sunColor.rgb * frame.sunDirection.w * 0.25;

    // The flat ambient, added directly rather than through constantAmbient: a particle
    // has no albedo to tint it and no occlusion to attenuate it, so what that helper does
    // to a surface reduces here to the term itself. Two poles of the irradiance cube
    // averaged used to stand here, and it went with the rest of the environment term --
    // the cube was built from the sky, so a plume drifting through a shadowed arcade was
    // lit by daylight.
    radiance += frame.ambient.rgb;

    // Punctual lights. The falloff is repeated here rather than shared: `lightRadiance`
    // lives in pbr.glsl with the BRDF machinery, and a particle wants the windowed
    // falloff alone.
    uint lightCount = uint(frame.params.y);
    for (uint i = 0u; i < lightCount; ++i) {
        Light l = lights[i];
        int type = int(l.params.z);
        if (type == LIGHT_DIRECTIONAL) continue; // the sun, already accounted for above

        vec3 toLight = l.position.xyz - worldPos;
        float distSq = dot(toLight, toLight);
        float dist = sqrt(max(distSq, 1e-8));
        vec3 L = toLight / dist;

        float attenuation = 1.0 / max(distSq, 1e-4);
        float range = l.position.w;
        if (range > 0.0) {
            float window = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
            attenuation *= window * window;
        }
        if (type == LIGHT_SPOT) {
            float cd = dot(normalize(-l.direction.xyz), L);
            attenuation *= clamp((cd - l.params.y) / max(l.params.x - l.params.y, 1e-4), 0.0, 1.0);
        }

        radiance += l.color.rgb * l.color.w * attenuation * 0.25;
    }

    // Albedo times irradiance, then premultiplied by coverage. Premultiplied here
    // rather than in the fragment shader because the fragment shader only ever scales
    // this by a mask, and a mask applied to an already-premultiplied colour is still
    // premultiplied.
    return vec4(authored.rgb * radiance * coverage, coverage);
}
