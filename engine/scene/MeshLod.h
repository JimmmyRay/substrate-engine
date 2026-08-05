#pragma once

#include "scene/SceneTypes.h"

#include <glm/glm.hpp>

#include <cstdint>

/**
 * @file engine/scene/MeshLod.h
 * @brief Generating LOD chains, and the arithmetic that selects between them.
 *
 * **Generation runs offline.** `buildLodChains` is called by the bake and by nothing else,
 * so a load straight from a document has no chains at all and draws every primitive at
 * level 0 -- see `Primitive::lods`, where zero levels is the default.
 *
 * **Selection runs per command, on the GPU.** What lives here is its arithmetic: the
 * thresholds, and the comparison `cull.comp` mirrors in three lines. The numbers stay on
 * this side and arrive as push constants, so the shader carries no constant a unit test
 * cannot reach.
 */
namespace scene {

struct SceneData;

/**
 * @brief Below this many indices a chain is not worth having.
 *
 * A thousand triangles: a mesh that small is cheaper to draw than to decide about. Lowering
 * it pulls small props into the selection path, which they are otherwise kept out of
 * entirely -- a primitive with no chain draws at level 0 at every distance.
 */
inline constexpr uint32_t kLodMinIndices = 3000;

/**
 * @brief Relative error `meshopt_simplify` may introduce, as a fraction of mesh extents.
 *
 * Five percent: loose for a mesh filling the screen, invisible at the coverage that selects
 * a level at all. `meshopt_simplify` treats it as a ceiling and stops short of the triangle
 * target rather than exceeding it, which is why a level that came back barely smaller is
 * dropped rather than kept.
 */
inline constexpr float kLodTargetError = 0.05f;

/**
 * @brief Simplify every primitive worth simplifying, and record the levels on it.
 *
 * Appends each level's indices to `data.indices`, the same array level 0 lives in, so a whole
 * chain is one allocation, one upload and one range for `unloadModel` to give back. Levels
 * share the vertex buffer -- `meshopt_simplify` returns indices into the original vertex
 * array -- so a chain costs indices and no vertices.
 *
 * Each level simplifies the level above it rather than the original, which is what keeps the
 * silhouette degrading monotonically instead of three independent reductions disagreeing
 * about which features to keep.
 *
 * Skips **deforming** primitives, which the renderer gives an infinite bounding box and
 * `cull.comp` therefore never selects for; **blended** ones, whose commands the forward pass
 * builds on the CPU without running `cull.comp` at all; and anything under `kLodMinIndices`.
 *
 * @return how many primitives came out carrying at least one level.
 */
uint32_t buildLodChains(SceneData& data);

/**
 * @brief Coverage below which each level past 0 is selected, from one base threshold.
 *
 * Component `i` is the threshold for entering level `i + 1`; w is unused and exists because
 * this is pushed to `cull.comp` as a `vec4`. Each level is a quarter of the one before it:
 * a level halves the triangle count, and halving the *linear* size on screen quarters the
 * area, so the sequence holds triangles per pixel roughly constant.
 */
[[nodiscard]] glm::vec4 lodCoverageThresholds(float base);

/**
 * @brief Which level a coverage selects, out of `levels` (which is at least 1).
 *
 * `coverage` is the fraction of the viewport the instance's projected bounds cover: the area
 * of the screen-space rectangle around the eight transformed corners, **unclamped**, so an
 * object mostly off screen is judged by its size and not by how much is in frame.
 *
 * **`cull.comp` mirrors this in three lines.** A change here that is not made there selects
 * a different level on the GPU than any test on this side can see.
 */
[[nodiscard]] uint32_t lodForCoverage(float coverage, const glm::vec4& thresholds, uint32_t levels);

} // namespace scene
