#include "gfx/Light.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gfx {

GpuLight makeDirectionalLight(const glm::vec3& towardLight, const glm::vec3& color, float intensity) {
    GpuLight l{};
    l.position = glm::vec4(0.0f);
    // Stored unnegated although the field means "the direction the light points": the
    // shader negates it to recover L, so negating here too aims the sun backwards.
    l.direction = glm::vec4(glm::normalize(towardLight), 0.0f);
    l.color = glm::vec4(color, intensity);
    l.params = glm::vec4(0.0f, 0.0f, static_cast<float>(LightType::Directional), 0.0f);
    return l;
}

GpuLight makePointLight(const glm::vec3& position, float range, const glm::vec3& color, float intensity) {
    GpuLight l{};
    l.position = glm::vec4(position, range);
    l.direction = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
    l.color = glm::vec4(color, intensity);
    l.params = glm::vec4(0.0f, 0.0f, static_cast<float>(LightType::Point), 0.0f);
    return l;
}

GpuLight makeSpotLight(const glm::vec3& position, const glm::vec3& direction, float range, float innerAngle,
                       float outerAngle, const glm::vec3& color, float intensity) {
    GpuLight l{};
    l.position = glm::vec4(position, range);
    l.direction = glm::vec4(glm::normalize(direction), 0.0f);
    l.color = glm::vec4(color, intensity);
    // Cosines, not angles -- the shader compares these against a dot product directly.
    l.params = glm::vec4(std::cos(innerAngle), std::cos(outerAngle), static_cast<float>(LightType::Spot), 0.0f);
    return l;
}

float lightImportance(const GpuLight& light, const glm::vec3& viewPosition) {
    const auto type = static_cast<LightType>(static_cast<uint32_t>(light.params.z));
    if (type == LightType::Directional) return std::numeric_limits<float>::infinity();

    // Rec. 709 luma: ranking on `color.w` alone would score a blue light and a green one
    // of equal intensity as equally visible.
    const float luminance =
        (0.2126f * light.color.r + 0.7152f * light.color.g + 0.0722f * light.color.b) * light.color.w;

    const float d2 = glm::dot(glm::vec3(light.position) - viewPosition, glm::vec3(light.position) - viewPosition);

    // A metre, not an epsilon: an epsilon lets a light the camera walks through outrank
    // every other by ten orders of magnitude and evict the scene's lighting for a frame.
    return luminance / std::max(d2, 1.0f);
}

Frustum extractFrustum(const glm::mat4& m) {
    // Gribb-Hartmann: each plane is a sum or difference of two rows of the matrix. glm is
    // column-major, so row i is (m[0][i], m[1][i], m[2][i], m[3][i]).
    const glm::vec4 rowX(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 rowY(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 rowZ(m[0][2], m[1][2], m[2][2], m[3][2]);
    const glm::vec4 rowW(m[0][3], m[1][3], m[2][3], m[3][3]);

    Frustum f;
    f.planes[0] = rowW + rowX; // left
    f.planes[1] = rowW - rowX; // right
    f.planes[2] = rowW + rowY; // bottom
    f.planes[3] = rowW - rowY; // top
    // Reverse-Z infinite projection: near is w - z and there is no far plane, so the sixth
    // repeats the fifth. A plane at infinity here rejects everything.
    f.planes[4] = rowW - rowZ;
    f.planes[5] = rowW - rowZ;

    for (glm::vec4& p : f.planes) {
        const float length = glm::length(glm::vec3(p));
        // 1e-12, and left unnormalised below it: dividing by a degenerate row makes every
        // test a NaN compare, which reads as "cull everything". The half-space sign
        // survives an unnormalised plane; only the distance it reports does not.
        if (length > 1e-12f) p /= length;
    }
    return f;
}

bool lightVisible(const GpuLight& light, const Frustum& frustum) {
    const auto type = static_cast<LightType>(static_cast<uint32_t>(light.params.z));
    if (type == LightType::Directional) return true;

    const float range = light.position.w;
    // Zero means unbounded -- GpuLight::position documents it -- so it reaches everywhere.
    if (range <= 0.0f) return true;

    const glm::vec3 centre(light.position);
    for (const glm::vec4& p : frustum.planes) {
        // Per-plane, so a sphere outside the frustum but behind no single plane survives.
        // That is the safe direction: it shades a light contributing nothing rather than
        // dropping one that does.
        if (glm::dot(glm::vec3(p), centre) + p.w < -range) return false;
    }
    return true;
}

} // namespace gfx
