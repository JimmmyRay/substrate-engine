#include "scene/SpatialIndex.h"

#include <algorithm>

namespace scene {

namespace {

/// Slab test. Returns the entry distance, or a negative number for a miss.
///
/// The division by a zero component is deliberate and correct: IEEE gives +/-inf, the
/// comparisons below order correctly against it, and the only case that needs care is a
/// ray exactly on a slab plane, where 0 * inf is NaN and every comparison is false --
/// which reads as a miss on that axis and is what the `tmax >= tmin` guard turns into a
/// miss overall. Branching on each component instead costs three tests per node to avoid
/// an answer that is already right.
float rayBoxEntry(const glm::vec3& origin, const glm::vec3& invDirection, const glm::vec3& boxMin,
                  const glm::vec3& boxMax, float maxDistance) {
    const glm::vec3 t0 = (boxMin - origin) * invDirection;
    const glm::vec3 t1 = (boxMax - origin) * invDirection;
    const glm::vec3 lo = glm::min(t0, t1);
    const glm::vec3 hi = glm::max(t0, t1);

    const float tmin = std::max(std::max(lo.x, lo.y), lo.z);
    const float tmax = std::min(std::min(hi.x, hi.y), hi.z);

    if (tmax < 0.0f || tmin > tmax || tmin > maxDistance) return -1.0f;
    // A ray starting inside the box enters at zero rather than at a negative distance,
    // which is what makes a click inside a building select the building.
    return std::max(tmin, 0.0f);
}

bool boxesOverlap(const glm::vec3& aMin, const glm::vec3& aMax, const glm::vec3& bMin, const glm::vec3& bMax) {
    return aMin.x <= bMax.x && aMax.x >= bMin.x && aMin.y <= bMax.y && aMax.y >= bMin.y && aMin.z <= bMax.z &&
           aMax.z >= bMin.z;
}

/// The same test `gfx::lightVisible` makes, against a box instead of a sphere: a box is
/// outside when it is entirely behind any one plane. Conservative in the corner case two
/// planes share -- a box may be kept that no plane's half-space alone excludes -- and that
/// is the right direction to be wrong in for a culler.
bool boxInFrustum(const gfx::Frustum& frustum, const glm::vec3& boxMin, const glm::vec3& boxMax) {
    for (const glm::vec4& plane : frustum.planes) {
        // The corner furthest along the plane normal. If even that is behind, all eight are.
        const glm::vec3 positive(plane.x >= 0.0f ? boxMax.x : boxMin.x, plane.y >= 0.0f ? boxMax.y : boxMin.y,
                                 plane.z >= 0.0f ? boxMax.z : boxMin.z);
        if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f) return false;
    }
    return true;
}

} // namespace

void SpatialIndex::build(const InstanceTable& table) {
    tree.clear();
    order.clear();
    boxes.clear();
    centroids.clear();
    builtSlots = table.slotCount();

    order.reserve(table.liveCount());
    boxes.reserve(table.liveCount());
    centroids.reserve(table.liveCount());
    for (uint32_t slot = 0; slot < table.slotCount(); ++slot) {
        if ((table.slot(slot).meta.z & kInstanceLive) == 0) continue;
        const GpuInstanceBounds& b = table.slotBounds(slot);
        order.push_back(slot);
        boxes.push_back(b);
        centroids.push_back((glm::vec3(b.worldMin) + glm::vec3(b.worldMax)) * 0.5f);
    }

    if (order.empty()) return;

    // Worst case is one leaf per kLeafSize instances plus the internal nodes above them,
    // which is under 2n/kLeafSize. Reserved so the recursion never reallocates under the
    // references it holds.
    tree.reserve(2 * (order.size() / kLeafSize + 1));
    (void)buildRange(0, static_cast<uint32_t>(order.size()));
}

uint32_t SpatialIndex::buildRange(uint32_t first, uint32_t count) {
    const uint32_t self = static_cast<uint32_t>(tree.size());
    tree.push_back({});

    glm::vec3 boxMin(3.4e38f);
    glm::vec3 boxMax(-3.4e38f);
    glm::vec3 centroidMin(3.4e38f);
    glm::vec3 centroidMax(-3.4e38f);
    for (uint32_t i = 0; i < count; ++i) {
        const GpuInstanceBounds& b = boxes[first + i];
        boxMin = glm::min(boxMin, glm::vec3(b.worldMin));
        boxMax = glm::max(boxMax, glm::vec3(b.worldMax));
        centroidMin = glm::min(centroidMin, centroids[first + i]);
        centroidMax = glm::max(centroidMax, centroids[first + i]);
    }

    tree[self].boundsMin = boxMin;
    tree[self].boundsMax = boxMax;

    const glm::vec3 extent = centroidMax - centroidMin;
    const int axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);

    // A leaf, either because it is small enough or because every centroid is at the same
    // point. The second is not a corner case to tolerate but the shape a scene of stacked
    // instances actually takes, and splitting it would recurse until the stack overflowed.
    if (count <= kLeafSize || extent[axis] <= 0.0f) {
        tree[self].firstItem = first;
        tree[self].itemCount = count;
        return self;
    }

    const float split = (centroidMin[axis] + centroidMax[axis]) * 0.5f;

    // Partitioned in lockstep: `order` and `centroids` are two columns of one table, and a
    // sort that moved one without the other would silently pair a slot with a stranger's
    // centroid.
    uint32_t mid = first;
    for (uint32_t i = first; i < first + count; ++i) {
        if (centroids[i][axis] >= split) continue;
        std::swap(order[i], order[mid]);
        std::swap(boxes[i], boxes[mid]);
        std::swap(centroids[i], centroids[mid]);
        ++mid;
    }

    // Every centroid landed on one side. Possible with a spatial median even when the
    // extent is non-zero -- one outlier and a tight cluster does it -- so the halves are
    // taken by count instead. An unbalanced split is a slower tree; an empty one is
    // infinite recursion.
    if (mid == first || mid == first + count) mid = first + count / 2;

    (void)buildRange(first, mid - first);
    tree[self].firstItem = buildRange(mid, first + count - mid);
    tree[self].itemCount = 0;
    return self;
}

void SpatialIndex::refit(const InstanceTable& table) {
    if (tree.empty()) return;
    // The item boxes first, from the table, then the tree from them. Two passes rather
    // than one because a node's box is the union of its children's, and a leaf reading a
    // box its sibling had already refreshed would be reading two different frames.
    for (size_t i = 0; i < order.size(); ++i) boxes[i] = table.slotBounds(order[i]);
    refitNode(0);
}

void SpatialIndex::refitNode(uint32_t node) {
    SpatialNode& n = tree[node];
    if (n.itemCount != 0) {
        glm::vec3 boxMin(3.4e38f);
        glm::vec3 boxMax(-3.4e38f);
        for (uint32_t i = 0; i < n.itemCount; ++i) {
            const GpuInstanceBounds& b = boxes[n.firstItem + i];
            boxMin = glm::min(boxMin, glm::vec3(b.worldMin));
            boxMax = glm::max(boxMax, glm::vec3(b.worldMax));
        }
        n.boundsMin = boxMin;
        n.boundsMax = boxMax;
        return;
    }

    const uint32_t left = node + 1;
    const uint32_t right = n.firstItem;
    refitNode(left);
    refitNode(right);
    // Re-read: the recursion above may have reallocated nothing, but it did write through
    // `tree`, and holding a reference across it is the kind of thing that is correct today
    // and a use-after-invalidate the day this grows a push_back.
    tree[node].boundsMin = glm::min(tree[left].boundsMin, tree[right].boundsMin);
    tree[node].boundsMax = glm::max(tree[left].boundsMax, tree[right].boundsMax);
}

SpatialIndex::RayHit SpatialIndex::raycast(const glm::vec3& origin, const glm::vec3& direction,
                                           float maxDistance) const {
    RayHit hit;
    if (tree.empty()) return hit;

    const glm::vec3 invDirection = 1.0f / direction;

    // An explicit stack rather than recursion. A BVH traversal is the classic place a
    // deep tree turns a query into a stack overflow, and 64 is past the depth a median
    // split can reach for any instance count this engine can hold.
    uint32_t stack[64];
    uint32_t top = 0;
    stack[top++] = 0;

    float best = maxDistance;
    while (top > 0) {
        const uint32_t index = stack[--top];
        const SpatialNode& n = tree[index];

        const float entry = rayBoxEntry(origin, invDirection, n.boundsMin, n.boundsMax, best);
        // Re-tested against `best` rather than only on push: a node queued before a closer
        // hit was found is exactly what this prunes, and it is where most of the saving is.
        if (entry < 0.0f || entry > best) continue;

        if (n.itemCount != 0) {
            for (uint32_t i = 0; i < n.itemCount; ++i) {
                const uint32_t slot = order[n.firstItem + i];
                const GpuInstanceBounds& b = boxes[n.firstItem + i];
                const float t =
                    rayBoxEntry(origin, invDirection, glm::vec3(b.worldMin), glm::vec3(b.worldMax), best);
                if (t < 0.0f || t >= best) continue;
                best = t;
                hit.instance = slot;
                hit.distance = t;
            }
            continue;
        }

        if (top + 2 <= 64) {
            stack[top++] = index + 1;
            stack[top++] = n.firstItem;
        }
    }
    return hit;
}

void SpatialIndex::overlap(const glm::vec3& queryMin, const glm::vec3& queryMax, std::vector<uint32_t>& out) const {
    if (tree.empty()) return;

    uint32_t stack[64];
    uint32_t top = 0;
    stack[top++] = 0;

    while (top > 0) {
        const uint32_t index = stack[--top];
        const SpatialNode& n = tree[index];
        if (!boxesOverlap(n.boundsMin, n.boundsMax, queryMin, queryMax)) continue;

        if (n.itemCount != 0) {
            for (uint32_t i = 0; i < n.itemCount; ++i) {
                const uint32_t slot = order[n.firstItem + i];
                const GpuInstanceBounds& b = boxes[n.firstItem + i];
                if (boxesOverlap(glm::vec3(b.worldMin), glm::vec3(b.worldMax), queryMin, queryMax)) {
                    out.push_back(slot);
                }
            }
            continue;
        }

        if (top + 2 <= 64) {
            stack[top++] = index + 1;
            stack[top++] = n.firstItem;
        }
    }
}

void SpatialIndex::visible(const gfx::Frustum& frustum, std::vector<uint32_t>& out) const {
    if (tree.empty()) return;

    uint32_t stack[64];
    uint32_t top = 0;
    stack[top++] = 0;

    while (top > 0) {
        const uint32_t index = stack[--top];
        const SpatialNode& n = tree[index];
        if (!boxInFrustum(frustum, n.boundsMin, n.boundsMax)) continue;

        if (n.itemCount != 0) {
            for (uint32_t i = 0; i < n.itemCount; ++i) {
                const uint32_t slot = order[n.firstItem + i];
                const GpuInstanceBounds& b = boxes[n.firstItem + i];
                if (boxInFrustum(frustum, glm::vec3(b.worldMin), glm::vec3(b.worldMax))) out.push_back(slot);
            }
            continue;
        }

        if (top + 2 <= 64) {
            stack[top++] = index + 1;
            stack[top++] = n.firstItem;
        }
    }
}

uint32_t SpatialIndex::depth() const {
    if (tree.empty()) return 0;

    uint32_t deepest = 0;
    std::vector<std::pair<uint32_t, uint32_t>> stack{{0u, 1u}};
    while (!stack.empty()) {
        const auto entry = stack.back();
        stack.pop_back();
        const uint32_t index = entry.first;
        const uint32_t level = entry.second;
        deepest = std::max(deepest, level);
        if (tree[index].itemCount != 0) continue;
        stack.push_back({index + 1, level + 1});
        stack.push_back({tree[index].firstItem, level + 1});
    }
    return deepest;
}

} // namespace scene
