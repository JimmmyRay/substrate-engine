// Cook-Torrance microfacet BRDF.
const float PI = 3.14159265359;

float distributionGGX(float NoH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// Height-correlated Smith visibility: the G term already divided by 4*NoL*NoV.
float visibilitySmithGGX(float NoV, float NoL, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float lambdaV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float lambdaL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

vec3 fresnelSchlick(vec3 f0, float cosTheta) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

/**
 * A flat ambient term for surfaces no light reaches.
 *
 * Diffuse only: a constant has no direction, so there is nothing honest to hand a
 * specular lobe. A metal has no diffuse and is therefore untouched -- it stays black
 * unless a reflection reaches it, which is correct, because a metal with no environment
 * to reflect genuinely has nothing to show.
 *
 * `occlusion` is the same product of SSAO and the glTF occlusion texture the environment
 * term used to take. Both are ambient quantities and this is the only ambient there is,
 * so this is the one place either of them is read at all.
 *
 * Here rather than in ibl.glsl because it is not image-based lighting and should not sit
 * in the file named for it: no cube, no lookup, no direction. Every caller already
 * includes pbr.glsl for shadeLight, and frame.glsl precedes it in all of them.
 */
vec3 constantAmbient(vec3 diffuseColor, float occlusion) {
    return frame.ambient.rgb * diffuseColor * occlusion;
}

/// Direct lighting contribution for one light.
vec3 shadeLight(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 diffuseColor, vec3 f0, float roughness) {
    float NoL = max(dot(N, L), 0.0);
    if (NoL <= 0.0) return vec3(0.0);

    vec3 H = normalize(V + L);
    float NoV = max(dot(N, V), 1e-4);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);

    float D = distributionGGX(NoH, roughness);
    float Vis = visibilitySmithGGX(NoV, NoL, roughness);
    vec3 F = fresnelSchlick(f0, VoH);

    vec3 specular = D * Vis * F;
    vec3 diffuse = (1.0 - F) * diffuseColor / PI;

    return (diffuse + specular) * radiance * NoL;
}

// Lives here rather than in shadow.glsl, where it started when the deferred lighting
// pass was its only caller. It moved because raytrace.glsl needs it too and could not
// include shadow.glsl -- that file also declared the cascade and cube-shadow
// descriptors, which a tracing compute pass has no business binding. shadow.glsl has
// since gone entirely, but this is still where the function belongs: it is pure maths
// over a Light, and both callers want exactly that.
/**
 * Radiance arriving at `P` from one light, **unoccluded**. Visibility is the caller's
 * to apply and is deliberately not folded in here: the two callers answer it
 * differently -- the lighting pass traces a shadow ray per light, shadeRayHit traces
 * none -- and this function is the part they agree on.
 *
 * The falloff maths is the KHR_lights_punctual model: inverse-square windowed to
 * reach zero at `range`, and a smooth ramp between the cone cosines for spots.
 */
vec3 lightRadiance(Light light, vec3 P, vec3 N, out vec3 L) {
    int type = int(light.params.z);

    if (type == LIGHT_DIRECTIONAL) {
        // *Not* negated, unlike the spot below, and this is the one line in the file
        // where the two conventions differ. `GpuLight::direction` holds "toward the
        // light" for a directional and "where it is aimed" for a spot -- Light.h says
        // so and argues for the asymmetry -- so L for a sun reads straight out.
        //
        // It was negated until S3, which is a bug S2 had already half-found: "the box
        // face pointing *at* the sun renders black" was this, and so was the floor of
        // every generated test scene receiving no sun at all. Sponza never showed it
        // because its floor is lit by IBL and four auto-placed punctual lights, and
        // because an interior lit from below is still an interior lit.
        L = normalize(light.direction.xyz);
        return light.color.rgb * light.color.w;
    }

    vec3 toLight = light.position.xyz - P;
    float distSq = dot(toLight, toLight);
    float dist = sqrt(max(distSq, 1e-8));
    L = toLight / dist;

    float range = light.position.w;
    float attenuation = 1.0 / max(distSq, 1e-4);
    if (range > 0.0) {
        // Windowed so influence reaches zero at the range instead of being clipped,
        // which would leave a visible disc edge on the floor.
        float window = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
        attenuation *= window * window;
    }

    if (type == LIGHT_SPOT) {
        float cd = dot(normalize(-light.direction.xyz), L);
        // params.x is cos(inner), params.y is cos(outer); inner is the larger cosine.
        attenuation *= clamp((cd - light.params.y) / max(light.params.x - light.params.y, 1e-4), 0.0, 1.0);
    }

    return light.color.rgb * light.color.w * attenuation;
}
