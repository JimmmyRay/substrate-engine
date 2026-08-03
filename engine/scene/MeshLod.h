#pragma once

#include "scene/SceneTypes.h"

#include <glm/glm.hpp>

#include <cstdint>

/**
 * @file engine/scene/MeshLod.h
 * @brief Generating LOD chains, and the arithmetic that selects between them (C17).
 *
 * ## Why meshoptimizer and not a quadric collapse written here
 *
 * The card left the dependency open between the two. `CLAUDE.md` settles it -- *"reach for
 * a third-party library that solves a solved problem (VMA, fastgltf, rapidjson); those are
 * dependencies, not architecture"* -- and mesh simplification is the definition of a solved
 * problem. What the conventions refuse is indirection *we* write over Vulkan, and a
 * simplifier is neither indirection nor over Vulkan: it takes an index array and returns a
 * shorter one. `external/meshoptimizer` is the standard answer to that question and the
 * only reason to reimplement it would be to own the bugs.
 *
 * ## Two halves, and they are here together on purpose
 *
 * **Generation runs offline.** `buildLodChains` is called by the bake and by nothing else,
 * which is the whole reason C15's sidecar exists: simplifying a scene is seconds of work
 * whose answer never changes between runs, and paying it on a player's machine at every
 * launch is what a cooked scene format is for. A load from a document therefore has no
 * chains at all -- see `Primitive::lods`, where zero levels is the default -- and that is
 * correct rather than a gap.
 *
 * **Selection runs per command, on the GPU.** What lives here is the arithmetic behind it:
 * the thresholds, which are a formula, and the comparison, which `cull.comp` mirrors in
 * three lines. Keeping the *numbers* on this side is what stops the shader carrying magic
 * constants nothing can test -- the thresholds are pushed, so there is one place they are
 * decided and it is one a unit test can reach.
 */
namespace scene {

struct SceneData;

// ------------------------------------------------------------------ generation

/**
 * @brief Below this many indices a chain is not worth having.
 *
 * A thousand triangles. Two things meet at that number and both point the same way: a mesh
 * this small is already cheaper to draw than to decide about, and -- the reason it is a
 * *stated* floor rather than a tuning knob -- it is what keeps small props out of the
 * selection path entirely. A prop with no chain draws at LOD 0 at every distance whatever
 * the coverage test says, which is exactly the property the golden set checks for.
 */
inline constexpr uint32_t kLodMinIndices = 3000;

/**
 * @brief Relative error `meshopt_simplify` may introduce, as a fraction of mesh extents.
 *
 * Five percent, which is loose for a mesh filling the screen and invisible for one covering
 * the fraction of it that selects a level at all. The simplifier treats it as a ceiling and
 * stops short of the triangle target rather than exceeding it, which is why a level that
 * came back barely smaller is dropped below instead of kept.
 */
inline constexpr float kLodTargetError = 0.05f;

/**
 * @brief Simplify every primitive worth simplifying, and record the levels on it.
 *
 * Appends each level's indices to `data.indices` -- the same array LOD 0 lives in, so the
 * whole chain is one allocation, one upload and one range for `unloadModel` to give back --
 * and fills `Primitive::lods` and `Primitive::lodCount`. Levels share the vertex buffer:
 * `meshopt_simplify` returns indices into the original vertex array, so a chain costs
 * indices and no vertices.
 *
 * Each level simplifies the level above it rather than the original. That is what makes it
 * a chain: level 2 is a reduction of level 1's silhouette, so the sequence degrades
 * monotonically instead of three independent reductions disagreeing about which features to
 * keep.
 *
 * Three kinds of primitive are skipped, and each for a reason the selection path already
 * has: **deforming** ones are drawn from the buffer `skinning.comp` writes and are given an
 * infinite bounding box by the renderer, so they are never culled and never selected;
 * **blended** ones are drawn by the forward pass, which builds its own commands on the CPU
 * and does not run through `cull.comp` at all; and anything under `kLodMinIndices`.
 *
 * @return how many primitives came out carrying at least one level.
 */
uint32_t buildLodChains(SceneData& data);

// ------------------------------------------------------------------- selection

/**
 * @brief Coverage below which each level past 0 is selected, from one base threshold.
 *
 * Component `i` is the threshold for entering level `i + 1`; w is unused and exists because
 * this is pushed to `cull.comp` as a `vec4`. Each level is a quarter of the one before it,
 * because a level halves the triangle count and halving the *linear* size on screen is
 * quartering the area -- so the sequence keeps a roughly constant number of triangles per
 * pixel, which is the only invariant a LOD threshold has ever been trying to hold.
 */
[[nodiscard]] glm::vec4 lodCoverageThresholds(float base);

/**
 * @brief Which level a coverage selects, out of `levels` (which is at least 1).
 *
 * `coverage` is the fraction of the viewport the instance's projected bounds cover -- the
 * area of the screen-space rectangle around the eight transformed corners, not clamped to
 * the viewport, so an object mostly off screen is still judged by its size and not by how
 * much of it happens to be in frame.
 *
 * **`cull.comp` mirrors this in three lines and says so.** Two copies rather than one is
 * the cost of the test living on the GPU and the numbers being decidable on the CPU; what
 * is not duplicated is any constant, because the thresholds arrive as a push constant.
 */
[[nodiscard]] uint32_t lodForCoverage(float coverage, const glm::vec4& thresholds, uint32_t levels);

} // namespace scene
