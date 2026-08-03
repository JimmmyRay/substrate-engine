#version 450
#extension GL_GOOGLE_include_directive : require

// The 1x G-buffer is genuinely single-sampled, so its depth binds as a sampler2D and the
// lod argument is the second one rather than the sample index.
#define GDEPTH_SAMPLER sampler2D
#define GDEPTH_FETCH(tex, coord) texelFetch(tex, coord, 0)
#define GDEPTH_SIZE(tex) textureSize(tex, 0)

#include "ssr_upsample_body.glsl"
