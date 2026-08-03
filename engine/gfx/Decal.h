#pragma once

#include <glm/glm.hpp>

#include <cstdint>

/**
 * @file engine/gfx/Decal.h
 * @brief A projected decal, in a header with no Vulkan in it.
 *
 * Split out of `Renderer.h` for the reason `DebugView.h` was: everything in that header
 * reaches `VkDevice`, so nothing in it can be compiled into `SUBSTRATE_HOSTED_SOURCES` and
 * nothing in it can be unit tested. A decal is four plain fields and one piece of
 * arithmetic, and the arithmetic is worth a test.
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
    /**
     * @brief Project the disc inscribed in the footprint instead of the whole square.
     *
     * A field rather than a texture with a circular alpha in it, because the texture a
     * decal samples comes out of the *scene's* bindless array -- it is a glTF texture, and
     * a game that wants a round mark it did not author into a document has no way to put
     * one there. The shape is one `length()` in `decal.frag`; the alternative is a texture
     * upload path for a picture of a circle.
     *
     * The projection axis is unaffected: this narrows the footprint, not the depth, so a
     * disc on a thin wall still stops at the far side.
     */
    bool round = false;
};

/**
 * @brief A decal facing out of a surface at a point (C3).
 *
 * The other half of "spawn an effect where something happened":
 * `renderer.decals.push_back(gfx::decalAt(hit.point, hit.normal, 0.5f))` is a bullet hole.
 * Before this, a decal could only come from `GameSetup::Decal` -- authored in C++ at
 * startup, which is fine for the scorch marks in a sample scene and useless for a hit.
 *
 * **A free function rather than a handle table**, and that is the deliberate half. A decal
 * list is a `std::vector<Decal>` the game owns and can erase from, so it already satisfies
 * "everything creatable is destroyable"; giving `Renderer` a slot table with generations
 * would be lifetime machinery in the one class CLAUDE.md most wants free of it. What was
 * actually missing was the arithmetic, which is a function.
 *
 * @param normal the surface normal to project along. A decal projects down its local Y, so
 *        this becomes its local +Y. A zero normal leaves it unrotated.
 * @param size edge length of the square footprint. The projection depth is the same,
 *        which is what stops a decal on a thin wall bleeding through to the far side.
 */
[[nodiscard]] Decal decalAt(const glm::vec3& position, const glm::vec3& normal, float size,
                            uint32_t textureIndex = 0, const glm::vec4& tint = glm::vec4(1.0f));

} // namespace gfx
