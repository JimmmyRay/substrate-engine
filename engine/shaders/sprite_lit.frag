#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

/**
 * A sprite that is lit, which is a quad that goes through the G-buffer (P6).
 *
 * The opposite trade to `sprite.frag`, and the two headers state each other's rather than
 * only their own. That pass draws after the tonemap into the virtual target, so a texel in
 * the file is the texel on the screen and nothing occludes, blooms, reflects or fogs it.
 * This one writes albedo, a normal, roughness and metallic into the four attachments the
 * deferred pass reads -- so the sprite is shadowed, occluded by geometry in front of it,
 * ambient-occluded, reflected by SSR, fogged and tonemapped, and by construction is **not**
 * bit-identical to its source file. That is the whole reason to use it.
 *
 * ## What this shader adds to the standard material path, and why it is a variant
 *
 * Exactly two things, and neither is expressible as a material field:
 *
 * - **The image comes from set 2**, the array a game loaded through `e.images()` (P1), not
 *   from set 1's array, which holds what a glTF file brought. The authoring path for a
 *   sprite sheet is a PNG, not a wrapper document.
 * - **The UV rect is in texels**, and the divide by the image's size happens here. That is
 *   `sprite.frag`'s own argument arriving one pass along: `ImageTable` holds no `VkImage`,
 *   so nothing CPU-side knows the file's dimensions and `textureSize` is the only place the
 *   sentence "the engine does the division, once, against the dimensions it loaded" is true.
 *
 * A zero width or height in the rect means the whole image, resolved here for the same
 * reason -- the call site cannot know the size.
 *
 * ## Cutout, not blend
 *
 * ALPHA_MODE MASK. A blended sprite would have to go forward, after lighting, and would be
 * excluded from `dynamicCount()` and therefore from TAA motion correction. A hard alpha edge
 * is what pixel art has anyway, and the `discard` buys depth, velocity, occlusion, SSAO, SSR
 * and a cut-out shadow. The discard is before `gbufferWrite`, which the contract requires:
 * the derivatives it takes are undefined under non-uniform control flow.
 */

#include "gbuffer_contract.glsl"

/// The engine's image array (P1) -- the same slots `sprite.frag` and the overlay index, with
/// the same fallback: slot zero is the font atlas, so a material naming nothing draws
/// something visible instead of reading a descriptor nobody wrote.
layout(set = 2, binding = 0) uniform sampler2D gameImages[];

void main() {
    Material m = gbufferMaterial();

    vec2 size = vec2(textureSize(gameImages[nonuniformEXT(m.gameImage)], 0));

    // The quad's own UVs are the 0..1 corner; the rect that corner is mapped into is the
    // material's, so a frame change is one `setLitSpriteUv` and not new geometry.
    vec2 rectMin = m.params.xy;
    vec2 rectSize = m.params.zw;
    if (rectSize.x <= 0.0) {
        rectMin.x = 0.0;
        rectSize.x = size.x;
    }
    if (rectSize.y <= 0.0) {
        rectMin.y = 0.0;
        rectSize.y = size.y;
    }

    vec4 texel = texture(gameImages[nonuniformEXT(m.gameImage)], (rectMin + vUV * rectSize) / size);

    // The image was uploaded as _SRGB and the hardware decoded it before it arrived, so the
    // texel is already linear and multiplying a linear tint into it is the whole conversion.
    // `baseColorFactor` is the tint, in the same slot every other material puts it.
    float alpha = texel.a * m.baseColorFactor.a;
    if (alpha < m.alphaCutoff) discard;

    // Two-sided: a sprite is a single sheet and the variant turns culling off, so the back
    // face needs the normal it actually presents rather than the one the winding implies.
    vec3 normal = normalize(vNormal);
    if (!gl_FrontFacing) normal = -normal;

    Surface s;
    s.albedo = texel.rgb * m.baseColorFactor.rgb;
    s.normal = normal;
    s.occlusion = 1.0;
    s.roughness = m.roughnessFactor;
    s.metallic = m.metallicFactor;
    s.emissive = m.emissiveFactor.rgb * texel.rgb;
    gbufferWrite(s);
}
