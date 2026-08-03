#version 450

/**
 * The perspective divide, and nothing else. See velocity.vert for what the two clip
 * positions mean and why the output is a correction rather than a velocity.
 *
 * The divide is here rather than in the vertex stage because it is not affine: a
 * screen-space position interpolated before dividing is wrong everywhere except the
 * vertices, and the error is largest exactly where a surface is most oblique.
 */

layout(location = 0) in vec4 vPrevClipTrue;
layout(location = 1) in vec4 vPrevClipFromDepth;

layout(location = 0) out vec2 outCorrection;

void main() {
    // Reverse-Z infinite projection: w is the view-space distance, positive for
    // anything in front of the camera. A vertex that was behind the previous camera
    // has no previous screen position at all, so the correction is left at zero and
    // `taa.comp`'s own offscreen test rejects the history -- which is the same answer
    // by a shorter route than inventing a projected position for it.
    if (vPrevClipTrue.w <= 0.0 || vPrevClipFromDepth.w <= 0.0) {
        outCorrection = vec2(0.0);
        return;
    }

    vec2 trueUv = (vPrevClipTrue.xy / vPrevClipTrue.w) * 0.5 + 0.5;
    vec2 depthUv = (vPrevClipFromDepth.xy / vPrevClipFromDepth.w) * 0.5 + 0.5;
    outCorrection = trueUv - depthUv;
}
