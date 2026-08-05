#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

/**
 * @file engine/gfx/Skin.h
 * @brief What the skinning dispatch reads about one deformed character.
 *
 * The renderer's half of animation, so that a pass sizing a buffer and a dispatch reading
 * joints name no animator -- see `engine/Modules.h`.
 */
namespace gfx {

/**
 * @brief One character's place in the flat joint and weight numbering, and the blocks it
 *        fills this frame.
 *
 * **The spans are the animator's own storage and must be re-taken whenever a character is
 * created or destroyed.** Creating one moves both blocks; a span held across it uploads
 * freed memory as a pose, which draws a plausible character rather than crashing.
 */
struct SkinCharacter {
    /// Where this slot's joints begin in the one flat numbering `skinning.comp` indexes
    /// through its `jointBase` push constant. Belongs to the slot, and never moves.
    uint32_t jointOffset = 0;
    /// The same, for the morph weights of the placement's own run.
    uint32_t weightOffset = 0;
    std::span<const glm::mat4> joints;
    std::span<const float> weights;
};

} // namespace gfx
