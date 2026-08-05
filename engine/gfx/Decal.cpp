#include "gfx/Decal.h"

#include <cmath>

namespace gfx {

Decal decalAt(const glm::vec3& position, const glm::vec3& normal, float size, uint32_t textureIndex,
              const glm::vec4& tint) {
    Decal d;
    d.textureIndex = textureIndex;
    d.tint = tint;

    glm::vec3 up{0.0f, 1.0f, 0.0f};
    const float length = glm::length(normal);
    if (length > 1e-6f) up = normal / length;
    // The world axis least parallel to `up`, so the cross product is well conditioned.
    const glm::vec3 seed = std::abs(up.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(seed, up));
    const glm::vec3 forward = glm::cross(up, right);

    const float extent = std::max(size, 1e-4f);
    d.transform[0] = glm::vec4(right * extent, 0.0f);
    d.transform[1] = glm::vec4(up * extent, 0.0f);
    d.transform[2] = glm::vec4(forward * extent, 0.0f);
    d.transform[3] = glm::vec4(position, 1.0f);
    return d;
}

} // namespace gfx
