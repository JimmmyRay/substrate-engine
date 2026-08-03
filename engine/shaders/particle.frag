#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

/**
 * The sprite, and nothing else.
 *
 * There is no lighting here on purpose -- it happened once per particle in
 * particle_simulate.comp, and particle_light.glsl explains why. What is left is a mask:
 * one cell of a sheet out of the scene's bindless array, cross-faded into the next, or
 * the procedural soft disc a scene with no particle art gets.
 *
 * ## Premultiplied, in and out
 *
 * `vColor` is already premultiplied radiance, so a mask multiplies both halves of it
 * and the result is still premultiplied. The pipeline blends `src + dst * (1 - src.a)`,
 * which is alpha-over for a particle whose alpha is non-zero and plain additive for one
 * whose alpha is zero -- one blend state, therefore one draw, therefore one global sort.
 */

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) flat in uint vTexture;
layout(location = 3) flat in vec4 vFrameA;
layout(location = 4) flat in vec3 vFrameB;
layout(location = 5) flat in float vErode;

layout(set = 2, binding = 1) uniform sampler2D textures[];

layout(location = 0) out vec4 outColor;

void main() {
    if (vTexture == 0xFFFFFFFFu) {
        // A soft radial disc. Squared rather than linear, because a linear ramp has a
        // visible cone edge where it reaches zero and a puff of smoke does not.
        //
        // This is the *spark*, not the fallback for everything: a disc has no internal
        // detail, so a volume built from it reads as a heap of balls whatever the tuning.
        // Anything with structure -- flame, smoke -- names a sheet instead. See
        // systems.md, "Particles: the sheet is the effect".
        float r = clamp(length(vUV * 2.0 - 1.0), 0.0, 1.0);
        float mask = 1.0 - r;
        outColor = vColor * (mask * mask);
        return;
    }

    // **Inset by half a texel, in the cell's own space.** Without it a bilinear tap at a
    // frame's edge reaches into the neighbouring frame, which puts a sliver of an
    // unrelated puff along every sprite's border. Only for a real sheet -- a 1x1 grid has
    // nothing to bleed from and must sample its edges exactly as it always did.
    vec2 uv = vUV;
    if (vFrameA.zw != vec2(1.0)) {
        vec2 half_texel = 0.5 / (vec2(textureSize(textures[nonuniformEXT(vTexture)], 0)) * vFrameA.zw);
        uv = clamp(uv, half_texel, 1.0 - half_texel);
    }

    vec4 texel = texture(textures[nonuniformEXT(vTexture)], vFrameA.xy + uv * vFrameA.zw);
    if (vFrameB.z > 0.0) {
        vec4 next = texture(textures[nonuniformEXT(vTexture)], vFrameB.xy + uv * vFrameA.zw);
        texel = mix(texel, next, vFrameB.z);
    }

    // Erosion. The sheet's own alpha is the noise field, so the particle dissolves along
    // the detail it already has rather than dimming as one shape. Rescaled by what is
    // left, so the surviving core stays at full strength instead of fading with it --
    // dimming is what `colorEnd.a` is for and doing both here would fade it twice.
    float a = clamp((texel.a - vErode) / max(1.0 - vErode, 1e-3), 0.0, 1.0);

    // Straight alpha in the file, premultiplied on the way out: rgb tints and alpha
    // masks, so a sprite with a black surround does not add a black square to whatever
    // is behind it.
    outColor = vColor * vec4(texel.rgb, 1.0) * a;
}
