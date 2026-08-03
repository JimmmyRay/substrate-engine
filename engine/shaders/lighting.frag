#version 450
#extension GL_GOOGLE_include_directive : require

#define GBUFFER_MULTISAMPLE 1

#include "features.glsl"
#include "frame.glsl"
#include "octahedral.glsl"
#include "pbr.glsl"
#include "ibl.glsl"
#include "shadow.glsl"
#include "lighting_body.glsl"
