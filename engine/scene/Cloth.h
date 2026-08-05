#pragma once

#include "scene/SceneTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/**
 * @file engine/scene/Cloth.h
 * @brief The authoring convention cloth arrives by, and the mesh maths around the solver.
 *
 * The solver itself is Jolt's, inside `physics::PhysicsWorld`, and the bookkeeping over it is
 * `physics::ClothSystem`; everything here is arithmetic, and `SceneParse.cpp` runs it before
 * a solver exists. Naming a Jolt or Vulkan type in this translation unit puts it on the
 * include path of every file that touches a scene.
 */
namespace scene {

/**
 * @brief The mesh-name prefix that makes geometry cloth, and the attribute that pins it.
 *
 * `scripts/check_pins.py` carries the only other copy of these two strings and of the 0.999
 * threshold, on the far side of the exporter. `ClothTwoProducersAgree` in
 * `tests/ClothTests.cpp` is what holds the two together; edit one without the other and the
 * exporter and the engine disagree about what cloth is.
 */
inline constexpr std::string_view kFabricPrefix = "FABRIC_";
/// The per-vertex float attribute, on the vertex domain, 1 pinned and 0 free. The leading
/// underscore is load-bearing: it is what makes Blender's stock glTF exporter pass a
/// custom mesh attribute through verbatim, and it is why no exporter patch is needed.
inline constexpr std::string_view kPinAttribute = "_PIN_WEIGHT";

/**
 * @brief Is this mesh fabric?
 *
 * Prefix only, so a bare `FABRIC_` counts -- matching Python's `str.startswith` in
 * `check_pins.py`. Requiring a suffix here makes the two producers disagree.
 *
 * Callers must pass the glTF **mesh** name (Blender's data-block), not the object name;
 * confusing the two is a file where every other check passes and the engine sees no cloth.
 */
[[nodiscard]] constexpr bool isFabricMesh(std::string_view meshName) {
    return meshName.substr(0, kFabricPrefix.size()) == kFabricPrefix;
}

/**
 * @brief The authoring weight a `_PIN_WEIGHT` attribute carries, as an inverse mass.
 *
 * The one place the two vocabularies meet: the attribute is a weight with 1 pinned, Jolt's
 * `mInvMass` is an inverse mass with 0 pinned.
 *
 * `>= 0.999` rather than `== 1.0f`: a weight that has been through a float attribute, an
 * exporter and a base64 round trip does not compare equal to one, and a curtain whose top
 * edge came back as `0.99999994` is a curtain on the floor.
 *
 * Out-of-range clamps and NaN maps to free rather than refusing -- `check_pins.py` is what
 * rejects a bad file, before the engine opens it.
 */
[[nodiscard]] constexpr float clothInvMass(float pinWeight) {
    if (!(pinWeight > 0.0f)) return 1.0f; // catches NaN too, which must not pin
    if (pinWeight >= 0.999f) return 0.0f;
    return 1.0f - pinWeight;
}

/**
 * @brief One `FABRIC_` primitive, as the loader read it, ready to become a soft body.
 *
 * Every span views scene-owned storage and must outlive the call it is passed to.
 */
struct ClothDesc {
    /// Rest pose, in the primitive's object space. `transform` is what puts it in the
    /// world.
    std::span<const Vertex> vertices;
    /// Zero-based into `vertices`, not the scene's absolute indices -- the caller rebases.
    std::span<const ClothVertex> masses;
    std::span<const uint32_t> indices;
    /// Where the node hierarchy placed it, baked into the vertices by `ClothSystem::add`.
    glm::mat4 transform{1.0f};
};

/**
 * @brief A simulation mesh, welded, with the map back to the render mesh.
 *
 * A glTF vertex is a *shading* vertex: the exporter duplicates one at every UV seam,
 * smoothing split and material boundary. Feeding those straight to a soft body puts two
 * unconstrained particles at one point, and the fabric tears itself open along every seam on
 * the first step -- silently, and looking exactly like a solver bug.
 *
 * Welded by quantised position only. Bringing normals or UVs into the key un-welds the two
 * sides of a sheet, which must move together. `cloth.gltf` has no duplicate vertices, so it
 * cannot catch a regression here; `tests/ClothTests.cpp` tests the weld directly.
 */
struct ClothTopology {
    /// One entry per welded particle: where it starts, in world space.
    std::vector<glm::vec3> positions;
    /// One entry per welded particle: the *minimum* inverse mass of the shading vertices that
    /// merged into it, so a seam with one pinned side stays pinned. Averaging instead invents
    /// a half-pinned vertex the author did not write.
    std::vector<float> invMasses;
    /// Triangles, as welded indices, with degenerates already dropped -- Jolt asserts on one.
    std::vector<uint32_t> faces;
    /// Render vertex -> welded particle: `positions[remap[i]]` is where render vertex `i`
    /// went.
    std::vector<uint32_t> remap;

    [[nodiscard]] bool empty() const { return positions.empty() || faces.empty(); }
};

/**
 * @brief Weld a primitive into a simulation mesh.
 *
 * Transforms to world space *before* welding: the quantisation grid is world-space, so
 * welding first gives a scaled mesh a different answer than the same mesh unscaled.
 *
 * A `masses` span shorter than `vertices` leaves the remainder free. A cloth left pinned
 * nowhere is refused by `ClothSystem::add`, not here.
 */
[[nodiscard]] ClothTopology weldCloth(const ClothDesc& desc);

/**
 * @brief Recompute normals and re-orthogonalise tangents over a deformed mesh, in place.
 *
 * Jolt returns positions and nothing else, so the shading basis is rebuilt here.
 *
 * `restTangents` is what the tangents are orthogonalised *from*, and the span must be the
 * rest pose. Passing the vertices' own current tangents makes Gram-Schmidt a fold over its
 * own output: a frame that ran one step and a frame that ran four reach the same positions
 * and different tangents, so a cloth's shading becomes a function of the frame rate. An empty
 * span falls back to the vertices' tangents and is only correct for a one-shot call.
 */
void recomputeClothNormals(std::span<Vertex> vertices, std::span<const uint32_t> indices,
                           std::span<const glm::vec4> restTangents = {});

} // namespace scene
