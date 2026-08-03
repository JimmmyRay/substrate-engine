// The sRGB transfer function, for the one direction the engine needs it in.
//
// Two shaders want it -- overlay.frag for a theme colour and sprite.frag for a sprite
// tint -- and both want it for the same reason, so it lives here rather than in two
// places. That is principles.md's shader rule ("a formula that two shaders need lives in
// a .glsl both include, at the narrowest scope that reaches them"), and this is the
// narrowest scope: nothing else in engine/shaders/ converts a colour by hand, because
// every texture in the engine is loaded as _SRGB and decoded by the sampler.

#ifndef SRGB_GLSL
#define SRGB_GLSL

/**
 * Vertex and instance colours are authored in sRGB -- they are the numbers a person picks
 * a theme or a tint with -- and the target is an _SRGB format, so the hardware encodes
 * linear to sRGB on write. Without this that encode is applied to a value which was never
 * linear and every colour arrives far too light: S6's panel background, a 0x17 grey,
 * measured 0x55 on screen and read as half transparent over a bright scene.
 *
 * Converted here rather than on the CPU, and that is the point of the note. The colour is
 * R8G8B8A8, so converting before packing would quantise the *linear* value into eight bits
 * -- and linear eight-bit has almost no resolution in exactly the dark greys a panel is
 * made of, where 0x17 sRGB lands on 3/255 and comes back as 0x1E. In the shader the
 * conversion happens after the value is normalised to float, which costs one pow on the
 * few thousand fragments these passes cover and loses nothing.
 *
 * Alpha is deliberately not passed through it: sRGB encodes the colour channels only, and
 * blending with an encoded alpha would make every fade non-linear in a way nobody asked
 * for.
 */
vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

#endif
