#version 450
#extension GL_GOOGLE_include_directive : require

/// Multisampled G-buffer depth. Sample 0 only: this decides which side of a silhouette a
/// low-resolution reflection texel belongs to, and the reflection itself was traced from
/// sample 0 in the first place.
#define GDEPTH_SAMPLER sampler2DMS
#define GDEPTH_FETCH(tex, coord) texelFetch(tex, coord, 0)
/// No lod argument on a multisampled sampler; sampler2D requires one.
#define GDEPTH_SIZE(tex) textureSize(tex)

#include "ssr_upsample_body.glsl"
