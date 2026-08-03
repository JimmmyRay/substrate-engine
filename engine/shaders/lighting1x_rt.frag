// 460, not 450 like every other shader here: glslang only recognises GL_EXT_ray_query
// at GLSL 4.60, and the failure is an undeclared-identifier error on `rayQueryEXT` that
// says nothing about the version.
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require

// Single-sampled G-buffer with traced shadows -- the 1x baseline's counterpart to
// lighting_rt.frag, which is where the argument for a fourth file rather than a fourth
// specialisation constant is written down.
#define ENABLE_RAY_QUERY 1

#include "features.glsl"
#include "frame.glsl"
#include "octahedral.glsl"
#include "pbr.glsl"
#include "ibl.glsl"
#include "rayshadow.glsl"
#include "lighting_body.glsl"
