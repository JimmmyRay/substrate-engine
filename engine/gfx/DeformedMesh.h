#pragma once

#include "scene/SceneTypes.h"

#include <cstdint>
#include <span>

/**
 * @file engine/gfx/DeformedMesh.h
 * @brief What the deformed-vertex upload reads about one mesh a solver reshapes.
 *
 * The renderer's half of cloth, so that a pass sizing a staging buffer and a copy region name
 * no solver -- see `engine/Modules.h`.
 */
namespace gfx {

/**
 * @brief One mesh whose vertices arrive by transfer rather than out of the skinning dispatch.
 *
 * **The span is the solver's own storage and must be re-taken whenever a body is placed.**
 * Placing one can move every mesh already tracked, and a span held across it copies freed
 * memory into the vertex buffer, which draws a plausible surface rather than crashing.
 */
struct DeformedMesh {
    /// The instance these vertices deform. `Renderer::skinDestBase[instance]` is where they
    /// land in the deformed vertex buffer.
    uint32_t instance = 0xFFFFFFFFu;
    /// The solved mesh, rewritten in place once a frame after the steps.
    std::span<const scene::Vertex> vertices;
};

} // namespace gfx
