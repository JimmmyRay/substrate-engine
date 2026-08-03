// 460, not 450 like every other shader here: glslang only recognises GL_EXT_ray_query
// at GLSL 4.60, and the failure is an undeclared-identifier error on `rayQueryEXT` that
// says nothing about the version.
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require

// Multisampled G-buffer, with traced shadows available.
//
// A separate file rather than a specialisation constant inside lighting.frag, because
// what differs is a *descriptor binding*: the acceleration structure at set 2 binding 2
// exists only on a device with VK_KHR_acceleration_structure, and a layout cannot be
// specialised away. Tracing is unconditional inside this file: it is compiled only when
// the device can trace and `render.rt` asked for it, and there is no second toggle -- ray
// tracing is one switch covering shadows and reflections together.
#define GBUFFER_MULTISAMPLE 1
#define ENABLE_RAY_QUERY 1

#include "features.glsl"
#include "frame.glsl"
#include "octahedral.glsl"
#include "pbr.glsl"
#include "ibl.glsl"
#include "rayshadow.glsl"
#include "lighting_body.glsl"
