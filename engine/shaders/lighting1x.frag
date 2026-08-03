#version 450
#extension GL_GOOGLE_include_directive : require

// No GBUFFER_MULTISAMPLE: single-sampled G-buffer, the honest 1x baseline.

#include "features.glsl"
#include "frame.glsl"
#include "octahedral.glsl"
#include "pbr.glsl"
#include "ibl.glsl"
#include "shadow.glsl"
#include "lighting_body.glsl"
