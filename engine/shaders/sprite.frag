#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

/**
 * The sprite, and nothing else. No lighting -- that is P6, and it goes through the
 * G-buffer for it.
 *
 * ## The division happens here, once, against the image that is actually resident
 *
 * `SpriteDesc::uv` is in **texels**, because an atlas an artist authored is measured in
 * pixels in the tool that made it and normalised coordinates in a public API are where
 * half-texel errors come from. Nothing on the CPU knows how many texels the file has --
 * `ImageTable` holds the lifetime and the renderer holds the pixels -- so the divide is
 * here, where `textureSize` is the file's own answer. A zero width or height means the
 * whole image, resolved for the same reason: the call site cannot know the size.
 *
 * ## Premultiplied out, straight in
 *
 * The pipeline blends `src + dst * (1 - src.a)`, so this writes premultiplied. The texel
 * is straight alpha in the file and the tint is a straight sRGB colour, which is why only
 * one side of the multiply is converted -- the image was uploaded as _SRGB and the
 * hardware decoded it before it arrived.
 */

#include "srgb.glsl"

/// The engine's image array (P1) -- the same set the overlay binds, with the same
/// fallback: slot zero is the font atlas, so a handle that resolved to nothing draws
/// something visible and harmless instead of reading a descriptor nobody wrote.
layout(set = 0, binding = 0) uniform sampler2D images[];

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec4 vTint;
layout(location = 2) flat in uint vImage;
layout(location = 3) flat in vec4 vUvRect;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 size = vec2(textureSize(images[nonuniformEXT(vImage)], 0));

    vec2 rectMin = vUvRect.xy;
    vec2 rectSize = vUvRect.zw;
    if (rectSize.x <= 0.0) {
        rectMin.x = 0.0;
        rectSize.x = size.x;
    }
    if (rectSize.y <= 0.0) {
        rectMin.y = 0.0;
        rectSize.y = size.y;
    }

    // At 1:1 the fragment centre of destination texel k lands on `rectMin + k + 0.5`,
    // which is the centre of source texel k -- so a nearest tap returns that texel and
    // nothing else. That is what "a texel authored is a texel presented" reduces to at
    // this end of the frame, and `pixelExact` is what puts a nearest sampler here.
    vec4 texel = texture(images[nonuniformEXT(vImage)], (rectMin + vCorner * rectSize) / size);

    vec3 rgb = srgbToLinear(vTint.rgb) * texel.rgb;
    float a = vTint.a * texel.a;
    outColor = vec4(rgb * a, a);
}
