/**
 * Octahedral unit-vector encoding (Cigolle et al., JCGT 2014).
 *
 * Two components instead of three, which takes the G-buffer normal target from
 * RGBA16F to RG16F -- 8 bytes per sample down to 4. At 4x MSAA and 1600x900 that is
 * 23 MB per frame of attachment traffic the G-buffer pass no longer writes and the
 * lighting pass no longer reads.
 *
 * ## Why octahedral and not "just drop z"
 *
 * Storing xy and reconstructing z = sqrt(1 - x^2 - y^2) loses the sign of z, which is
 * fine for view-space normals and wrong here: these are world-space, and a wall facing
 * away from the origin is as common as one facing toward it. Octahedral folds the
 * whole sphere onto the square with no hemisphere assumption and near-uniform angular
 * error -- about 0.2 degrees worst case at this precision, against the ~0.5 degrees a
 * naive RGB8 normal gives.
 *
 * ## Why RG16F and not RG16_UNORM
 *
 * UNORM would spend its bits uniformly over the domain, which is what this encoding
 * wants, and half-float instead wastes precision near zero where the octahedral map
 * needs it least. It is still the right choice: R16G16_SFLOAT is on Vulkan's mandatory
 * colour-attachment list and R16G16_UNORM is not, and the difference is 12 effective
 * bits against 16 on a value whose consumer is a normalize(). Measured against the
 * pre-change golden, the largest channel difference in the shaded image is small
 * enough to be reported in plan/11-tier3.md rather than argued about here.
 */

vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z >= 0.0) return n.xy;
    // Lower hemisphere: fold the octahedron's bottom half out to the corners of the
    // square. The sign copy is what keeps the fold continuous across each axis.
    return (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
}

vec3 octDecode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    // Negative z means the encoded point was outside the central diamond, so undo the
    // fold. Written as a max rather than a branch: both halves are one instruction and
    // the branch would diverge across every silhouette in the frame.
    float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}
