#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

/**
 * Which curve maps scene-referred radiance to display. A *non-boolean* constant, and
 * deliberately: the three operators are mutually exclusive, so one selector gives one
 * variant per operator where three booleans would have given eight -- five of them
 * nonsense. Must match TonemapOperator in Renderer.h.
 */
layout(constant_id = 0) const uint TONEMAP_OPERATOR = 0; // 0 ACES, 1 Reinhard, 2 clamp

/// The bloom composite. `frame.params.w` remains the strength; the constant is what
/// decides whether the chain is sampled at all.
layout(constant_id = 1) const bool ENABLE_BLOOM = true;

layout(set = 1, binding = 0) uniform sampler2D hdrColor;
layout(set = 1, binding = 1) uniform sampler2D bloomChain;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Narkowicz's ACES filmic approximation.
vec3 acesFilmic(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 applyTonemap(vec3 x) {
    // Reinhard, for comparison against ACES rather than as a serious alternative: it
    // desaturates highlights where ACES holds their hue, and seeing the two side by
    // side is the point of having the selector at all.
    if (TONEMAP_OPERATOR == 1u) return x / (x + 1.0);
    // Straight clamp: no curve at all, so everything above 1.0 blows out. The honest
    // "what does the HDR buffer actually contain" view.
    if (TONEMAP_OPERATOR == 2u) return clamp(x, 0.0, 1.0);
    return acesFilmic(x);
}

void main() {
    vec3 hdr = texture(hdrColor, vUV).rgb;

    // Bloom is added before exposure and before the tonemap, because that is where it
    // physically belongs: the scattering it stands in for happens in the lens, on
    // scene-referred radiance. Adding it after the tonemap would let it push already
    // clipped highlights past white and wash the image out instead of blooming it.
    // params.w is the strength; 0 leaves the composite a no-op without a rebuild.
    if (ENABLE_BLOOM) {
        hdr += textureLod(bloomChain, vUV, 0.0).rgb * frame.params.w;
    }
    hdr *= frame.params.x;

    // Debug views are already display-referred; tonemapping them would misrepresent
    // the values being inspected. A uniform test, not a constant: the debug view
    // changes with a keypress and is not worth a pipeline per view.
    vec3 mapped = frame.flags.x != 0u ? clamp(hdr, 0.0, 1.0) : applyTonemap(hdr);

    // The swapchain is _SRGB, so the hardware applies the transfer function on write.
    outColor = vec4(mapped, 1.0);
}
