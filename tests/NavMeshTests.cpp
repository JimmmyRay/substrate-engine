#include "nav/NavMesh.h"

#include "scene/Physics.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <utility>
#include <vector>

using namespace nav;
using namespace scene;

/**
 * @file tests/NavMeshTests.cpp
 * @brief The walkable surface, the path through it, and the walk along it.
 *
 * Three stages and three kinds of property:
 *
 * 1. **The bake keeps what an agent could stand on and nothing else.** Slope, winding,
 *    welding, and the islands that are dropped for being unreachable scraps.
 * 2. **A path is the shortest one through the corridor A\* found.** This is the half that
 *    is easy to get subtly wrong and impossible to notice: a corridor of triangles is
 *    already a "working" path, and walking their centroids zig-zags in a way that looks
 *    like bad steering rather than a bad path. So the tests below assert the *shape* --
 *    that an open plane yields two points, and that an L hugs its inner corner exactly.
 * 3. **Steering ends.** A follower that never reports done is an agent that vibrates on
 *    the spot forever, which is the failure this subsystem is most likely to ship.
 */
namespace {

struct Soup {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
};

/// One quad in the XZ plane, wound so its normal points up -- which is what the bake
/// requires and what a glTF floor already does.
void addCell(Soup& s, float x, float z, float cell, float y = 0.0f) {
    const auto base = static_cast<uint32_t>(s.positions.size());
    s.positions.push_back({x, y, z});
    s.positions.push_back({x, y, z + cell});
    s.positions.push_back({x + cell, y, z + cell});
    s.positions.push_back({x + cell, y, z});
    for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) s.indices.push_back(base + i);
}

/// A solid box, every face wound so its normal points out of it. What the bake reads to
/// decide what is standing on a floor is the faces that are too steep to walk, so a box
/// authored as six quads is the smallest thing that can stand on anything.
void addBox(Soup& s, const glm::vec3& lo, const glm::vec3& hi) {
    const auto base = static_cast<uint32_t>(s.positions.size());
    s.positions.push_back({lo.x, lo.y, lo.z});
    s.positions.push_back({hi.x, lo.y, lo.z});
    s.positions.push_back({hi.x, lo.y, hi.z});
    s.positions.push_back({lo.x, lo.y, hi.z});
    s.positions.push_back({lo.x, hi.y, lo.z});
    s.positions.push_back({hi.x, hi.y, lo.z});
    s.positions.push_back({hi.x, hi.y, hi.z});
    s.positions.push_back({lo.x, hi.y, hi.z});
    for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u,   // down
                             4u, 6u, 5u, 4u, 7u, 6u,   // up
                             0u, 4u, 5u, 0u, 5u, 1u,   // -Z
                             3u, 2u, 6u, 3u, 6u, 7u,   // +Z
                             0u, 3u, 7u, 0u, 7u, 4u,   // -X
                             1u, 5u, 6u, 1u, 6u, 2u}) { // +X
        s.indices.push_back(base + i);
    }
}

/// A floor from a predicate over cell coordinates. Every cell is authored independently,
/// so its four corners are distinct vertices and the mesh only becomes connected if
/// welding works -- which is the same shape a merged glTF scene arrives in.
template <typename F> Soup floorOf(int w, int h, F filled, float cell = 1.0f) {
    Soup s;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            if (filled(i, j)) addCell(s, static_cast<float>(i) * cell, static_cast<float>(j) * cell, cell);
        }
    }
    return s;
}

NavBuildParams sharpParams() {
    // Radius zero and no island culling, for the tests that assert an exact path shape.
    // Both defaults are right for an agent and wrong for a ruler.
    NavBuildParams p;
    p.agentRadius = 0.0f;
    p.minRegionArea = 0.0f;
    return p;
}

/// Every triangle whose boundary the point lies on, as a start point each. A point on a tile
/// boundary is on two of them and a point on a grid corner on six; `nearest` returns whichever
/// the BVH reached first and all of them are equally correct, so a test that asked only
/// `nearest` would be red or green by traversal order rather than by the property under test.
std::vector<NavPoint> startsAt(const NavMesh& nav, const glm::vec3& p) {
    std::vector<NavPoint> out;
    for (uint32_t t = 0; t < nav.triangleCount(); ++t) {
        const NavTriangle& tri = nav.triangle(t);
        for (uint32_t e = 0; e < 3; ++e) {
            const glm::vec3 a = nav.vertex(tri.v[e]);
            const glm::vec3 ab = nav.vertex(tri.v[(e + 1) % 3]) - a;
            const float s = glm::dot(p - a, ab) / glm::dot(ab, ab);
            if (s < 0.0f || s > 1.0f) continue;
            if (glm::length(p - (a + ab * s)) > 1e-5f) continue;
            out.push_back({t, p});
            break;
        }
    }
    return out;
}

float pathLength(const std::vector<glm::vec3>& path) {
    float total = 0.0f;
    for (size_t i = 1; i < path.size(); ++i) {
        const glm::vec3 d = path[i] - path[i - 1];
        total += std::sqrt(d.x * d.x + d.z * d.z);
    }
    return total;
}

} // namespace

// ==================================================================== bake

TEST(NavMesh, AFlatFloorBakesToOneConnectedRegion) {
    const Soup s = floorOf(4, 4, [](int, int) { return true; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    EXPECT_EQ(nav.triangleCount(), 32u); // 16 cells, two triangles each.
    EXPECT_EQ(nav.regionCount(), 1u);
    // 5x5 grid of corners: only welding gets this down from 64 authored vertices.
    EXPECT_EQ(nav.vertexCount(), 25u);
}

TEST(NavMesh, WeldingIsWhatCreatesAdjacency) {
    const Soup s = floorOf(2, 1, [](int, int) { return true; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());
    ASSERT_EQ(nav.triangleCount(), 4u);

    uint32_t linked = 0;
    for (uint32_t t = 0; t < nav.triangleCount(); ++t) {
        for (const uint32_t n : nav.triangle(t).neighbour) {
            if (n != NavMesh::kNoTriangle) ++linked;
        }
    }
    // Three interior edges, each counted from both sides.
    EXPECT_EQ(linked, 6u);
}

TEST(NavMesh, WeldingIsAboutDriftNotExactness) {
    // Exactly coincident corners weld at any epsilon, so the parameter is not really about
    // tiling -- it is about a mesh that has been through an exporter. A millimetre of drift
    // is what an epsilon too small leaves as three disconnected tiles, and it is what a
    // real glTF arrives carrying.
    Soup s;
    addCell(s, 0.0f, 0.0f, 1.0f);
    addCell(s, 1.001f, 0.0f, 1.0f);
    addCell(s, 2.002f, 0.0f, 1.0f);

    NavBuildParams tight = sharpParams();
    tight.weldEpsilon = 1e-5f;
    NavMesh split;
    split.bake(s.positions, s.indices, tight);
    EXPECT_EQ(split.regionCount(), 3u);

    NavBuildParams loose = sharpParams();
    loose.weldEpsilon = 0.01f;
    NavMesh joined;
    joined.bake(s.positions, s.indices, loose);
    EXPECT_EQ(joined.regionCount(), 1u);
}

TEST(NavMesh, AVerticalWallIsNotWalkable) {
    Soup s;
    const auto base = static_cast<uint32_t>(s.positions.size());
    s.positions.push_back({0.0f, 0.0f, 0.0f});
    s.positions.push_back({0.0f, 2.0f, 0.0f});
    s.positions.push_back({2.0f, 2.0f, 0.0f});
    for (const uint32_t i : {0u, 1u, 2u}) s.indices.push_back(base + i);

    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());
    EXPECT_TRUE(nav.empty());
}

TEST(NavMesh, ACeilingIsNotAFloor) {
    // Wound the other way, so its normal points down. Absolute-value slope filtering would
    // accept this and bake the underside of every floor in the scene.
    Soup s;
    s.positions = {{0.0f, 3.0f, 0.0f}, {1.0f, 3.0f, 0.0f}, {1.0f, 3.0f, 1.0f}, {0.0f, 3.0f, 1.0f}};
    s.indices = {0, 1, 2, 0, 2, 3};

    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());
    EXPECT_TRUE(nav.empty());
}

TEST(NavMesh, SlopeIsATheshholdAndTheThresholdIsHonoured) {
    // Two ramps either side of 45 degrees, baked with the same parameters.
    const auto ramp = [](float rise) {
        Soup s;
        s.positions = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, rise, 1.0f}, {1.0f, rise, 0.0f}};
        s.indices = {0, 1, 2, 0, 2, 3};
        return s;
    };

    NavMesh gentle;
    gentle.bake(ramp(0.5f).positions, ramp(0.5f).indices, sharpParams()); // ~27 degrees
    EXPECT_FALSE(gentle.empty());

    NavMesh steep;
    steep.bake(ramp(2.0f).positions, ramp(2.0f).indices, sharpParams()); // ~63 degrees
    EXPECT_TRUE(steep.empty());
}

// -------------------------------------------------------------- what stands on the floor

TEST(NavMesh, APillarStandingOnAFloorIsCutOutOfIt) {
    // **The floor is one quad**, which is how a floor is authored and what made this
    // findable: there is no tessellation to hide behind, so a bake that cannot see the
    // pillar hands back two triangles and routes every agent straight through it.
    // **Clear of the quad's own diagonal**, which is not fussiness: a corner of the hole
    // sitting exactly on the edge where the floor's two triangles meet is a portal of zero
    // width, and A* then finds a corridor the funnel cannot pull tight through. That is a
    // property of a corridor search rather than of the cut, and it is not what is under test.
    Soup s;
    addCell(s, -5.0f, -5.0f, 10.0f);
    addBox(s, {-1.0f, 0.0f, 2.0f}, {1.0f, 3.0f, 4.0f});

    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    const NavPoint from = nav.nearest({-4.0f, 0.0f, 3.0f});
    const NavPoint to = nav.nearest({4.0f, 0.0f, 3.0f});
    ASSERT_TRUE(from);
    ASSERT_TRUE(to);

    // Cutting a hole in a floor must not cut the floor in half.
    EXPECT_TRUE(nav.reachable(from, to));
    // The pillar's top is walkable and is a place; it is simply not one you can walk to.
    const NavPoint above = nav.nearest({0.0f, 3.0f, 3.0f});
    ASSERT_TRUE(above);
    EXPECT_FALSE(nav.reachable(from, above));

    EXPECT_FALSE(nav.raycast(from, to.position));

    std::vector<glm::vec3> path;
    ASSERT_TRUE(nav.findPath(from, to, path));
    EXPECT_GT(path.size(), 2u);
    // Round two corners of the footprint rather than through it: 3.162 out, 2 across, 3.162
    // back. Written as arithmetic because a length lifted off a passing run cannot be wrong.
    EXPECT_NEAR(pathLength(path), 2.0f * std::sqrt(10.0f) + 2.0f, 0.02f);
}

TEST(NavMesh, TheCutIsTheFootprintAndNotAQuantisationOfIt) {
    // The property the whole triangle approach is here for, and the one a voxel field cannot
    // offer: there is no cell size, so the hole's edge is the pillar's edge to the millimetre
    // rather than to the nearest cell.
    Soup s;
    addCell(s, -5.0f, -5.0f, 10.0f);
    addBox(s, {-1.0f, 0.0f, -1.0f}, {1.0f, 3.0f, 1.0f});

    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    EXPECT_TRUE(nav.nearest({1.02f, 0.0f, 0.0f}, 0.01f));
    EXPECT_FALSE(nav.nearest({0.98f, 0.0f, 0.0f}, 0.01f));
}

TEST(NavMesh, AFloorTileInsideAPillarIsNotWalkable) {
    // The tessellated case, and the one splitting alone does not answer: nothing crosses a
    // tile that is wholly under the pillar, so it is never cut -- it is simply inside, and
    // what settles that is what is above it rather than what split it.
    Soup s = floorOf(9, 9, [](int, int) { return true; });
    addBox(s, {3.0f, 0.0f, 3.0f}, {6.0f, 3.0f, 6.0f});

    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    EXPECT_FALSE(nav.nearest({4.5f, 0.0f, 4.5f}, 0.4f));
    EXPECT_TRUE(nav.nearest({1.5f, 0.0f, 1.5f}, 0.4f));
}

TEST(NavMesh, APillarElsewhereDoesNotBendAPathAcrossOpenFloor) {
    // Cutting a hole leaves the rest of the floor in pieces, and a path across it now crosses
    // portals where it used to cross nothing. **Default parameters on purpose**, agent radius
    // included: every one of those portals is inset, and a path that bends at each is an agent
    // that weaves across an empty room because something is standing in the far corner.
    Soup s;
    addCell(s, -5.0f, -5.0f, 10.0f);
    addBox(s, {-1.0f, 0.0f, 2.0f}, {1.0f, 3.0f, 4.0f});

    NavMesh nav;
    nav.bake(s.positions, s.indices, {});

    const NavPoint from = nav.nearest({-4.0f, 0.0f, -3.0f});
    const NavPoint to = nav.nearest({4.0f, 0.0f, -3.0f});
    ASSERT_TRUE(from);
    ASSERT_TRUE(to);

    std::vector<glm::vec3> path;
    ASSERT_TRUE(nav.findPath(from, to, path));
    EXPECT_EQ(path.size(), 2u);
    EXPECT_NEAR(pathLength(path), 8.0f, 1e-3f);
}

TEST(NavMesh, AFloorUnderABridgeIsStillWalkable) {
    // **Standing on and above are different questions**, and only the first one cuts.
    // Clearance is not modelled here and that is deliberate -- see the file comment -- so a
    // bridge crossing over a floor must take nothing away from it.
    Soup s;
    addCell(s, -5.0f, -5.0f, 10.0f);
    addBox(s, {-1.0f, 2.0f, -5.0f}, {1.0f, 3.0f, 5.0f});

    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    const NavPoint from = nav.nearest({-4.0f, 0.0f, 0.0f});
    const NavPoint to = nav.nearest({4.0f, 0.0f, 0.0f});
    ASSERT_TRUE(from);
    ASSERT_TRUE(to);
    EXPECT_TRUE(nav.raycast(from, to.position));

    std::vector<glm::vec3> path;
    ASSERT_TRUE(nav.findPath(from, to, path));
    EXPECT_EQ(path.size(), 2u);
    EXPECT_NEAR(pathLength(path), 8.0f, 1e-3f);
}

TEST(NavMesh, UnreachableScrapsAreDropped) {
    // A big floor and a one-metre shelf off on its own -- the windowsill every real scene
    // bakes dozens of. Without the cull, `nearest` can snap an agent onto it and every
    // path from there fails.
    Soup s = floorOf(4, 4, [](int, int) { return true; });
    addCell(s, 100.0f, 100.0f, 1.0f, 5.0f);

    NavBuildParams p;
    p.agentRadius = 0.0f;
    p.minRegionArea = 2.0f;
    NavMesh nav;
    nav.bake(s.positions, s.indices, p);

    EXPECT_EQ(nav.regionCount(), 1u);
    EXPECT_EQ(nav.triangleCount(), 32u);
    EXPECT_FALSE(nav.nearest({100.5f, 5.0f, 100.5f}, 2.0f));
}

TEST(NavMesh, TwoRoomsThatDoNotTouchAreTwoRegions) {
    Soup s = floorOf(2, 2, [](int, int) { return true; });
    Soup far = floorOf(2, 2, [](int, int) { return true; });
    for (glm::vec3& p : far.positions) p.x += 50.0f;
    const auto base = static_cast<uint32_t>(s.positions.size());
    s.positions.insert(s.positions.end(), far.positions.begin(), far.positions.end());
    for (const uint32_t i : far.indices) s.indices.push_back(base + i);

    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());
    EXPECT_EQ(nav.regionCount(), 2u);

    const NavPoint a = nav.nearest({0.5f, 0.0f, 0.5f});
    const NavPoint b = nav.nearest({50.5f, 0.0f, 0.5f});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_FALSE(nav.reachable(a, b));

    std::vector<glm::vec3> path;
    EXPECT_FALSE(nav.findPath(a, b, path));
    EXPECT_TRUE(path.empty());
}

// ==================================================================== queries

TEST(NavMesh, NearestSnapsOntoTheSurface) {
    const Soup s = floorOf(4, 4, [](int, int) { return true; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    const NavPoint p = nav.nearest({2.0f, 3.0f, 2.0f}, 4.0f);
    ASSERT_TRUE(p);
    EXPECT_NEAR(p.position.y, 0.0f, 1e-4f);
    EXPECT_NEAR(p.position.x, 2.0f, 1e-4f);
    EXPECT_NEAR(p.position.z, 2.0f, 1e-4f);
}

TEST(NavMesh, NearestRefusesRatherThanTeleporting) {
    const Soup s = floorOf(2, 2, [](int, int) { return true; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());
    // The difference between "the agent stepped off the mesh" and "the agent went home".
    EXPECT_FALSE(nav.nearest({500.0f, 0.0f, 500.0f}, 4.0f));
    EXPECT_TRUE(nav.nearest({500.0f, 0.0f, 500.0f}, 1000.0f));
}

TEST(NavMesh, DropToFloorPrefersTheSurfaceBelowNotTheNearestOne) {
    // A ground floor and a balcony directly above it. An agent falling from above the
    // balcony lands on the balcony; one standing just under it lands on the ground.
    Soup s = floorOf(4, 4, [](int, int) { return true; });
    addCell(s, 1.0f, 1.0f, 1.0f, 3.0f);

    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    const NavPoint high = nav.dropToFloor({1.5f, 10.0f, 1.5f}, 20.0f);
    ASSERT_TRUE(high);
    EXPECT_NEAR(high.position.y, 3.0f, 1e-3f);

    const NavPoint low = nav.dropToFloor({1.5f, 2.5f, 1.5f}, 20.0f);
    ASSERT_TRUE(low);
    EXPECT_NEAR(low.position.y, 0.0f, 1e-3f);

    EXPECT_FALSE(nav.dropToFloor({1.5f, 10.0f, 1.5f}, 1.0f));
}

// ==================================================================== paths

TEST(NavMesh, AnOpenPlaneGivesAStraightLine) {
    // The whole point of string-pulling. The corridor across this floor is a dozen
    // triangles; the path is two points, because nothing in between is a turn.
    const Soup s = floorOf(6, 6, [](int, int) { return true; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    const NavPoint a = nav.nearest({0.5f, 0.0f, 0.5f});
    const NavPoint b = nav.nearest({5.5f, 0.0f, 5.5f});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    std::vector<uint32_t> corridor;
    ASSERT_TRUE(nav.findCorridor(a, b, corridor));
    EXPECT_GT(corridor.size(), 2u) << "the corridor really does cross many triangles";

    std::vector<glm::vec3> path;
    ASSERT_TRUE(nav.findPath(a, b, path));
    ASSERT_EQ(path.size(), 2u);
    EXPECT_NEAR(pathLength(path), std::sqrt(50.0f), 1e-3f);
}

TEST(NavMesh, AnLShapedCorridorHugsItsInnerCorner) {
    // A corridor of triangles is already *a* path, so this asserts where the corner is
    // rather than merely that a path exists. It is not the check on the funnel's left and
    // right: one turn is little enough that the smoothing pass pulls a mirrored funnel's
    // output back onto the same three waypoints. Two turns is not --
    // `AUShapedCorridorInXYTurnsInsideBothOfItsCorners`.
    const Soup s = floorOf(6, 6, [](int i, int j) { return j == 0 || i == 5; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());
    ASSERT_EQ(nav.regionCount(), 1u);

    const NavPoint a = nav.nearest({0.5f, 0.0f, 0.5f});
    const NavPoint b = nav.nearest({5.5f, 0.0f, 5.5f});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    std::vector<glm::vec3> path;
    ASSERT_TRUE(nav.findPath(a, b, path));
    ASSERT_EQ(path.size(), 3u) << "start, the corner, and the goal";
    EXPECT_NEAR(path[1].x, 5.0f, 1e-3f);
    EXPECT_NEAR(path[1].z, 1.0f, 1e-3f);

    // And it is shorter than going around the outside would be.
    EXPECT_LT(pathLength(path), 11.0f);
}

TEST(NavMesh, TheCorridorIsActuallyConnected) {
    const Soup s = floorOf(6, 6, [](int i, int j) { return j == 0 || i == 5; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    std::vector<uint32_t> corridor;
    ASSERT_TRUE(nav.findCorridor(nav.nearest({0.5f, 0.0f, 0.5f}), nav.nearest({5.5f, 0.0f, 5.5f}), corridor));
    ASSERT_GE(corridor.size(), 2u);

    for (size_t i = 0; i + 1 < corridor.size(); ++i) {
        const NavTriangle& t = nav.triangle(corridor[i]);
        const bool adjacent = t.neighbour[0] == corridor[i + 1] || t.neighbour[1] == corridor[i + 1] ||
                              t.neighbour[2] == corridor[i + 1];
        EXPECT_TRUE(adjacent) << "step " << i << " of the corridor is not an edge";
    }
}

TEST(NavMesh, APathToWhereYouAlreadyStandIsTwoPoints) {
    const Soup s = floorOf(4, 4, [](int, int) { return true; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    const NavPoint a = nav.nearest({1.2f, 0.0f, 1.2f});
    const NavPoint b = nav.nearest({1.3f, 0.0f, 1.3f});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    std::vector<glm::vec3> path;
    ASSERT_TRUE(nav.findPath(a, b, path));
    EXPECT_EQ(path.size(), 2u);
}

TEST(NavMesh, TheAgentRadiusKeepsThePathOffTheCorner) {
    // Same L, with a radius. The corner waypoint must move *into* the corridor, because a
    // path through the exact corner is a path an agent with width clips.
    const Soup s = floorOf(6, 6, [](int i, int j) { return j == 0 || i == 5; });

    NavBuildParams p;
    p.agentRadius = 0.3f;
    p.minRegionArea = 0.0f;
    NavMesh nav;
    nav.bake(s.positions, s.indices, p);

    const NavPoint a = nav.nearest({0.5f, 0.0f, 0.5f});
    const NavPoint b = nav.nearest({5.5f, 0.0f, 5.5f});
    std::vector<glm::vec3> path;
    ASSERT_TRUE(nav.findPath(a, b, path));
    ASSERT_GE(path.size(), 3u);

    // The inner corner is at (5, 1). Every waypoint between the ends must clear it.
    for (size_t i = 1; i + 1 < path.size(); ++i) {
        const float dx = path[i].x - 5.0f;
        const float dz = path[i].z - 1.0f;
        EXPECT_GT(std::sqrt(dx * dx + dz * dz), 0.1f) << "waypoint " << i << " is on the corner";
    }
}

TEST(NavMesh, AFalsyEndpointIsRefusedRatherThanCrashed) {
    const Soup s = floorOf(2, 2, [](int, int) { return true; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    std::vector<glm::vec3> path;
    path.push_back({1.0f, 2.0f, 3.0f}); // Stale content, to prove it is cleared.
    EXPECT_FALSE(nav.findPath(NavPoint{}, nav.nearest({0.5f, 0.0f, 0.5f}), path));
    EXPECT_TRUE(path.empty());
    EXPECT_FALSE(nav.findPath(nav.nearest({0.5f, 0.0f, 0.5f}), NavPoint{}, path));
}

TEST(NavMesh, AnEmptyMeshAnswersEverythingWithNo) {
    const NavMesh nav;
    EXPECT_TRUE(nav.empty());
    EXPECT_FALSE(nav.nearest({0.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(nav.dropToFloor({0.0f, 0.0f, 0.0f}));
    std::vector<glm::vec3> path;
    EXPECT_FALSE(nav.findPath(NavPoint{}, NavPoint{}, path));
}

TEST(NavMesh, ASecondBakeLeavesNoTraceOfTheFirst) {
    const Soup big = floorOf(6, 6, [](int, int) { return true; });
    const Soup small = floorOf(2, 2, [](int, int) { return true; });

    NavMesh nav;
    nav.bake(big.positions, big.indices, sharpParams());
    ASSERT_EQ(nav.triangleCount(), 72u);
    nav.bake(small.positions, small.indices, sharpParams());
    EXPECT_EQ(nav.triangleCount(), 8u);
    EXPECT_EQ(nav.vertexCount(), 9u);
    EXPECT_FALSE(nav.nearest({5.5f, 0.0f, 5.5f}, 1.0f));
}

// ==================================================================== steering

TEST(PathFollower, WalksThePathAndStops) {
    PathFollower f;
    f.reset({{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}});

    glm::vec3 position(0.0f);
    for (int i = 0; i < 2000 && !f.done(); ++i) {
        position += steer(f, position, 5.0f) * (1.0f / 60.0f);
    }

    EXPECT_TRUE(f.done());
    EXPECT_NEAR(position.x, 10.0f, 0.2f);
    EXPECT_EQ(steer(f, position, 5.0f), glm::vec3(0.0f));
}

TEST(PathFollower, SteeringIsHorizontal) {
    // A ramp must not slow an agent down for climbing it, and must not drive the agent
    // into the floor either -- the Y component belongs to whatever owns gravity.
    PathFollower f;
    f.reset({{0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 0.0f}});
    const glm::vec3 v = steer(f, {0.0f, 0.0f, 0.0f}, 5.0f);
    EXPECT_FLOAT_EQ(v.y, 0.0f);
    EXPECT_NEAR(v.x, 5.0f, 1e-4f);
}

TEST(PathFollower, OneSlowFrameDoesNotOrbitACornerItAlreadyPassed) {
    // Four waypoints, and a first call made from beyond the third. A follower that
    // advanced one waypoint per call would turn around and walk back.
    PathFollower f;
    f.reset({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}});

    const glm::vec3 v = steer(f, {2.0f, 0.0f, 0.0f}, 5.0f);
    EXPECT_EQ(f.waypoint, 3u);
    EXPECT_GT(v.x, 0.0f) << "it is walking forward, not back to waypoint 1";
}

TEST(PathFollower, ArrivalSlowsOnlyOnTheLastWaypoint) {
    PathFollower f;
    f.reset({{0.0f, 0.0f, 0.0f}, {0.6f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}});
    // Just outside the waypoint radius of an intermediate corner: full speed, because
    // easing into every turn is how an agent ends up crawling around a building.
    EXPECT_NEAR(glm::length(steer(f, {0.0f, 0.0f, 0.0f}, 5.0f)), 5.0f, 1e-4f);

    PathFollower g;
    g.reset({{0.0f, 0.0f, 0.0f}, {0.25f, 0.0f, 0.0f}});
    g.waypointRadius = 0.01f;
    EXPECT_LT(glm::length(steer(g, {0.0f, 0.0f, 0.0f}, 5.0f)), 5.0f);
}

TEST(PathFollower, AnEmptyPathIsAlreadyDone) {
    PathFollower f;
    EXPECT_TRUE(f.done());
    EXPECT_EQ(steer(f, {0.0f, 0.0f, 0.0f}, 5.0f), glm::vec3(0.0f));
}

TEST(PathFollower, ABakedPathCanBeWalkedEndToEnd) {
    // The three stages together, which is the only test here that would catch a mismatch
    // between them -- a path whose waypoints are ordered goal-first, say.
    const Soup s = floorOf(6, 6, [](int i, int j) { return j == 0 || i == 5; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    std::vector<glm::vec3> path;
    ASSERT_TRUE(nav.findPath(nav.nearest({0.5f, 0.0f, 0.5f}), nav.nearest({5.5f, 0.0f, 5.5f}), path));

    PathFollower f;
    f.reset(path);
    glm::vec3 position = path.front();
    for (int i = 0; i < 4000 && !f.done(); ++i) {
        position += steer(f, position, 3.0f) * (1.0f / 60.0f);
        // It never leaves the corridor it was routed through.
        ASSERT_TRUE(nav.nearest(position, 1.0f)) << "step " << i << " walked off the mesh";
    }
    EXPECT_TRUE(f.done());
    EXPECT_NEAR(position.x, 5.5f, 0.3f);
    EXPECT_NEAR(position.z, 5.5f, 0.3f);
}

// ==================================================================== visibility

TEST(NavMesh, RaycastSeesAcrossAnOpenFloor) {
    const Soup s = floorOf(6, 6, [](int, int) { return true; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    const NavPoint a = nav.nearest({0.5f, 0.0f, 0.5f});
    ASSERT_TRUE(a);
    EXPECT_TRUE(nav.raycast(a, {5.5f, 0.0f, 5.5f}));
    // And stops at the edge of the world rather than reporting success past it.
    EXPECT_FALSE(nav.raycast(a, {50.0f, 0.0f, 50.0f}));
}

TEST(NavMesh, RaycastDoesNotSeeRoundACorner) {
    // The L again. Its two ends are not visible to each other, which is the whole reason
    // the path through it needs a corner.
    const Soup s = floorOf(6, 6, [](int i, int j) { return j == 0 || i == 5; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    const NavPoint a = nav.nearest({0.5f, 0.0f, 0.5f});
    ASSERT_TRUE(a);
    EXPECT_FALSE(nav.raycast(a, {5.5f, 0.0f, 5.5f}));
    EXPECT_TRUE(nav.raycast(a, {4.5f, 0.0f, 0.5f})) << "straight down the first leg";
}

TEST(NavMesh, RaycastAcrossAGapIsBlocked) {
    Soup s = floorOf(2, 1, [](int, int) { return true; });
    addCell(s, 5.0f, 0.0f, 1.0f);
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    const NavPoint a = nav.nearest({0.5f, 0.0f, 0.5f});
    ASSERT_TRUE(a);
    EXPECT_FALSE(nav.raycast(a, {5.5f, 0.0f, 0.5f}));
}

TEST(NavMesh, AWideAgentDoesNotFitWhereAThinOneDoes) {
    // A one-metre corridor. The centre line is clear for anything; a two-metre-wide band
    // is not, and `corridorClear` is the difference between the two.
    const Soup s = floorOf(6, 3, [](int, int j) { return j == 1; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    const glm::vec3 from{0.5f, 0.0f, 1.5f};
    const glm::vec3 to{5.5f, 0.0f, 1.5f};
    EXPECT_TRUE(nav.corridorClear(from, to, 0.0f));
    EXPECT_TRUE(nav.corridorClear(from, to, 0.3f));
    EXPECT_FALSE(nav.corridorClear(from, to, 1.0f));
}

/**
 * **A line that lies entirely on the mesh is not an obstruction.** A grid of square cells is
 * the shape that makes this the normal case rather than a curiosity: a ray along a tile
 * boundary leaves every triangle it crosses exactly through a vertex, so the separation test
 * that picks the exit edge ties at zero at every step. Answering "neither edge" there is what
 * reports a clear line blocked.
 *
 * Asserted from **both** triangles sharing the boundary, because only one of the two is wrong:
 * which one `nearest` hands back is decided by BVH order and by which comparison rounded to
 * exactly zero, which is why the same world answered differently in Debug and in Release.
 */
TEST(NavMesh, ARayAlongATileBoundaryIsClear) {
    const Soup s = floorOf(6, 6, [](int, int) { return true; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());

    // Mid-edge to mid-edge, then corner to corner: the second starts on a grid vertex, which
    // is where six triangles meet and five of them are not the one the ray travels through.
    for (const auto& span : {std::pair<glm::vec3, glm::vec3>{{3.0f, 0.0f, 0.5f}, {3.0f, 0.0f, 5.5f}},
                             std::pair<glm::vec3, glm::vec3>{{0.5f, 0.0f, 3.0f}, {5.5f, 0.0f, 3.0f}},
                             std::pair<glm::vec3, glm::vec3>{{3.0f, 0.0f, 1.0f}, {3.0f, 0.0f, 5.0f}}}) {
        const std::vector<NavPoint> starts = startsAt(nav, span.first);
        ASSERT_FALSE(starts.empty());
        for (const NavPoint& a : starts) {
            EXPECT_TRUE(nav.raycast(a, span.second))
                << "from triangle " << a.triangle << " at (" << span.first.x << ", " << span.first.z << ")";
        }
        EXPECT_TRUE(nav.corridorClear(span.first, span.second, 0.0f));
    }
}

TEST(NavMesh, AFalsyStartSeesNothing) {
    const Soup s = floorOf(2, 2, [](int, int) { return true; });
    NavMesh nav;
    nav.bake(s.positions, s.indices, sharpParams());
    EXPECT_FALSE(nav.raycast(NavPoint{}, {0.5f, 0.0f, 0.5f}));
    EXPECT_FALSE(nav.corridorClear({500.0f, 0.0f, 500.0f}, {0.5f, 0.0f, 0.5f}, 0.0f));
}

// ================================================ the plane a flat world lies in

namespace {

/// The same quad as `addCell`, in the **XY** plane and wound so its normal points along
/// +Z. This is the floor a 2D game has: `ColliderFreedom::Plane2D` is X and Y translation
/// with Z rotation, gravity is -Y, and the orthographic camera looks down -Z.
void addCellXY(Soup& s, float x, float y, float cell, float z = 0.0f) {
    const auto base = static_cast<uint32_t>(s.positions.size());
    s.positions.push_back({x, y, z});
    s.positions.push_back({x + cell, y, z});
    s.positions.push_back({x + cell, y + cell, z});
    s.positions.push_back({x, y + cell, z});
    for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) s.indices.push_back(base + i);
}

/// `floorOf`'s counterpart, cell for cell, so a world and its XZ twin differ in nothing but
/// which plane they are in.
template <typename F> Soup floorOfXY(int w, int h, F filled, float cell = 1.0f) {
    Soup s;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            if (filled(i, j)) addCellXY(s, static_cast<float>(i) * cell, static_cast<float>(j) * cell, cell);
        }
    }
    return s;
}

Soup flatWorldXY(int w, int h, float cell = 1.0f) {
    return floorOfXY(w, h, [](int, int) { return true; }, cell);
}

} // namespace

/**
 * **The disagreement this row was opened for.** 2D physics lives in XY; navigation baked
 * only XZ. So a 2D game's bodies and the only navmesh it could build were in perpendicular
 * planes, and one of the two had to be rewritten by the game.
 *
 * The first arm is what the tree did before: an XY floor baked against +Y is not a floor at
 * all, it is a wall, and every triangle fails the slope filter in silence.
 */
TEST(NavMesh, AFlatWorldInXYBakesOnlyWhenItSaysWhichWayIsUp) {
    const Soup s = flatWorldXY(4, 4);

    NavMesh wrongPlane;
    wrongPlane.bake(s.positions, s.indices, sharpParams());
    EXPECT_TRUE(wrongPlane.empty()) << "an XY floor read as XZ is a wall, and a wall is not walkable";

    NavBuildParams p = sharpParams();
    p.up = {0.0f, 0.0f, 1.0f};
    NavMesh nav;
    nav.bake(s.positions, s.indices, p);
    EXPECT_EQ(nav.triangleCount(), 32u);
    EXPECT_EQ(nav.regionCount(), 1u);

    // And the vertices come back where they were put, not in the solver's own frame.
    bool sawOrigin = false;
    for (uint32_t i = 0; i < nav.vertexCount(); ++i) {
        EXPECT_NEAR(nav.vertex(i).z, 0.0f, 1e-5f);
        if (glm::length(nav.vertex(i)) < 1e-5f) sawOrigin = true;
    }
    EXPECT_TRUE(sawOrigin);
}

TEST(NavMesh, APathAcrossAFlatWorldStaysInTheFlatWorld) {
    const Soup s = flatWorldXY(4, 4);
    NavBuildParams p = sharpParams();
    p.up = {0.0f, 0.0f, 1.0f};
    NavMesh nav;
    nav.bake(s.positions, s.indices, p);
    ASSERT_FALSE(nav.empty());

    const NavPoint from = nav.nearest({0.5f, 0.5f, 0.0f});
    const NavPoint to = nav.nearest({3.5f, 3.5f, 0.0f});
    ASSERT_TRUE(from);
    ASSERT_TRUE(to);
    EXPECT_NEAR(from.position.x, 0.5f, 1e-4f);
    EXPECT_NEAR(from.position.y, 0.5f, 1e-4f);
    EXPECT_NEAR(from.position.z, 0.0f, 1e-4f);

    std::vector<glm::vec3> path;
    ASSERT_TRUE(nav.findPath(from, to, path));
    ASSERT_GE(path.size(), 2u);
    // An open plane straightens to its two endpoints, exactly as the XZ case does. What
    // that says is that the path is straight, and nothing about left and right: a funnel
    // with its sides swapped still crosses an open floor by the shortest route, so the
    // handedness of the rotation is `AUShapedCorridorInXYTurnsInsideBothOfItsCorners`'s
    // claim and not this one's.
    EXPECT_EQ(path.size(), 2u);
    for (const glm::vec3& w : path) EXPECT_NEAR(w.z, 0.0f, 1e-4f);
    EXPECT_NEAR(glm::length(path.back() - path.front()), std::sqrt(2.0f) * 3.0f, 0.01f);
}

/**
 * **The handedness check, and the reason it is a U and not an L or an open plane.** Swap
 * the funnel's left and right and the corridor's own vertices come back instead of the
 * corners; the smoothing pass then pulls that straight again, and across an open plane or
 * round a single L it lands on the same waypoints the correct funnel found. It is the
 * *second* turn that it cannot repair -- a mirrored funnel leaves this path five waypoints
 * long, out at (1, 0) on the outer wall and a metre and a half further walked.
 *
 * So this is the test the rotation is watched by. It is in XY because that is the frame
 * the rotation exists for, and a rotation that flipped handedness would swap left and
 * right for exactly the meshes it applies to.
 */
TEST(NavMesh, AUShapedCorridorInXYTurnsInsideBothOfItsCorners) {
    const Soup s = floorOfXY(7, 7, [](int i, int j) { return j == 0 || i == 0 || i == 6; });
    NavBuildParams p = sharpParams();
    p.up = {0.0f, 0.0f, 1.0f};
    NavMesh nav;
    nav.bake(s.positions, s.indices, p);
    ASSERT_EQ(nav.regionCount(), 1u);

    const NavPoint a = nav.nearest({0.5f, 6.5f, 0.0f});
    const NavPoint b = nav.nearest({6.5f, 6.5f, 0.0f});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    std::vector<glm::vec3> path;
    ASSERT_TRUE(nav.findPath(a, b, path));
    ASSERT_EQ(path.size(), 4u) << "start, both inner corners, and the goal";
    EXPECT_NEAR(path[1].x, 1.0f, 1e-3f);
    EXPECT_NEAR(path[1].y, 1.0f, 1e-3f);
    EXPECT_NEAR(path[2].x, 6.0f, 1e-3f);
    EXPECT_NEAR(path[2].y, 1.0f, 1e-3f);
    for (const glm::vec3& w : path) EXPECT_NEAR(w.z, 0.0f, 1e-4f);

    // `pathLength` measures XZ, which in this plane is one of the two axes travelled.
    float walked = 0.0f;
    for (size_t i = 1; i < path.size(); ++i) walked += glm::length(path[i] - path[i - 1]);
    EXPECT_LT(walked, 16.5f) << "round the outside is 17.5";
}

/**
 * **The row's actual claim: the two subsystems agree.** A `Plane2D` body has X and Y
 * translation and Z rotation and nothing else, so a path in XZ hands it a steering vector
 * whose only usable component is X -- the body slides along one axis and never arrives,
 * and no error is reported anywhere because both halves are behaving exactly as
 * documented. Baked in the body's own plane, it walks the path and stops.
 *
 * `PathFollower::up` is the second half of it. A follower dropping +Y measures its progress
 * along the one axis the agent is not travelling on, so every waypoint stays a metre and a
 * half away forever.
 */
TEST(NavMesh, APlane2DBodyWalksAPathBakedInItsOwnPlane) {
    const Soup s = flatWorldXY(6, 6);
    NavBuildParams p = sharpParams();
    p.up = {0.0f, 0.0f, 1.0f};
    NavMesh nav;
    nav.bake(s.positions, s.indices, p);
    ASSERT_FALSE(nav.empty());

    const NavPoint from = nav.nearest({0.5f, 0.5f, 0.0f});
    const NavPoint to = nav.nearest({5.5f, 5.5f, 0.0f});
    ASSERT_TRUE(from);
    ASSERT_TRUE(to);

    PathFollower follower;
    follower.up = {0.0f, 0.0f, 1.0f};
    {
        std::vector<glm::vec3> path;
        ASSERT_TRUE(nav.findPath(from, to, path));
        follower.reset(std::move(path));
    }

    // A top-down flat world, so no gravity: down is the camera's business and there is no
    // floor to fall onto.
    PhysicsConfig cfg;
    cfg.gravity = {0.0f, 0.0f, 0.0f};
    PhysicsWorld world;
    world.init(cfg, 4);

    ColliderDesc agent;
    agent.name = "agent";
    agent.shape = ColliderShape::Box;
    agent.motion = ColliderMotion::Dynamic;
    agent.halfExtent = {0.2f, 0.2f, 0.2f};
    agent.freedom = ColliderFreedom::Plane2D;
    agent.transform = glm::translate(glm::mat4(1.0f), from.position);
    const BodyId body = world.createBody(agent);
    ASSERT_TRUE(body.valid());
    world.finalize();

    constexpr float kStep = 1.0f / 60.0f;
    for (int i = 0; i < 600 && !follower.done(); ++i) {
        const glm::vec3 at(world.bodyTransform(body, 0.0f)[3]);
        world.setLinearVelocity(body, steer(follower, at, 3.0f));
        world.step(kStep);
    }

    EXPECT_TRUE(follower.done()) << "the agent never finished the path";
    const glm::vec3 at(world.bodyTransform(body, 0.0f)[3]);
    EXPECT_NEAR(at.x, to.position.x, 0.6f);
    EXPECT_NEAR(at.y, to.position.y, 0.6f);
    // And it never left the plane, which is what `Plane2D` is for and what a path in the
    // wrong plane would have been asking it to do.
    EXPECT_NEAR(at.z, 0.0f, 1e-4f);
}

/**
 * **The same line, in the plane a tilemap is actually built in.** A square grid is what D18
 * opened the door to, and a path along a tile boundary is the ordinary case there rather than
 * a degenerate one -- so the answer must not depend on which of the two triangles sharing the
 * boundary the query started in, nor on the frame the mesh was baked in.
 */
TEST(NavMesh, ARayAlongATileBoundaryIsClearInXY) {
    const Soup s = flatWorldXY(6, 6);
    NavBuildParams p = sharpParams();
    p.up = {0.0f, 0.0f, 1.0f};
    NavMesh nav;
    nav.bake(s.positions, s.indices, p);
    ASSERT_FALSE(nav.empty());

    for (const auto& span : {std::pair<glm::vec3, glm::vec3>{{3.0f, 0.5f, 0.0f}, {3.0f, 5.5f, 0.0f}},
                             std::pair<glm::vec3, glm::vec3>{{0.5f, 3.0f, 0.0f}, {5.5f, 3.0f, 0.0f}},
                             std::pair<glm::vec3, glm::vec3>{{3.0f, 1.0f, 0.0f}, {3.0f, 5.0f, 0.0f}}}) {
        const std::vector<NavPoint> starts = startsAt(nav, span.first);
        ASSERT_FALSE(starts.empty());
        for (const NavPoint& a : starts) {
            EXPECT_TRUE(nav.raycast(a, span.second))
                << "from triangle " << a.triangle << " at (" << span.first.x << ", " << span.first.y << ")";
        }
        EXPECT_TRUE(nav.corridorClear(span.first, span.second, 0.0f));
    }
}
