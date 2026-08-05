#pragma once

#include "scene/Cloth.h"
#include "scene/SceneTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

/**
 * @file engine/physics/ClothSystem.h
 * @brief The bookkeeping over the soft bodies a `PhysicsWorld` solves.
 *
 * The authoring convention and the mesh maths are `scene/Cloth.h`, which the loader runs
 * before a world exists. What is here holds indices into a world, so it has to be destroyed
 * before that world -- which is why the pair is one module and one translation unit. See
 * `physics/PhysicsModule.cpp`.
 */
namespace physics {

class PhysicsWorld;

/**
 * @brief Every cloth in the scene, and the vertices the renderer copies to the GPU.
 *
 * Dense indices, no generation counters, unlike `PhysicsWorld` and `scene::InstanceTable`: a
 * cloth is created at load and destroyed with the scene, so an index cannot go stale under a
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
        /// The soft body in the physics world, as a raw slot rather than a `ClothId`: nothing
        /// destroys one cloth, so there is no stale index to detect.
        uint32_t body = 0xFFFFFFFFu;
        /// The render mesh, updated in place every frame; rest pose until the first `update`.
        std::vector<scene::Vertex> vertices;
        /// Render-mesh indices, zero-based, kept for the normal recompute.
        std::vector<uint32_t> indices;
        /// The rest pose's tangents, in world space. Every frame's Gram-Schmidt must start
        /// from these -- see `scene::recomputeClothNormals`.
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
    bool add(PhysicsWorld& world, uint32_t instance, uint32_t primitive, const scene::ClothDesc& desc);

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

} // namespace physics
