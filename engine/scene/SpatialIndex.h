#pragma once

#include "gfx/Light.h"
#include "scene/InstanceTable.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

/**
 * @file engine/scene/SpatialIndex.h
 * @brief A bounding-volume hierarchy over the instance table (C9).
 *
 * ## What it is for
 *
 * Three questions the engine could only answer by walking every instance: *what does this
 * ray pass through*, *what is inside this box*, and *what is inside this frustum*. Each is
 * O(n) today and each has a caller that wants it every frame or on every click --
 * click-to-select, an audio occlusion probe, a trigger volume, a query the CPU asks before
 * the GPU cull runs.
 *
 * ## What it indexes, and why that is not what the row said
 *
 * The row this comes from indexes **node bounds** and is gated on a scene tree that does
 * not exist here. This indexes **instance bounds**, which needs no tree: `InstanceTable`
 * already keeps a world-space box per slot and already refreshes it in `setTransform`,
 * which is the only thing that can invalidate one. Every consumer the row named --
 * picking, a broadphase, occlusion probes -- asks about *things in the world*, and in this
 * engine a thing in the world is an instance.
 *
 * The gate was real for the other half of the row, and it is the half not built here:
 * *incremental update on reparenting*. A tree is what makes a structural change local; a
 * flat table has no reparenting, so what a moved instance needs is a **refit**, and that is
 * what `refit` does.
 *
 * ## What it answers, and what it does not
 *
 * Boxes, not triangles. `raycast` returns the instances a ray *could* hit, nearest box
 * entry first -- a broadphase, which is what a BVH over AABBs can honestly be. The narrow
 * phase is `PhysicsWorld::raycast` (C2) where a scene has colliders, or the caller's own
 * triangle test where it does not. Conflating the two is how a picking system ends up
 * selecting the object whose *box* was in front.
 *
 * ## Build policy
 *
 * Top-down, splitting at the spatial median of the longest axis. Not SAH, and the reason is
 * the use: this index is rebuilt whenever the table's topology changes and refitted
 * whenever anything moves, so build time is paid far more often than a static-scene index
 * would pay it. A median split builds in one pass over the centroids and gives a tree whose
 * query cost is within a small factor of SAH's on the scenes this engine loads. **If a
 * profile ever shows queries dominating builds, that is the measurement that justifies
 * SAH** -- and not before.
 */
namespace scene {

/**
 * @brief One node. Leaves carry a range of instance slots; internal nodes carry a right
 *        child index, the left child being the next node in the array.
 *
 * Depth-first order, so the left child is always adjacent and only one index is stored.
 * That is what keeps a node at 32 bytes and the traversal a stack of `uint32_t`.
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
    /**
     * @brief Rebuild from every live instance in `table`.
     *
     * Instances with no live flag are skipped, so a table with holes in it produces a tree
     * over what is actually there. Records `table.revision()`, which is what `stale()`
     * compares against.
     */
    void build(const InstanceTable& table);

    /**
     * @brief Recompute every node's box from the table, leaving the topology alone.
     *
     * What a moved instance needs, and it is O(n) over the *nodes* rather than a rebuild:
     * no sort, no allocation, one bottom-up pass. A tree refitted after large movement
     * gets progressively worse-shaped without ever becoming wrong, which is the trade --
     * `build` is what fixes the shape, and a caller that moves everything every frame
     * should call it instead.
     *
     * Undefined if the table's topology changed; `stale()` is how a caller finds out.
     */
    void refit(const InstanceTable& table);

    /// True when the table has changed shape since `build`. A moved instance does not make
    /// the index stale -- it makes it want a `refit`, which is a different and much cheaper
    /// answer, and conflating the two is how an index gets rebuilt sixty times a second.
    [[nodiscard]] bool stale(const InstanceTable& table) const { return table.slotCount() != builtSlots; }

    /// A ray's nearest *possible* hit: the instance whose box the ray enters first.
    /// `distance` is that entry distance, not a surface distance. Negative means a miss,
    /// the same convention `PhysicsWorld::RayHit` uses and for the same reason.
    struct RayHit {
        uint32_t instance = kNoInstance;
        float distance = -1.0f;
        [[nodiscard]] explicit operator bool() const { return distance >= 0.0f; }
    };

    /// What a query returns when nothing was in range. A slot index a caller can hold, so
    /// it is a named value rather than "check the distance and hope".
    static constexpr uint32_t kNoInstance = 0xFFFFFFFFu;

    /**
     * @brief Nearest instance box the ray enters, within `maxDistance`.
     *
     * `direction` need not be normalised; distances come back in units of it. A ray
     * starting inside a box hits it at distance zero, which is what a click inside a
     * building should select.
     */
    [[nodiscard]] RayHit raycast(const glm::vec3& origin, const glm::vec3& direction,
                                 float maxDistance = 3.4e38f) const;

    /// Every instance whose box overlaps the given one. Appends; the caller owns `out` and
    /// can reuse it across frames, which is the whole reason this is not a return value.
    void overlap(const glm::vec3& queryMin, const glm::vec3& queryMax, std::vector<uint32_t>& out) const;

    /// Every instance whose box is inside or crossing the frustum. Shares `gfx::Frustum`
    /// with C8's light culling rather than declaring a second plane set -- one definition
    /// of "inside", tested once.
    void visible(const gfx::Frustum& frustum, std::vector<uint32_t>& out) const;

    [[nodiscard]] const std::vector<SpatialNode>& nodes() const { return tree; }
    /// The boxes, in leaf order and parallel to `items()`. Copied out of the table at
    /// build and refresh, so a query needs no table at all -- which is what lets one be
    /// answered from a worker while the main thread is moving things.
    [[nodiscard]] const std::vector<GpuInstanceBounds>& itemBounds() const { return boxes; }
    /// Instance slots in leaf order. A leaf's range indexes this, not the table.
    [[nodiscard]] const std::vector<uint32_t>& items() const { return order; }
    [[nodiscard]] bool empty() const { return tree.empty(); }

    /// Longest root-to-leaf path, for the log line and for a test that a degenerate scene
    /// -- ten thousand instances at one point -- does not build a ten-thousand-deep tree.
    [[nodiscard]] uint32_t depth() const;

    /// Instances per leaf, past which a node is not split. Four rather than one: a leaf
    /// test is a box test either way, and four boxes tested linearly beat three more
    /// levels of traversal on every scene measured.
    static constexpr uint32_t kLeafSize = 4;

  private:
    uint32_t buildRange(uint32_t first, uint32_t count);
    void refitNode(uint32_t node);

    std::vector<SpatialNode> tree;
    std::vector<uint32_t> order;
    std::vector<GpuInstanceBounds> boxes;
    /// Centroids, kept only for the duration of a build. A member rather than a local so a
    /// rebuild of the same scene reuses the allocation.
    std::vector<glm::vec3> centroids;
    uint32_t builtSlots = 0;
};

} // namespace scene
