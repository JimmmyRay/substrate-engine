/**
 * Shared cubemap and importance-sampling helpers for the IBL chain.
 *
 * Face indexing follows the Vulkan cube convention (+X -X +Y -Y +Z -Z), because the
 * storage views these shaders write through are 2D_ARRAY views of a cube image and
 * layer N *is* face N.
 */

const float PI = 3.14159265359;

/// Direction through the centre of texel (uv in [0,1]) on `face`.
vec3 cubeDirection(uint face, vec2 uv) {
    vec2 t = uv * 2.0 - 1.0;
    if (face == 0u) return normalize(vec3(1.0, -t.y, -t.x));
    if (face == 1u) return normalize(vec3(-1.0, -t.y, t.x));
    if (face == 2u) return normalize(vec3(t.x, 1.0, t.y));
    if (face == 3u) return normalize(vec3(t.x, -1.0, -t.y));
    if (face == 4u) return normalize(vec3(t.x, -t.y, 1.0));
    return normalize(vec3(-t.x, -t.y, -1.0));
}

/// Hammersley point set: a low-discrepancy sequence, so a few hundred samples cover
/// the hemisphere far more evenly than the same count drawn at random.
float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint i, uint n) { return vec2(float(i) / float(n), radicalInverseVdC(i)); }

/// GGX importance sample: returns a half-vector distributed by the NDF, which is what
/// makes the prefilter converge in hundreds of samples instead of millions.
vec3 importanceSampleGGX(vec2 xi, vec3 N, float roughness) {
    float a = roughness * roughness;

    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 h = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangentX = normalize(cross(up, N));
    vec3 tangentY = cross(N, tangentX);
    return normalize(tangentX * h.x + tangentY * h.y + N * h.z);
}
