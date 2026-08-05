#pragma once

#include <glm/glm.hpp>

#include <cstdint>

/**
 * @file engine/gfx/Decal.h
 * @brief A projected decal, in a header with no Vulkan in it.
 *
 * Separate from `Renderer.h` so it can be compiled into `SUBSTRATE_HOSTED_SOURCES` and
 * unit tested; adding a Vulkan include here takes that away.
 */
namespace gfx {

/// One projected decal. Must match the `Decal` struct the decal shaders read.
struct Decal {
    /// World transform of the volume: translation, rotation and size. The decal occupies
    /// [-0.5, 0.5] on each axis of its own space and projects along local Y.
    glm::mat4 transform{1.0f};
    glm::vec4 tint{1.0f};
    uint32_t textureIndex = 0;
    /// Fraction of the half-extent over which alpha ramps to zero at the boundary.
    float edgeFade = 0.25f;
    /// Project the disc inscribed in the footprint instead of the whole square. Narrows
    /// the footprint, not the depth, so a disc on a thin wall still stops at the far side.
    bool round = false;
};

/**
 * @brief A decal facing out of a surface at a point.
 *
 * @param normal the surface normal to project along. A decal projects down its local Y, so
 *        this becomes its local +Y. A zero normal leaves it unrotated.
 * @param size edge length of the square footprint. The projection depth is the same,
 *        which is what stops a decal on a thin wall bleeding through to the far side.
 */
[[nodiscard]] Decal decalAt(const glm::vec3& position, const glm::vec3& normal, float size,
                            uint32_t textureIndex = 0, const glm::vec4& tint = glm::vec4(1.0f));

} // namespace gfx
