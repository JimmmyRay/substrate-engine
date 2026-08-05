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
 * The solver itself is Jolt's, inside `PhysicsWorld`; everything here is arithmetic. Naming
 * a Jolt or Vulkan type in this translation unit drops it out of `SUBSTRATE_HOSTED_SOURCES`
 * and out of reach of the sanitizers.
 */
namespace scene {

class PhysicsWorld;

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

/**
 * @brief Every cloth in the scene, and the vertices the renderer copies to the GPU.
 *
 * Dense indices, no generation counters, unlike `PhysicsWorld` and `InstanceTable`: a cloth
 * is created at load and destroyed with the scene, so an index cannot go stale under a
 * caller. Adding removal to this class breaks that and every held index with it.
 */
class ClothSystem {
  public:
    /// What one cloth needs the renderer to know.
    struct Cloth {
        /// The instance this cloth deforms. `Renderer::skinDestBase[slot]` is where its
        /// vertices land in the deformed vertex buffer.
        uint32_t instance = 0xFFFFFFFFu;
        /// The primitive it was built from.
        uint32_t primitive = 0xFFFFFFFFu;
        /// The soft body in the physics world, as a raw slot rather than a `ClothId`: typing
        /// it would put `Physics.h`, and so Jolt, on this header's include path.
        uint32_t body = 0xFFFFFFFFu;
        /// The render mesh, updated in place every frame; rest pose until the first `update`.
        std::vector<Vertex> vertices;
        /// Render-mesh indices, zero-based, kept for the normal recompute.
        std::vector<uint32_t> indices;
        /// The rest pose's tangents, in world space. Every frame's Gram-Schmidt must start
        /// from these -- see `recomputeClothNormals`.
        std::vector<glm::vec4> restTangents;
        /// Welded particle per render vertex.
        std::vector<uint32_t> remap;
        /// World-space bounds of the last solved pose. Not used for culling: cloth inherits
        /// the infinite culling box every deformed instance has.
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
        /// Largest distance any vertex moved in the last `update`, in metres.
        float lastMaxDisplacement = 0.0f;
    };

    /**
     * @brief Create a soft body for one primitive and start tracking it.
     *
     * @return false, having created nothing, when the primitive welds to no faces or has no
     *         vertex pinned. Simulating the second instead would drop the cloth out of the
     *         world on frame one, which reads as an engine bug rather than an authoring one.
     */
    bool add(PhysicsWorld& world, uint32_t instance, uint32_t primitive, const ClothDesc& desc);

    /**
     * @brief Read the solved pose back and reshade it. Once per frame, after the steps.
     *
     * Calling it per fixed step instead pays for up to three poses nothing draws; the normal
     * recompute is the expensive half.
     */
    void update(const PhysicsWorld& world);

    /// Drop the bookkeeping. The soft bodies belong to the world and are freed with it.
    void clear() { clothes.clear(); }

    [[nodiscard]] bool empty() const { return clothes.empty(); }
    [[nodiscard]] uint32_t count() const { return static_cast<uint32_t>(clothes.size()); }
    [[nodiscard]] const Cloth& at(uint32_t i) const { return clothes[i]; }
    /// Total vertices across every cloth. What the renderer sizes its staging buffer from.
    [[nodiscard]] uint32_t vertexCount() const;

  private:
    std::vector<Cloth> clothes;
    /// Solved particle positions. A member, not a local: the same shape as a local measured
    /// 179 ns and 2.4% of the step in allocation alone. Grown to the largest cloth, never
    /// shrunk, so only the first `particles` entries are meaningful.
    std::vector<glm::vec3> scratch;
};

} // namespace scene
