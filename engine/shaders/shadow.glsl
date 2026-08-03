/**
 * Sun shadow sampling for the non-traced path.
 *
 * One map, fitted to the scene, with no camera term in its projection. The cascades this
 * replaced were fitted to the view frustum, and every quantity that differed between two
 * cascades -- the world size of a texel, the world distance the depth bias pushed an
 * occluder, the width the filter kernel spanned -- changed when a surface crossed a
 * split. Which cascade a surface was in is a function of where the camera stands, so
 * walking towards a wall slid the shadow across it. Same camera, cascade assignment the
 * only difference: 24% of pixels changed, peak delta 630.
 *
 * That is not a bias that wanted tuning, and no crossfade hides it; it is the camera term
 * in the projection. `frame.sunViewProj` is built from the scene bounds and the sun, so a
 * moving camera cannot change what this returns for a given surface.
 *
 * ## Bias, in metres
 *
 * Both biases arrive already converted (Renderer::updateSunShadow): `shadowParams.y` is
 * the depth bias as a fraction of the box's depth range, `shadowParams.z` is the normal
 * offset still in world units. One projection means one conversion -- the arithmetic that
 * had to be repeated per cascade, and could disagree between them, is gone.
 *
 * ## When this runs, and when it does not
 *
 * Only where `render.rt` is off, or the device cannot trace. With ray tracing on the
 * lighting pass is compiled from lighting_rt.frag and asks rayshadow.glsl instead, and
 * the reflection pass traces beside it. Ray tracing is one switch covering both, so there
 * is no arrangement in which a surface is shadowed by a map while its own reflection is
 * shadowed by a ray or by nothing.
 *
 * That also bounds what this has to do: the SSR march that replaces traced reflections
 * samples `litColor`, an image this has already shadowed, so reflections inherit the
 * shadows without this file being involved.
 *
 * ## Two maps, not one
 *
 * The sun's map covers the sun. Everything else is the atlas below, and in an interior
 * that is *all* of the shadows: the roof blocks the sun from the whole nave, so the sun's
 * map correctly shadows it uniformly and draws no shapes, and every light that actually
 * lights the room is a point or a spot. With the atlas off, such a scene has no cast
 * shadows whatsoever -- which is exactly how its absence presented.
 */

/// Binding 2 is the TLAS the traced path uses. Binding 0 is the number the sun's map has
/// always had; binding 1 is the punctual atlas beside it -- its own array because it is a
/// different resolution and a perspective projection per layer.
layout(set = 2, binding = 0) uniform sampler2DShadow shadowMap;
layout(set = 2, binding = 1) uniform sampler2DArrayShadow punctualShadowMap;

/// Texel size of one atlas layer. Must match kPunctualShadowSize in Renderer.h.
const float PUNCTUAL_TEXEL = 1.0 / 1024.0;

/// Which cube face a direction falls on, in the +X -X +Y -Y +Z -Z order updateLights
/// builds the matrices in. Change one and the other breaks silently.
int cubeFace(vec3 dir) {
    vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) return dir.x > 0.0 ? 0 : 1;
    if (a.y >= a.z) return dir.y > 0.0 ? 2 : 3;
    return dir.z > 0.0 ? 4 : 5;
}

/**
 * @return 1.0 lit, 0.0 fully shadowed, for one punctual light.
 *
 * Deliberately not merged with the sun lookup: that one has a single orthographic box and
 * no layer to choose, this one picks a cube face and lives in a perspective projection.
 * A single function carrying both would be a parameter list of things one caller always
 * passes and the other never does.
 */
float punctualShadow(vec3 worldPos, vec3 N, vec3 L, float distToLight, int firstLayer, int type) {
    // Compiled out entirely when the feature is off, which takes the nine comparison taps
    // and the matrix multiply with it. `firstLayer < 0` is the runtime case: the light did
    // not fit the atlas, so it lights without occluding.
    if (!ENABLE_PUNCTUAL_SHADOWS || firstLayer < 0) return 1.0;

    // Normal offset in world units, scaled to what a texel actually covers here. A cube
    // face is 90 degrees across 1024 texels, so at `distToLight` a texel spans
    // 2*dist/1024. A fixed multiple of a *UV* texel has no idea how far away anything is,
    // and under-offsetting reads as acne.
    float texelWorld = distToLight * PUNCTUAL_TEXEL * 2.0;

    float ndotl = clamp(dot(N, L), 0.0, 1.0);
    float slope = clamp(1.0 - ndotl, 0.0, 1.0);

    // Both biases are applied here, in world units, *before* the projection -- the depth
    // one along the light ray, the normal one along the surface. Subtracting a constant
    // from the projected depth instead cannot work for a perspective shadow map: its
    // depth is 1/d-shaped, so a fixed offset buys a distance growing with the square of
    // range. For lights with near planes of centimetres and far planes of tens of metres
    // that was worth *metres* out where they actually light anything, and a shadow test
    // that ignores the nearest few metres of occluders lights the room through the floor.
    vec3 offsetPos = worldPos + N * frame.shadowParams.z * texelWorld
                              + L * texelWorld * (1.0 + slope * 4.0) * 3.0;

    int layer = firstLayer;
    if (type == LIGHT_POINT) {
        // The face is picked from the vector the lookup actually uses -- light to
        // *offset* position -- not from the fragment's own direction. The two differ by
        // the normal offset, which is a fraction of a texel everywhere except along a
        // face boundary, and there it is enough to put them on opposite sides: the face
        // gets chosen from one point and the projection done from the other, landing
        // outside the face that was chosen. That reads as "no shadow information" and
        // draws a bright line along every 45-degree plane through the lamp.
        vec3 lightPos = worldPos + L * distToLight;
        layer += cubeFace(offsetPos - lightPos);
    }

    vec4 lightSpace = shadowMatrices[layer] * vec4(offsetPos, 1.0);
    if (lightSpace.w <= 0.0) return 1.0;
    vec3 proj = lightSpace.xyz / lightSpace.w;

    if (any(lessThan(proj.xy, vec2(-1.0))) || any(greaterThan(proj.xy, vec2(1.0)))) return 1.0;
    if (proj.z < 0.0 || proj.z > 1.0) return 1.0;

    vec2 uv = proj.xy * 0.5 + 0.5;

    // No bias subtracted here: both are already in `offsetPos`. This is a clamp into the
    // range the comparison accepts, not a bias.
    float ref = clamp(proj.z, 0.0, 1.0);

    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(float(x), float(y)) * PUNCTUAL_TEXEL;
            // Held inside the face. A cube face's neighbour is a different array layer,
            // so a tap that walks off the edge cannot follow it there -- it leaves the
            // image, and the sampler's white border answers "lit". Eight of the nine taps
            // can be outside on a boundary fragment, which is a second bright line along
            // the same 45-degree planes. A full texel of inset, because each tap is a
            // hardware 2x2 whose footprint reaches half a texel further.
            //
            // The kernel goes one-sided in that last texel rather than seamless: doing it
            // properly means a cube array and filtering across faces, and that costs the
            // spots their place in the same atlas.
            vec2 tap = clamp(uv + offset, PUNCTUAL_TEXEL, 1.0 - PUNCTUAL_TEXEL);
            sum += texture(punctualShadowMap, vec4(tap, float(layer), ref));
        }
    }
    return sum / 9.0;
}

/**
 * @return 1.0 fully lit, 0.0 fully shadowed.
 *
 * The sampler compares, so `texture()` returns an already-filtered visibility rather than
 * a depth: each tap is a hardware 2x2 PCF and the 3x3 loop is 6x6 effective taps. At
 * 4096 over Sponza's 37 m that kernel spans about 5 cm, which reads as a soft contact
 * rather than a stair-step without pretending to be a real penumbra.
 */
float shadowFactor(vec3 worldPos, vec3 N, vec3 L) {
    if (!ENABLE_SHADOWS) return 1.0;

    // Along the normal, not along the light. A depth bias large enough to kill acne on
    // its own is always large enough to detach contact shadows and make objects float;
    // moving the lookup across the surface instead buys the same margin without it.
    // Scaled by how edge-on the surface is to the sun, which is where acne appears.
    float ndotl = clamp(dot(N, L), 0.0, 1.0);
    float slope = clamp(1.0 - ndotl * ndotl, 0.0, 1.0);
    vec3 offsetPos = worldPos + N * frame.shadowParams.z * (1.0 + slope * 2.0);

    vec4 lightSpace = frame.sunViewProj * vec4(offsetPos, 1.0);
    vec3 proj = lightSpace.xyz / lightSpace.w;

    // Outside the box entirely: lit. Clamping instead would smear the border texel
    // across everything beyond the fitted bounds.
    if (any(lessThan(proj.xy, vec2(-1.0))) || any(greaterThan(proj.xy, vec2(1.0)))) return 1.0;
    if (proj.z < 0.0 || proj.z > 1.0) return 1.0;

    vec2 uv = proj.xy * 0.5 + 0.5;

    // Slope-scaled: a surface edge-on to the light spans more depth per texel, so the
    // bias it needs grows as N.L falls.
    float bias = frame.shadowParams.y * (1.0 + slope * 4.0);
    float ref = clamp(proj.z - bias, 0.0, 1.0);

    float texel = frame.shadowParams.x;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(shadowMap, vec3(uv + vec2(float(x), float(y)) * texel, ref));
        }
    }
    return sum / 9.0;
}
