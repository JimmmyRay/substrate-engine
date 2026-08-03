#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

// The atlas is R8 coverage, not colour: the glyph's shape modulates alpha and the
// vertex supplies the tint. That is what lets one draw emit black shadow quads, white
// text quads and S6's solid rectangles from the same buffer -- a rect is a quad whose
// texcoords sit on the atlas's reserved solid block, so coverage is 1 and the vertex
// colour comes through unchanged.
//
// C5 made that atlas slot zero of an array rather than a binding of its own, so one
// pipeline draws text, rectangles and images. The array is the *overlay's*, not the
// scene's: the scene's bindless textures live in a set that does not exist until
// setScene, and the overlay draws the status text a user reads while a scene is still
// opening. Independence from scene lifetime is worth more than the sharing was.

// `srgbToLinear`, which sprite.frag wants for the same reason this does: a tint a person
// picked, multiplied into a texel the hardware already decoded. Two callers, one file, per
// principles.md's shader rule.
#include "srgb.glsl"

layout(set = 0, binding = 0) uniform sampler2D images[];

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) flat in uint vTexture;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(images[nonuniformEXT(vTexture)], vUv);

    // Slot zero is the atlas, and it is the one slot read as coverage: R8, no colour of
    // its own. Every other slot is an image with its own RGB, which the vertex colour
    // tints -- white leaves it alone, which is what `DrawList::image` defaults to.
    //
    // Images are uploaded as _SRGB, so the hardware has already decoded `texel.rgb` to
    // linear by the time it arrives here; the vertex colour has not been, which is why
    // only one side of this multiply is converted.
    vec4 src = vTexture == 0u ? vec4(1.0, 1.0, 1.0, texel.r) : texel;
    outColor = vec4(srgbToLinear(vColor.rgb) * src.rgb, vColor.a * src.a);
}
