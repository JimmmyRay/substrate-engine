#version 450

/**
 * Add a screen-space radiance buffer into the lit HDR target.
 *
 * Shared by screen-space reflections (3.1) and volumetric fog (3.2), which differ only
 * in the blend state their pipelines are built with -- additive for a reflection,
 * premultiplied-over for fog, which has to dim what is behind it as well as add to it.
 * The shader itself has nothing to choose between them: it hands the texel over and the
 * blend does the rest.
 *
 * Both run before bloom and TAA, so what they add glares and antialiases like any other
 * scene radiance. Composited at the end they would do neither.
 */

layout(set = 0, binding = 0) uniform sampler2D sourceColor;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() { outColor = texture(sourceColor, vUV); }
