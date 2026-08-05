#include "scene/SpatialIndex.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <random>
#include <vector>

using namespace core;
using namespace scene;

/**
 * @file tests/SpatialIndexTests.cpp
 * @brief The BVH: that it answers what a linear scan would, and faster.
 *
 * An acceleration structure has exactly one correctness property and it is easy to state:
 * **it must return what the O(n) answer would**. Everything else -- depth, node count,
 * build time -- is performance, and performance that returns the wrong set is worthless.
 *
 * So the load-bearing tests here compare against a brute-force scan over the same table,
 * on random scenes, for all three query kinds. A tree that prunes one box too many passes
 * every hand-written "does it find the cube" test and fails these.
 */
namespace {

/// A table of axis-aligned unit boxes at given centres. `InstanceDesc` takes object-space
/// bounds and a transform, which is what the table turns into the world box the index
/// reads -- so building the fixture this way exercises the same path a scene does.
InstanceTable tableOf(const std::vector<glm::vec3>& centres, float halfSize = 0.5f) {
    InstanceTable t;
    for (const glm::vec3& c : centres) {
        InstanceDesc d;
        d.transform = glm::translate(glm::mat4(1.0f), c);
        d.localMin = glm::vec3(-halfSize);
        d.localMax = glm::vec3(halfSize);
        (void)t.create(d);
    }
    return t;
}

std::vector<glm::vec3> randomCentres(size_t count, uint32_t seed, float spread = 40.0f) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-spread, spread);
    std::vector<glm::vec3> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) out.push_back({dist(rng), dist(rng), dist(rng)});
    return out;
}

/// The answer the index has to match, computed the slow way.
std::vector<uint32_t> bruteOverlap(const InstanceTable& t, const glm::vec3& lo, const glm::vec3& hi) {
    std::vector<uint32_t> out;
    for (uint32_t slot = 0; slot < t.slotCount(); ++slot) {
        if ((t.slot(slot).meta.z & kInstanceLive) == 0) continue;
        const GpuInstanceBounds& b = t.slotBounds(slot);
        const glm::vec3 bMin(b.worldMin);
        const glm::vec3 bMax(b.worldMax);
        if (bMin.x <= hi.x && bMax.x >= lo.x && bMin.y <= hi.y && bMax.y >= lo.y && bMin.z <= hi.z && bMax.z >= lo.z) {
            out.push_back(slot);
        }
    }
    return out;
}

/// Nearest box entry along a ray, the slow way. Same slab test as the index's, written out
/// separately on purpose: a shared helper would agree with the index by construction.
std::pair<uint32_t, float> bruteRay(const InstanceTable& t, const glm::vec3& o, const glm::vec3& d, float maxT) {
    std::pair<uint32_t, float> best{SpatialIndex::kNoInstance, -1.0f};
    float bestT = maxT;
    for (uint32_t slot = 0; slot < t.slotCount(); ++slot) {
        if ((t.slot(slot).meta.z & kInstanceLive) == 0) continue;
        const GpuInstanceBounds& b = t.slotBounds(slot);
        float lo = -1e30f;
        float hi = 1e30f;
        bool miss = false;
        for (int a = 0; a < 3; ++a) {
            if (std::abs(d[a]) < 1e-20f) {
                if (o[a] < b.worldMin[a] || o[a] > b.worldMax[a]) miss = true;
                continue;
            }
            float t0 = (b.worldMin[a] - o[a]) / d[a];
            float t1 = (b.worldMax[a] - o[a]) / d[a];
            if (t0 > t1) std::swap(t0, t1);
            lo = std::max(lo, t0);
            hi = std::min(hi, t1);
        }
        if (miss || hi < 0.0f || lo > hi) continue;
        const float entry = std::max(lo, 0.0f);
        if (entry > bestT) continue;
        bestT = entry;
        best = {slot, entry};
    }
    return best;
}

std::vector<uint32_t> sorted(std::vector<uint32_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace

TEST(SpatialIndex, AnEmptyTableBuildsAnEmptyTreeAndAnswersNothing) {
    // Every query has to survive it, because "no scene loaded yet" is a real frame.
    InstanceTable t;
    SpatialIndex index;
    index.build(t);
    EXPECT_TRUE(index.empty());
    EXPECT_EQ(index.depth(), 0u);

    std::vector<uint32_t> hits;
    index.overlap({-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, hits);
    EXPECT_TRUE(hits.empty());
    EXPECT_FALSE(index.raycast({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}));
}

TEST(SpatialIndex, TheRootBoxContainsEveryInstance) {
    const InstanceTable t = tableOf(randomCentres(200, 1));
    SpatialIndex index;
    index.build(t);
    ASSERT_FALSE(index.empty());

    const SpatialNode& root = index.nodes()[0];
    for (uint32_t slot = 0; slot < t.slotCount(); ++slot) {
        const GpuInstanceBounds& b = t.slotBounds(slot);
        EXPECT_LE(root.boundsMin.x, b.worldMin.x);
        EXPECT_LE(root.boundsMin.y, b.worldMin.y);
        EXPECT_LE(root.boundsMin.z, b.worldMin.z);
        EXPECT_GE(root.boundsMax.x, b.worldMax.x);
        EXPECT_GE(root.boundsMax.y, b.worldMax.y);
        EXPECT_GE(root.boundsMax.z, b.worldMax.z);
    }
}

TEST(SpatialIndex, EveryInstanceAppearsInExactlyOneLeaf) {
    // A partition, not a covering. An instance in two leaves is returned twice by every
    // query; an instance in none is invisible, which is the worse of the two.
    const InstanceTable t = tableOf(randomCentres(333, 7));
    SpatialIndex index;
    index.build(t);

    std::vector<uint32_t> seen;
    uint32_t leaves = 0;
    for (const SpatialNode& n : index.nodes()) {
        if (n.itemCount == 0) continue;
        ++leaves;
        EXPECT_LE(n.itemCount, SpatialIndex::kLeafSize);
        for (uint32_t i = 0; i < n.itemCount; ++i) seen.push_back(index.items()[n.firstItem + i]);
    }
    EXPECT_GT(leaves, 0u);
    ASSERT_EQ(seen.size(), 333u);
    std::sort(seen.begin(), seen.end());
    EXPECT_TRUE(std::adjacent_find(seen.begin(), seen.end()) == seen.end()) << "an instance is in two leaves";
    EXPECT_EQ(seen.front(), 0u);
    EXPECT_EQ(seen.back(), 332u);
}

TEST(SpatialIndex, OverlapMatchesTheLinearScanOnRandomScenes) {
    const std::vector<glm::vec3> centres = randomCentres(500, 11);
    const InstanceTable t = tableOf(centres);
    SpatialIndex index;
    index.build(t);

    std::mt19937 rng(99);
    std::uniform_real_distribution<float> pos(-45.0f, 45.0f);
    std::uniform_real_distribution<float> size(0.1f, 12.0f);
    for (int q = 0; q < 60; ++q) {
        const glm::vec3 centre{pos(rng), pos(rng), pos(rng)};
        const glm::vec3 half(size(rng));
        std::vector<uint32_t> hits;
        index.overlap(centre - half, centre + half, hits);
        EXPECT_EQ(sorted(hits), sorted(bruteOverlap(t, centre - half, centre + half))) << "query " << q;
    }
}

TEST(SpatialIndex, RaycastFindsTheSameNearestBoxAsTheLinearScan) {
    const InstanceTable t = tableOf(randomCentres(400, 23));
    SpatialIndex index;
    index.build(t);

    std::mt19937 rng(5);
    std::uniform_real_distribution<float> pos(-60.0f, 60.0f);
    std::uniform_real_distribution<float> dir(-1.0f, 1.0f);
    for (int q = 0; q < 100; ++q) {
        const glm::vec3 origin{pos(rng), pos(rng), pos(rng)};
        glm::vec3 direction{dir(rng), dir(rng), dir(rng)};
        if (glm::length(direction) < 1e-3f) direction = {0.0f, 0.0f, 1.0f};
        direction = glm::normalize(direction);

        const SpatialIndex::RayHit hit = index.raycast(origin, direction);
        const auto brute = bruteRay(t, origin, direction, 3.4e38f);

        EXPECT_EQ(static_cast<bool>(hit), brute.first != SpatialIndex::kNoInstance) << "query " << q;
        if (!hit) continue;
        // The *distance* is compared rather than the slot: two boxes can be entered at the
        // same t, and which one a scan finds first is an ordering detail neither
        // implementation promises.
        EXPECT_NEAR(hit.distance, brute.second, 1e-4f) << "query " << q;
    }
}

TEST(SpatialIndex, MaxDistanceExcludesWhatIsBeyondIt) {
    const InstanceTable t = tableOf({{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 50.0f}});
    SpatialIndex index;
    index.build(t);

    const SpatialIndex::RayHit near = index.raycast({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 10.0f);
    ASSERT_TRUE(near);
    EXPECT_EQ(near.instance, 0u);

    // Past the first box but short of the second: a miss, not the far one.
    EXPECT_FALSE(index.raycast({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 1.0f}, 5.0f));
}

TEST(SpatialIndex, ARayStartingInsideABoxHitsItAtZero) {
    // What a click inside a building has to do. A slab test that returned the negative
    // entry distance would report the box behind the camera instead.
    const InstanceTable t = tableOf({{0.0f, 0.0f, 0.0f}}, 2.0f);
    SpatialIndex index;
    index.build(t);

    const SpatialIndex::RayHit hit = index.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit.instance, 0u);
    EXPECT_FLOAT_EQ(hit.distance, 0.0f);
}

TEST(SpatialIndex, AnAxisAlignedRayAlongASlabDoesNotMissTheBoxItPasses) {
    // The division-by-zero case the slab test's comment argues about. A ray travelling
    // exactly along x through a box has an infinite t range on y and z, and a naive test
    // turns that into NaN and a miss.
    const InstanceTable t = tableOf({{10.0f, 0.0f, 0.0f}});
    SpatialIndex index;
    index.build(t);

    const SpatialIndex::RayHit hit = index.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    ASSERT_TRUE(hit);
    EXPECT_NEAR(hit.distance, 9.5f, 1e-4f);

    // And one that passes beside it misses.
    EXPECT_FALSE(index.raycast({0.0f, 3.0f, 0.0f}, {1.0f, 0.0f, 0.0f}));
}

TEST(SpatialIndex, FrustumVisibilityMatchesTestingEveryBox) {
    const InstanceTable t = tableOf(randomCentres(300, 31));
    SpatialIndex index;
    index.build(t);

    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 5.0f, 60.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(50.0f), 16.0f / 9.0f, 0.1f, 200.0f);
    const gfx::Frustum frustum = gfx::extractFrustum(proj * view);

    std::vector<uint32_t> hits;
    index.visible(frustum, hits);

    // The index must not drop anything the per-box test keeps. It may keep more -- the
    // node test is conservative -- but a culler that drops a visible object is a hole in
    // the world, so that direction is asserted exactly.
    std::vector<uint32_t> expected;
    for (uint32_t slot = 0; slot < t.slotCount(); ++slot) {
        const GpuInstanceBounds& b = t.slotBounds(slot);
        bool inside = true;
        for (const glm::vec4& plane : frustum.planes) {
            const glm::vec3 positive(plane.x >= 0.0f ? b.worldMax.x : b.worldMin.x,
                                     plane.y >= 0.0f ? b.worldMax.y : b.worldMin.y,
                                     plane.z >= 0.0f ? b.worldMax.z : b.worldMin.z);
            if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f) {
                inside = false;
                break;
            }
        }
        if (inside) expected.push_back(slot);
    }
    EXPECT_EQ(sorted(hits), sorted(expected));
    EXPECT_LT(hits.size(), t.slotCount()) << "the frustum culled nothing, so this proves nothing";
}

TEST(SpatialIndex, RefitFollowsMovedInstancesWithoutRebuilding) {
    std::vector<glm::vec3> centres = randomCentres(120, 41);
    InstanceTable t = tableOf(centres);
    SpatialIndex index;
    index.build(t);

    // Move everything a long way. The tree keeps its shape -- which is the trade `refit`
    // documents -- but every answer still has to be right.
    for (uint32_t slot = 0; slot < t.slotCount(); ++slot) {
        t.setTransform(t.idAt(slot), glm::translate(glm::mat4(1.0f), centres[slot] + glm::vec3(100.0f, 0.0f, 0.0f)));
    }
    EXPECT_FALSE(index.stale(t)) << "moving an instance is not a structural change";

    const size_t before = index.nodes().size();
    index.refit(t);
    EXPECT_EQ(index.nodes().size(), before) << "refit must not change the topology";

    const glm::vec3 lo{95.0f, -50.0f, -50.0f};
    const glm::vec3 hi{150.0f, 50.0f, 50.0f};
    std::vector<uint32_t> hits;
    index.overlap(lo, hi, hits);
    EXPECT_EQ(sorted(hits), sorted(bruteOverlap(t, lo, hi)));

    // And the old region is empty, which is what a stale tree would get wrong.
    hits.clear();
    index.overlap({-50.0f, -50.0f, -50.0f}, {50.0f, 50.0f, 50.0f}, hits);
    EXPECT_TRUE(hits.empty());
}

TEST(SpatialIndex, CreatingAnInstanceMakesTheIndexStale) {
    InstanceTable t = tableOf(randomCentres(10, 3));
    SpatialIndex index;
    index.build(t);
    EXPECT_FALSE(index.stale(t));

    InstanceDesc d;
    d.localMin = glm::vec3(-1.0f);
    d.localMax = glm::vec3(1.0f);
    (void)t.create(d);
    EXPECT_TRUE(index.stale(t)) << "a new slot is a structural change and needs a rebuild";
}

TEST(SpatialIndex, DestroyedInstancesAreNotIndexed) {
    InstanceTable t = tableOf({{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}});
    t.destroy(t.idAt(1));

    SpatialIndex index;
    index.build(t);
    EXPECT_EQ(index.items().size(), 2u);

    std::vector<uint32_t> hits;
    index.overlap({9.0f, -1.0f, -1.0f}, {11.0f, 1.0f, 1.0f}, hits);
    EXPECT_TRUE(hits.empty()) << "a destroyed instance is still in the table and must not be in the tree";
}

TEST(SpatialIndex, TenThousandInstancesAtOnePointDoNotBuildATenThousandDeepTree) {
    // The degenerate case a spatial median cannot split: every centroid identical. Without
    // the zero-extent leaf test this recurses until the stack goes.
    const InstanceTable t = tableOf(std::vector<glm::vec3>(10000, glm::vec3(0.0f)));
    SpatialIndex index;
    index.build(t);

    EXPECT_EQ(index.items().size(), 10000u);
    EXPECT_EQ(index.depth(), 1u) << "one leaf, because there is nothing to split on";

    std::vector<uint32_t> hits;
    index.overlap({-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, hits);
    EXPECT_EQ(hits.size(), 10000u);
}

TEST(SpatialIndex, ALargeSceneStaysShallowEnoughForTheTraversalStack) {
    // The traversal stack is a fixed 64 entries. A well-spread scene must not approach it,
    // and this is the assertion that would fail before a query started silently truncating.
    const InstanceTable t = tableOf(randomCentres(20000, 77, 500.0f));
    SpatialIndex index;
    index.build(t);
    EXPECT_LT(index.depth(), 40u) << "depth " << index.depth();
}
