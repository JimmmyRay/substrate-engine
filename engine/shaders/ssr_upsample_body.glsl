/**
 * Joint-bilateral upsample of a reduced-resolution reflection buffer (3.1).
 *
 * Included by ssr_upsample.frag (multisampled G-buffer) and ssr_upsample1x.frag, which
 * differ only in whether GBUFFER_MULTISAMPLE is defined -- the same split ssr.comp and
 * ssr1x.comp carry, and for the same reason: a genuinely single-sample depth image
 * cannot bind to a sampler2DMS descriptor.
 *
 * Bound only when `render.ssrScale` is below 1.0. At 1.0 the pass composites through
 * composite.frag, which is a single `texture()` and must stay one.
 *
 * The blend state is the additive one composite.frag's pipeline uses, so this shader's
 * job is only to decide what value a full-resolution pixel takes from the four
 * low-resolution texels around it -- bilinear where they all sit on the same surface,
 * and weighted towards the ones that do where they do not. Plain bilinear across a
 * silhouette drags a reflection off the reflecting surface and onto whatever is behind
 * it, which at a roughness cutoff of 0.4 is a mirror edge with a halo.
 */

layout(set = 0, binding = 3) uniform GDEPTH_SAMPLER gDepth;
layout(set = 1, binding = 0) uniform sampler2D sourceColor;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

/// How far apart two depths may be, as a fraction of the nearer one, before the further
/// one stops contributing. Reverse-Z depth is proportional to 1/z, so a *relative*
/// difference in it is a relative difference in view distance -- which is what makes one
/// constant work at every range. **An absolute epsilon here does not**: it would reject
/// neighbours a centimetre apart at the near plane and accept a whole room at the far one.
const float kDepthTolerance = 0.05;

void main() {
    ivec2 fullSize = GDEPTH_SIZE(gDepth);
    ivec2 lowSize = textureSize(sourceColor, 0);

    ivec2 fullCoord = clamp(ivec2(vUV * vec2(fullSize)), ivec2(0), fullSize - 1);
    float centreDepth = GDEPTH_FETCH(gDepth, fullCoord).r;

    // The four low-resolution texels a bilinear tap would blend, and its weights for them.
    vec2 low = vUV * vec2(lowSize) - 0.5;
    ivec2 base = ivec2(floor(low));
    vec2 f = low - vec2(base);

    vec4 sum = vec4(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 offset = ivec2(i & 1, i >> 1);
        ivec2 c = clamp(base + offset, ivec2(0), lowSize - 1);
        float bilinear = (offset.x == 0 ? 1.0 - f.x : f.x) * (offset.y == 0 ? 1.0 - f.y : f.y);
        if (bilinear <= 0.0) continue;

        // The full-resolution pixel this low-resolution texel's centre traced from. It is
        // the surface the reflection in it belongs to, and the only one worth comparing.
        ivec2 source = clamp(ivec2((vec2(c) + 0.5) * vec2(fullSize) / vec2(lowSize)), ivec2(0), fullSize - 1);
        float d = GDEPTH_FETCH(gDepth, source).r;

        float relative = abs(d - centreDepth) / max(max(d, centreDepth), 1e-6);
        float depthWeight = max(0.0, 1.0 - relative / kDepthTolerance);

        float w = bilinear * depthWeight;
        sum += texelFetch(sourceColor, c, 0) * w;
        weightSum += w;
    }

    // Every neighbour rejected means this pixel is on a surface none of them sampled --
    // a one-pixel sliver, or the far side of a silhouette. Bilinear is the wrong answer
    // there and so is black; the nearest texel is at least a reflection of something at
    // this depth's own scale, and it is what the reduced buffer actually has to offer.
    outColor = weightSum > 0.0 ? sum / weightSum : texelFetch(sourceColor, clamp(base, ivec2(0), lowSize - 1), 0);
}
