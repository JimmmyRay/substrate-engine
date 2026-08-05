#pragma once

#include "gfx/Light.h"
#include "scene/InstanceTable.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

/**
 * @file engine/scene/SpatialIndex.h
 * @brief A bounding-volume hierarchy over the instance table's world-space boxes.
 *
 * Boxes, not triangles: every query here is a broadphase. Treating a `raycast` result as a
 * hit is how a picking system selects the object whose *box* was in front -- the narrow phase
 * is `PhysicsWorld::raycast`, or the caller's own triangle test where a scene has no
 * colliders.
 *
 * Split at the spatial median of the longest axis, not by SAH: this index is rebuilt on every
 * topology change and refitted on every move, so build cost is paid far more often than query
 * cost. Only a profile showing queries dominating builds justifies changing that.
 */
namespace scene {

/**
 * @brief One node. Leaves carry a range of instance slots; internal nodes carry a right
 *        child index, the left child being the next node in the array.
 *
 * Nodes are emitted depth-first, and the left child being adjacent is what lets one index
 * stand for two. Emitting them in any other order silently breaks every traversal.
 */
struct SpatialNode {
    glm::vec3 boundsMin{0.0f};
    /// Leaves: the first entry in `items()`. Internal: the right child's node index.
    uint32_t firstItem = 0;
    glm::vec3 boundsMax{0.0f};
    /// Zero means internal. A leaf always holds at least one instance.
    uint32_t itemCount = 0;
};

static_assert(sizeof(SpatialNode) == 32, "SpatialNode should stay cache-friendly");

class SpatialIndex {
  public:
    /// Rebuild from every live instance in `table`, skipping dead slots.
    void build(const InstanceTable& table);

    /**
     * @brief Recompute every node's box from the table, leaving the topology alone.
     *
     * The answer for a moved instance: one bottom-up pass, no sort, no allocation. A tree
     * refitted through large movement gets worse-shaped without becoming wrong; `build` is
     * what fixes the shape.
     *
     * Undefined if the table's topology changed -- `stale()` is how a caller finds out.
     */
    void refit(const InstanceTable& table);

    /// True when the table has changed shape since `build`. A moved instance does not make
    /// the index stale, it makes it want a `refit`; conflating the two rebuilds sixty times
    /// a second.
    [[nodiscard]] bool stale(const InstanceTable& table) const { return table.slotCount() != builtSlots; }

    /// A ray's nearest *possible* hit: the instance whose box the ray enters first.
    /// `distance` is that box-entry distance, not a surface distance. Negative means a miss,
    /// matching `PhysicsWorld::RayHit`.
    struct RayHit {
        uint32_t instance = kNoInstance;
        float distance = -1.0f;
        [[nodiscard]] explicit operator bool() const { return distance >= 0.0f; }
    };

    /// What a query returns when nothing was in range.
    static constexpr uint32_t kNoInstance = 0xFFFFFFFFu;

    /**
     * @brief Nearest instance box the ray enters, within `maxDistance`.
     *
     * `direction` need not be normalised, and distances come back in units of it -- an
     * unnormalised one silently rescales `maxDistance` too. A ray starting inside a box hits
     * it at zero, which is what makes a click inside a building select the building.
     */
    [[nodiscard]] RayHit raycast(const glm::vec3& origin, const glm::vec3& direction,
                                 float maxDistance = 3.4e38f) const;

    /// Every instance whose box overlaps the given one. Appends to `out` rather than
    /// clearing it, so a caller can reuse one buffer across frames.
    void overlap(const glm::vec3& queryMin, const glm::vec3& queryMax, std::vector<uint32_t>& out) const;

    /// Every instance whose box is inside or crossing the frustum. Appends, as `overlap`
    /// does. Shares `gfx::Frustum` with light culling: one definition of "inside".
    void visible(const gfx::Frustum& frustum, std::vector<uint32_t>& out) const;

    [[nodiscard]] const std::vector<SpatialNode>& nodes() const { return tree; }
    /// The boxes, in leaf order and parallel to `items()`. Copied out of the table at build
    /// and refit, so a query touches no table and can be answered while one is being moved.
    [[nodiscard]] const std::vector<GpuInstanceBounds>& itemBounds() const { return boxes; }
    /// Instance slots in leaf order. A leaf's range indexes this, not the table.
    [[nodiscard]] const std::vector<uint32_t>& items() const { return order; }
    [[nodiscard]] bool empty() const { return tree.empty(); }

    /// Longest root-to-leaf path.
    [[nodiscard]] uint32_t depth() const;

    /// Instances per leaf, past which a node is not split. Four rather than one: a leaf test
    /// is a box test either way, and four tested linearly beat three more traversal levels on
    /// every scene measured.
    static constexpr uint32_t kLeafSize = 4;

  private:
    uint32_t buildRange(uint32_t first, uint32_t count);
    void refitNode(uint32_t node);

    std::vector<SpatialNode> tree;
    std::vector<uint32_t> order;
    std::vector<GpuInstanceBounds> boxes;
    /// Centroids, meaningful only during a build; a member so a rebuild reuses the allocation.
    std::vector<glm::vec3> centroids;
    uint32_t builtSlots = 0;
};

} // namespace scene
