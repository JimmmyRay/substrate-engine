#version 450
#extension GL_GOOGLE_include_directive : require

/**
 * Screen-space motion correction for TAA's dynamic tier (3.4).
 *
 * ## What this writes, and why it is not a velocity
 *
 * `taa.comp` reprojects every pixel by unprojecting its depth and pushing the world
 * point through the previous camera. For anything that did not move, that is exactly
 * right, and it has been the whole of TAA's reprojection since 3.4 landed. What it
 * cannot know is that the *object* moved as well as the camera.
 *
 * So this pass does not write a velocity. It writes the **difference between where
 * depth reprojection will land and where the surface actually was** -- a correction
 * `taa.comp` adds to the reprojection it already computes. The point is the clear
 * value: a correction of zero means "the reprojection was right", which is precisely
 * what static geometry needs and precisely what an untouched texel holds. A sentinel
 * would have needed a magic magnitude and a branch per pixel to recognise, and would
 * have had to pick a number no real motion could reach; zero is the identity here and
 * no draw has to write it.
 *
 * That is what makes the cost at zero dynamic objects a clear and nothing else.
 *
 * ## What it does not capture
 *
 * `prevModels[]` is a per-*instance* transform, so a skinned or morphed instance gets
 * the motion of its object and not of its deformation -- a character walking across a
 * room reprojects correctly, an arm swinging within a stationary body does not. The
 * honest fix is double-buffered deformed vertices, which doubles the skinned vertex
 * buffer to correct a residual that the neighbourhood clamp already fails safe on.
 * Stated rather than hidden, in the same spirit as the blended-surface case above it.
 */

#include "frame.glsl"
#include "instance.glsl"

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec4 vPrevClipTrue;
layout(location = 1) out vec4 vPrevClipFromDepth;

void main() {
    Instance inst = instances[gl_InstanceIndex];

    vec4 world = inst.model * vec4(inPosition, 1.0);
    vec4 prevWorld = prevInstances[gl_InstanceIndex] * vec4(inPosition, 1.0);

    // Where the surface actually was, and where reprojecting this frame's depth will
    // decide it was. Both through the *unjittered* previous matrix, because that is
    // the matrix `taa.comp` reprojects with -- correcting a jittered reprojection with
    // an unjittered correction would reintroduce the half-pixel drift that pass spent
    // a paragraph getting rid of.
    vPrevClipTrue = frame.prevViewProj * prevWorld;
    vPrevClipFromDepth = frame.prevViewProj * world;

    // Rasterised with the jittered matrix, like every other geometry pass this frame.
    // Coverage has to match the G-buffer exactly: a correction written at a pixel the
    // G-buffer resolved to a different surface is worse than no correction at all.
    gl_Position = frame.viewProj * world;
}
