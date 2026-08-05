#include "scene/SpatialIndex.h"

#include <algorithm>

namespace scene {

namespace {

/// Slab test. Returns the entry distance, or a negative number for a miss.
///
/// Requires IEEE division semantics: a zero direction component gives +/-inf, which orders
/// correctly, and a ray exactly on a slab plane gives NaN, which fails every comparison and
/// so falls out of the `tmin > tmax` guard as a miss. Do not add per-component branches to
/// "fix" this -- they cost three tests per node and change no answer.
float rayBoxEntry(const glm::vec3& origin, const glm::vec3& invDirection, const glm::vec3& boxMin,
                  const glm::vec3& boxMax, float maxDistance) {
    const glm::vec3 t0 = (boxMin - origin) * invDirection;
    const glm::vec3 t1 = (boxMax - origin) * invDirection;
    const glm::vec3 lo = glm::min(t0, t1);
    const glm::vec3 hi = glm::max(t0, t1);

    const float tmin = std::max(std::max(lo.x, lo.y), lo.z);
    const float tmax = std::min(std::min(hi.x, hi.y), hi.z);

    if (tmax < 0.0f || tmin > tmax || tmin > maxDistance) return -1.0f;
    // Clamped so a ray starting inside the box enters at zero rather than behind itself,
    // which is what makes a click inside a building select the building.
    return std::max(tmin, 0.0f);
}

bool boxesOverlap(const glm::vec3& aMin, const glm::vec3& aMax, const glm::vec3& bMin, const glm::vec3& bMax) {
    return aMin.x <= bMax.x && aMax.x >= bMin.x && aMin.y <= bMax.y && aMax.y >= bMin.y && aMin.z <= bMax.z &&
           aMax.z >= bMin.z;
}

/// Conservative: a box straddling two planes' shared corner is kept even though no plane's
/// half-space alone excludes it. Erring the other way drops visible geometry.
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

    // Worst case is under 2n/kLeafSize nodes. Reserved up front so `buildRange` cannot
    // reallocate `tree` under a node it is still writing.
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

    // The zero-extent arm is not a corner case: stacked instances share a centroid, and
    // splitting them recurses until the stack overflows.
    if (count <= kLeafSize || extent[axis] <= 0.0f) {
        tree[self].firstItem = first;
        tree[self].itemCount = count;
        return self;
    }

    const float split = (centroidMin[axis] + centroidMax[axis]) * 0.5f;

    // `order`, `boxes` and `centroids` are three columns of one table: swapping any without
    // the others silently pairs a slot with a stranger's box.
    uint32_t mid = first;
    for (uint32_t i = first; i < first + count; ++i) {
        if (centroids[i][axis] >= split) continue;
        std::swap(order[i], order[mid]);
        std::swap(boxes[i], boxes[mid]);
        std::swap(centroids[i], centroids[mid]);
        ++mid;
    }

    // A spatial median can put every centroid on one side even with non-zero extent -- one
    // outlier against a tight cluster does it. An unbalanced split is a slower tree; an empty
    // one is infinite recursion, so fall back to halving by count.
    if (mid == first || mid == first + count) mid = first + count / 2;

    (void)buildRange(first, mid - first);
    tree[self].firstItem = buildRange(mid, first + count - mid);
    tree[self].itemCount = 0;
    return self;
}

void SpatialIndex::refit(const InstanceTable& table) {
    if (tree.empty()) return;
    // Every item box first, then the tree from them. Folding the two passes together would
    // let one leaf read refreshed boxes and its sibling stale ones.
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
    // Re-indexed rather than reusing `n`: the recursion writes through `tree`, so a reference
    // held across it becomes a use-after-invalidate the day this grows a push_back.
    tree[node].boundsMin = glm::min(tree[left].boundsMin, tree[right].boundsMin);
    tree[node].boundsMax = glm::max(tree[left].boundsMax, tree[right].boundsMax);
}

SpatialIndex::RayHit SpatialIndex::raycast(const glm::vec3& origin, const glm::vec3& direction,
                                           float maxDistance) const {
    RayHit hit;
    if (tree.empty()) return hit;

    const glm::vec3 invDirection = 1.0f / direction;

    // Explicit stack, not recursion: a deep tree is where a BVH query becomes a stack
    // overflow. 64 is past the depth a median split reaches for any instance count this
    // engine can hold, and the push below drops nodes rather than growing it.
    uint32_t stack[64];
    uint32_t top = 0;
    stack[top++] = 0;

    float best = maxDistance;
    while (top > 0) {
        const uint32_t index = stack[--top];
        const SpatialNode& n = tree[index];

        const float entry = rayBoxEntry(origin, invDirection, n.boundsMin, n.boundsMax, best);
        // Re-tested against `best` on pop, not only on push: a node queued before a closer
        // hit was found is where most of the pruning comes from.
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
