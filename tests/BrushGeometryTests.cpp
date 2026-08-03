#include "scene/Collider.h"
#include "scene/Physics.h"
#include "scene/SceneTypes.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

using namespace scene;

/**
 * @file tests/BrushGeometryTests.cpp
 * @brief C18's outcome: there is no CSG solver, and this is what a game writes instead.
 *
 * The row asked for `engine/csg/` -- a list of half-space brushes clipped against each
 * other into a `scene::MeshData` -- and it was declined.
 * [`limitations.md`](../docs/architecture/limitations.md) carries the argument and the two
 * triggers that would reverse it. **This file is the part of that decision that could
 * otherwise rot**: a refusal whose replacement is a code sketch in a document is a refusal
 * nobody can check, so the sketch lives here, compiles against the real headers, and runs
 * in the suite under every sanitizer.
 *
 * Nothing below is engine code. Every line is what a *game* writes, over things the engine
 * already has:
 *
 * | The brush wants | What answers it | Row |
 * |---|---|---|
 * | A solid, as geometry | A `MeshData`, handed to `Engine::createMesh` | G4 |
 * | A union of solids | Concatenation, or nothing at all -- see below | G4 |
 * | A subtraction | Rectangle arithmetic, or three brushes instead of one | -- |
 * | The solid as collision | One `ColliderDesc` per brush | S4.2 |
 * | Unloading a blockout | `Engine::removeModel` | C10 |
 * | Coarser levels of it | `substrate-bake`, offline | C17 / D9 |
 *
 * The one call this file cannot make is `e.createMesh(std::move(mesh))`, which takes a
 * `VulkanContext` and an `Uploader`. So the tests build the `MeshData` -- which is all of
 * the arithmetic -- and stop one line short of the device, exactly as `TilemapTests.cpp`
 * does for P8.
 *
 * ## Why the union half of CSG is nothing at all
 *
 * Quake solved brushes on the CPU because it had to: a BSP tree, a PVS and a lightmap all
 * need the *surfaces* of the union, so interior faces had to be clipped away before any of
 * the three could be built. This engine has none of them. It has a depth buffer, and two
 * interpenetrating opaque solids drawn through a depth buffer show exactly the boundary of
 * their union. A blockout does not need a solver to look right; it needs the two boxes
 * submitted.
 *
 * What the union would still buy is fewer triangles, and that number has already been
 * measured against this engine and found not to bind: `limitations.md` records 20% fewer
 * triangles moving `GBuffer` 0.481 -> 0.474 ms and `Frame` 3.384 -> 3.339 ms. The whole
 * room below is 148 triangles.
 *
 * ## Why the subtraction half is arithmetic
 *
 * Blockout brushes are boxes, and a rectangle with a rectangular hole in it is eight
 * rectangles. `windowWall` is that, in both directions, and it comes out closed with no
 * epsilon anywhere -- `AWindowWallIsClosedAndEveryEdgeIsMatched` and
 * `EveryCoordinateIsOneTheAuthorWrote` are the two properties a clipper would have had to
 * earn and that this gets by construction.
 *
 * The case that is *not* eight rectangles -- an opening that reaches the floor -- is not a
 * subtraction either. A doorway is a left pier, a right pier and a lintel, which is what a
 * mapper builds and what `doorwayWall` returns: three boxes, no boolean.
 */

namespace {

// ============================================================================ the brush

/// An axis-aligned box, which is what a blockout brush is. Named for what it is rather
/// than `Brush`, because nothing here is a brush in the CSG sense: it is never clipped,
/// never intersected, and never asked whether a point is inside it.
struct Box {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    [[nodiscard]] glm::vec3 centre() const { return (min + max) * 0.5f; }
    [[nodiscard]] glm::vec3 halfExtent() const { return (max - min) * 0.5f; }
};

/// Texture repeats per world unit, so the projection below is a scale a game picks once.
constexpr float kTexelsPerUnit = 0.5f;

/**
 * @brief Texture coordinates for a face, projected along its dominant axis.
 *
 * The card's own expected-wrong-estimate: *"a brush face has no natural UV, and whether
 * that is a projection axis per face or an authored value decides whether this stays L."*
 * It is a projection axis per face, it is four lines, and it is a **world-space**
 * projection rather than a face-local one -- which is the whole reason it works. Two
 * brushes meeting at a plane get continuous texturing without either knowing about the
 * other and without anything welding them; `AdjacentBrushesTextureContinuously` pins it.
 *
 * An engine could not have chosen this for everybody. A side-view game wants one axis, a
 * first-person interior wants three, a stylised one wants the number the artist typed.
 */
[[nodiscard]] glm::vec2 projectUv(const glm::vec3& p, const glm::vec3& normal) {
    const glm::vec3 a = glm::abs(normal);
    if (a.x >= a.y && a.x >= a.z) return {p.z * kTexelsPerUnit, -p.y * kTexelsPerUnit};
    if (a.y >= a.z) return {p.x * kTexelsPerUnit, p.z * kTexelsPerUnit};
    return {p.x * kTexelsPerUnit, -p.y * kTexelsPerUnit};
}

/// One outward-facing quad, wound anticlockwise seen from outside. Four vertices rather
/// than shared corners for `unitCube`'s reason: a box's normals are per face, so a shared
/// corner would have to carry three of them.
void emitQuad(MeshData& mesh, const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
              const glm::vec3& normal) {
    // A tangent perpendicular to the normal, taken off whichever world axis is furthest
    // from it. Arbitrary, and stated as arbitrary: a blockout carries no normal map, and
    // the loader makes the same choice for a mesh that arrives without tangents.
    const glm::vec3 up = glm::abs(normal.y) > 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec4 tangent(glm::normalize(glm::cross(up, normal)), 1.0f);

    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (const glm::vec3& p : {p0, p1, p2, p3}) {
        mesh.vertices.push_back({p, normal, tangent, projectUv(p, normal)});
    }
    for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + i);
}

/// A face of constant x, spanning [y0, y1] x [z0, z1]. `sign` is which way it looks.
void emitFaceX(MeshData& mesh, float x, float y0, float y1, float z0, float z1, float sign) {
    const glm::vec3 n(sign, 0.0f, 0.0f);
    if (sign > 0.0f) {
        emitQuad(mesh, {x, y0, z1}, {x, y0, z0}, {x, y1, z0}, {x, y1, z1}, n);
    } else {
        emitQuad(mesh, {x, y0, z0}, {x, y0, z1}, {x, y1, z1}, {x, y1, z0}, n);
    }
}

void emitFaceY(MeshData& mesh, float y, float x0, float x1, float z0, float z1, float sign) {
    const glm::vec3 n(0.0f, sign, 0.0f);
    if (sign > 0.0f) {
        emitQuad(mesh, {x0, y, z1}, {x1, y, z1}, {x1, y, z0}, {x0, y, z0}, n);
    } else {
        emitQuad(mesh, {x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}, n);
    }
}

void emitFaceZ(MeshData& mesh, float z, float x0, float x1, float y0, float y1, float sign) {
    const glm::vec3 n(0.0f, 0.0f, sign);
    if (sign > 0.0f) {
        emitQuad(mesh, {x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}, n);
    } else {
        emitQuad(mesh, {x1, y0, z}, {x0, y0, z}, {x0, y1, z}, {x1, y1, z}, n);
    }
}

/// The whole of a box brush: six faces, twenty-four vertices, thirty-six indices. This is
/// [`game/demo/DemoWorld.cpp`](../game/demo/DemoWorld.cpp)'s `unitCube` generalised from a
/// unit at the origin to a pair of corners, which is the one change a blockout wants.
[[nodiscard]] MeshData boxBrush(const Box& b) {
    MeshData mesh;
    emitFaceZ(mesh, b.max.z, b.min.x, b.max.x, b.min.y, b.max.y, 1.0f);
    emitFaceZ(mesh, b.min.z, b.min.x, b.max.x, b.min.y, b.max.y, -1.0f);
    emitFaceX(mesh, b.max.x, b.min.y, b.max.y, b.min.z, b.max.z, 1.0f);
    emitFaceX(mesh, b.min.x, b.min.y, b.max.y, b.min.z, b.max.z, -1.0f);
    emitFaceY(mesh, b.max.y, b.min.x, b.max.x, b.min.z, b.max.z, 1.0f);
    emitFaceY(mesh, b.min.y, b.min.x, b.max.x, b.min.z, b.max.z, -1.0f);
    return mesh;
}

/**
 * @brief The union of two brushes, when one is genuinely wanted: concatenate and rebase.
 *
 * Not a boolean. The second mesh's indices move up by the first's vertex count, which is
 * the same rebase `GltfScene::createMesh` performs one level down when it hands out a
 * range of the shared buffer. Interior faces are left in, because a depth buffer hides
 * them and removing them is what a solver would have been for.
 */
void appendBrush(MeshData& dst, const MeshData& src) {
    const auto base = static_cast<uint32_t>(dst.vertices.size());
    dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());
    for (const uint32_t i : src.indices) dst.indices.push_back(base + i);
}

/**
 * @brief A wall slab with a rectangular opening strictly inside it.
 *
 * The only shape in a blockout a subtraction would genuinely have produced, and it is a
 * grid. Each of the two big faces is a 3x3 of rectangles with the middle cell missing; the
 * four narrow sides are split along the same rows and columns, so no edge of one face ends
 * in the middle of an edge of another; and four reveals line the opening.
 *
 * **Splitting the sides is the whole of "T-junctions are where these break".** Leaving the
 * left side one tall quad while the front face has vertices at y1 and y2 on the shared
 * edge is a T-junction, and a hairline crack under interpolation. Here it costs four extra
 * quads decided by the author rather than a tolerance decided by a clipper.
 *
 * The opening must not touch an edge of the wall. That is not a limitation of the
 * arithmetic; it is a statement about what the shape then is -- see `doorwayWall`.
 */
[[nodiscard]] MeshData windowWall(const Box& wall, float holeMinX, float holeMaxX, float holeMinY, float holeMaxY) {
    const std::array<float, 4> xs = {wall.min.x, holeMinX, holeMaxX, wall.max.x};
    const std::array<float, 4> ys = {wall.min.y, holeMinY, holeMaxY, wall.max.y};
    const float z0 = wall.min.z;
    const float z1 = wall.max.z;

    MeshData mesh;
    for (uint32_t j = 0; j < 3; ++j) {
        for (uint32_t i = 0; i < 3; ++i) {
            if (i == 1 && j == 1) continue; // the opening
            emitFaceZ(mesh, z1, xs[i], xs[i + 1], ys[j], ys[j + 1], 1.0f);
            emitFaceZ(mesh, z0, xs[i], xs[i + 1], ys[j], ys[j + 1], -1.0f);
        }
    }
    for (uint32_t j = 0; j < 3; ++j) {
        emitFaceX(mesh, xs[3], ys[j], ys[j + 1], z0, z1, 1.0f);
        emitFaceX(mesh, xs[0], ys[j], ys[j + 1], z0, z1, -1.0f);
    }
    for (uint32_t i = 0; i < 3; ++i) {
        emitFaceY(mesh, ys[3], xs[i], xs[i + 1], z0, z1, 1.0f);
        emitFaceY(mesh, ys[0], xs[i], xs[i + 1], z0, z1, -1.0f);
    }
    // The reveals, facing into the opening rather than out of the wall.
    emitFaceX(mesh, xs[1], ys[1], ys[2], z0, z1, 1.0f);
    emitFaceX(mesh, xs[2], ys[1], ys[2], z0, z1, -1.0f);
    emitFaceY(mesh, ys[1], xs[1], xs[2], z0, z1, 1.0f);
    emitFaceY(mesh, ys[2], xs[1], xs[2], z0, z1, -1.0f);
    return mesh;
}

/// A wall with a doorway, which is three boxes and no subtraction: two piers and a lintel.
/// This is what a mapper builds, and it is why the opening in `windowWall` has to be
/// interior -- an opening that reaches an edge does not divide a solid, it decomposes it,
/// and the decomposition is smaller than the boolean would have been.
[[nodiscard]] std::array<Box, 3> doorwayWall(const Box& wall, float doorMinX, float doorMaxX, float doorTopY) {
    return {Box{{wall.min.x, wall.min.y, wall.min.z}, {doorMinX, wall.max.y, wall.max.z}},
            Box{{doorMaxX, wall.min.y, wall.min.z}, {wall.max.x, wall.max.y, wall.max.z}},
            Box{{doorMinX, doorTopY, wall.min.z}, {doorMaxX, wall.max.y, wall.max.z}}};
}

// =========================================================================== the checks

/// Exact positions as a key. Every coordinate below is one an author typed, so bit
/// equality is the right comparison -- an epsilon here would hide the very property these
/// tests exist to establish.
struct VertexKey {
    float x, y, z;
    bool operator<(const VertexKey& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

/**
 * @brief Is every directed edge matched by the same number of opposite-directed edges?
 *
 * Closed and consistently wound, which is what "watertight" actually means, stated so that
 * it survives a mesh made of several solids: a single closed surface has every edge
 * matched one-to-one, and appending closed surfaces adds balanced multisets to a balanced
 * multiset. Positions are matched exactly, so a T-junction fails it -- the long edge has no
 * partner and neither short edge has one either.
 */
[[nodiscard]] bool everyEdgeIsMatched(const MeshData& mesh) {
    std::map<VertexKey, uint32_t> ids;
    std::vector<uint32_t> id(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const glm::vec3& p = mesh.vertices[i].position;
        id[i] = ids.emplace(VertexKey{p.x, p.y, p.z}, static_cast<uint32_t>(ids.size())).first->second;
    }

    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const uint32_t a = id[mesh.indices[t]];
        const uint32_t b = id[mesh.indices[t + 1]];
        const uint32_t c = id[mesh.indices[t + 2]];
        ++edges[{a, b}];
        ++edges[{b, c}];
        ++edges[{c, a}];
    }
    for (const auto& [edge, count] : edges) {
        const auto opposite = edges.find({edge.second, edge.first});
        if (opposite == edges.end() || opposite->second != count) return false;
    }
    return true;
}

/// Surface area by the cross product, which is what tells a degenerate face from a real
/// one without any tolerance being consulted.
[[nodiscard]] double surfaceArea(const MeshData& mesh) {
    double total = 0.0;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const glm::vec3& a = mesh.vertices[mesh.indices[t]].position;
        const glm::vec3& b = mesh.vertices[mesh.indices[t + 1]].position;
        const glm::vec3& c = mesh.vertices[mesh.indices[t + 2]].position;
        total += 0.5 * static_cast<double>(glm::length(glm::cross(b - a, c - a)));
    }
    return total;
}

/// Bit equality against a set of authored values. `memcmp` rather than `==` so that the
/// claim being made is unmistakably "the same float", not "close enough".
[[nodiscard]] bool holds(const std::vector<float>& set, float v) {
    for (const float f : set) {
        if (std::memcmp(&f, &v, sizeof(float)) == 0) return true;
    }
    return false;
}

// ============================================================================= the room
//
// Eight by six by three inside, walls a quarter thick, a window in the north wall and a
// doorway in the south. Every number is one an author wrote: there is no derived
// coordinate anywhere in the blockout.

constexpr float kThick = 0.25f;
constexpr float kWide = 8.0f;
constexpr float kDeep = 6.0f;
constexpr float kTall = 3.0f;

/// Floor, ceiling and the two solid walls.
[[nodiscard]] std::vector<Box> shellBrushes() {
    return {
        Box{{0.0f, -kThick, 0.0f}, {kWide, 0.0f, kDeep}},
        Box{{0.0f, kTall, 0.0f}, {kWide, kTall + kThick, kDeep}},
        Box{{-kThick, 0.0f, 0.0f}, {0.0f, kTall, kDeep}},
        Box{{kWide, 0.0f, 0.0f}, {kWide + kThick, kTall, kDeep}},
    };
}

[[nodiscard]] Box northWall() { return {{0.0f, 0.0f, kDeep}, {kWide, kTall, kDeep + kThick}}; }
[[nodiscard]] Box southWall() { return {{0.0f, 0.0f, -kThick}, {kWide, kTall, 0.0f}}; }

/// The whole blockout as one `MeshData`, ready for `Engine::createMesh`.
[[nodiscard]] MeshData roomMesh() {
    MeshData room;
    for (const Box& b : shellBrushes()) appendBrush(room, boxBrush(b));
    appendBrush(room, windowWall(northWall(), 3.0f, 5.0f, 1.0f, 2.0f));
    for (const Box& b : doorwayWall(southWall(), 3.0f, 5.0f, 2.0f)) appendBrush(room, boxBrush(b));
    return room;
}

/**
 * @brief One static box collider per brush.
 *
 * The other half of what a Quake brush was -- a solid is what you see *and* what you walk
 * into -- and it needs no solve either. Note that the window wall collides as a whole
 * slab: you cannot climb through a window at blockout, so its collision is the box and its
 * geometry is the grid. That divergence is a decision the game makes in one line, and it
 * is one a solver handing back a single mesh could not have offered.
 */
[[nodiscard]] std::vector<ColliderDesc> roomColliders() {
    std::vector<Box> boxes = shellBrushes();
    boxes.push_back(northWall());
    for (const Box& b : doorwayWall(southWall(), 3.0f, 5.0f, 2.0f)) boxes.push_back(b);

    std::vector<ColliderDesc> out;
    out.reserve(boxes.size());
    for (const Box& b : boxes) {
        ColliderDesc c;
        c.name = "blockout";
        c.shape = ColliderShape::Box;
        c.motion = ColliderMotion::Static;
        c.halfExtent = b.halfExtent();
        c.transform = glm::translate(glm::mat4(1.0f), b.centre());
        out.push_back(std::move(c));
    }
    return out;
}

[[nodiscard]] glm::vec3 bodyPosition(const PhysicsWorld& world, BodyId body) {
    return glm::vec3(world.bodyTransform(body, 0.0f)[3]);
}

} // namespace

// ------------------------------------------------------------------------ a solid brush

TEST(BrushGeometryTest, ABoxBrushIsTwentyFourVerticesAndNothingElse) {
    const MeshData mesh = boxBrush({{0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 4.0f}});

    EXPECT_EQ(mesh.vertices.size(), 24u);
    EXPECT_EQ(mesh.indices.size(), 36u);
    for (const uint32_t i : mesh.indices) EXPECT_LT(i, mesh.vertices.size());

    // Six faces of a 2 x 1 x 4 box: 2*(2*1) + 2*(2*4) + 2*(1*4) = 28.
    EXPECT_NEAR(surfaceArea(mesh), 28.0, 1e-4);
    EXPECT_TRUE(everyEdgeIsMatched(mesh));

    // Bounds are left at their default, so `createMesh` derives them from the vertices --
    // which for a brush is exact, because the vertices *are* the corners the author typed.
    EXPECT_EQ(mesh.localMin, glm::vec3(0.0f));
    EXPECT_EQ(mesh.localMax, glm::vec3(0.0f));
    glm::vec3 lo(1e9f);
    glm::vec3 hi(-1e9f);
    for (const Vertex& v : mesh.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    EXPECT_EQ(lo, glm::vec3(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(hi, glm::vec3(2.0f, 1.0f, 4.0f));

    // Every normal is a unit axis, every one points out of the solid, and every tangent
    // lies in its face.
    for (const Vertex& v : mesh.vertices) {
        EXPECT_FLOAT_EQ(glm::length(v.normal), 1.0f);
        EXPECT_GT(glm::dot(v.position - glm::vec3(1.0f, 0.5f, 2.0f), v.normal), 0.0f);
        EXPECT_NEAR(glm::dot(glm::vec3(v.tangent), v.normal), 0.0f, 1e-6f);
    }
}

// ----------------------------------------------------------------------- the subtraction

TEST(BrushGeometryTest, AWindowWallIsThirtyTwoRectanglesAndTheOpeningIsReallyAbsent) {
    const MeshData mesh = windowWall(northWall(), 3.0f, 5.0f, 1.0f, 2.0f);

    // 8 grid cells per big face, 3 quads per narrow side, 4 reveals.
    constexpr size_t kQuads = 8 * 2 + 3 * 4 + 4;
    static_assert(kQuads == 32, "the grid is 3x3 less its middle, twice over");
    EXPECT_EQ(mesh.vertices.size(), kQuads * 4);
    EXPECT_EQ(mesh.indices.size(), kQuads * 6);
    for (const uint32_t i : mesh.indices) EXPECT_LT(i, mesh.vertices.size());

    // The hole is absent rather than covered: no triangle of either big face contains the
    // opening's centre.
    const glm::vec2 holeCentre(4.0f, 1.5f);
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const glm::vec3& a = mesh.vertices[mesh.indices[t]].position;
        const glm::vec3& b = mesh.vertices[mesh.indices[t + 1]].position;
        const glm::vec3& c = mesh.vertices[mesh.indices[t + 2]].position;
        if (a.z != b.z || b.z != c.z) continue; // not a face of constant z
        const auto side = [&](const glm::vec3& p, const glm::vec3& q) {
            return (q.x - p.x) * (holeCentre.y - p.y) - (q.y - p.y) * (holeCentre.x - p.x);
        };
        const float s0 = side(a, b);
        const float s1 = side(b, c);
        const float s2 = side(c, a);
        const bool inside = (s0 >= 0.0f && s1 >= 0.0f && s2 >= 0.0f) || (s0 <= 0.0f && s1 <= 0.0f && s2 <= 0.0f);
        EXPECT_FALSE(inside) << "a triangle still covers the opening";
    }

    // Area, against the closed form. Two big faces of 8 x 3 less a 2 x 1 hole, four narrow
    // sides of the slab, four reveals. Nothing here needs a tolerance either.
    const double faces = 2.0 * (kWide * kTall - 2.0 * 1.0);
    const double sides = 2.0 * (kWide * kThick) + 2.0 * (kTall * kThick);
    const double reveals = 2.0 * (2.0 * kThick) + 2.0 * (1.0 * kThick);
    EXPECT_NEAR(surfaceArea(mesh), faces + sides + reveals, 1e-4);
}

TEST(BrushGeometryTest, AWindowWallIsClosedAndEveryEdgeIsMatched) {
    const Box wall = northWall();

    // The property the card sized at L for. Coplanar faces, near-degenerate splits and
    // T-junctions are what a clipper gets wrong, and none of the three can arise when the
    // author states the coordinates. Splitting the four narrow sides along the same rows
    // and columns as the grid is what makes this hold, and it costs four extra quads.
    EXPECT_TRUE(everyEdgeIsMatched(windowWall(wall, 3.0f, 5.0f, 1.0f, 2.0f)));

    // Still closed when the opening is off-centre, tall, or very nearly the whole wall.
    // That is an author's choice rather than a robustness question, which is the
    // difference this row is about.
    EXPECT_TRUE(everyEdgeIsMatched(windowWall(wall, 0.5f, 1.0f, 0.25f, 2.75f)));
    EXPECT_TRUE(everyEdgeIsMatched(windowWall(wall, 0.0625f, 7.9375f, 0.0625f, 2.9375f)));
}

TEST(BrushGeometryTest, EveryCoordinateIsOneTheAuthorWrote) {
    const MeshData mesh = windowWall(northWall(), 3.0f, 5.0f, 1.0f, 2.0f);

    // The eight numbers this wall was described with, and every vertex is made of them --
    // bit for bit, by `memcmp`, with no epsilon consulted anywhere. A plane-clipping solve
    // reaches the same corners through three-plane intersections and can only land on them
    // to within a tolerance somebody has to choose, which is where the classic failures
    // live: two faces that should be coplanar and are not, a split that should be exact
    // and yields a sliver, a corner that appears twice a few ULPs apart.
    const std::vector<float> xs = {0.0f, 3.0f, 5.0f, kWide};
    const std::vector<float> ys = {0.0f, 1.0f, 2.0f, kTall};
    const std::vector<float> zs = {kDeep, kDeep + kThick};
    for (const Vertex& v : mesh.vertices) {
        EXPECT_TRUE(holds(xs, v.position.x)) << "x " << v.position.x << " was computed rather than written";
        EXPECT_TRUE(holds(ys, v.position.y)) << "y " << v.position.y << " was computed rather than written";
        EXPECT_TRUE(holds(zs, v.position.z)) << "z " << v.position.z << " was computed rather than written";
    }
}

TEST(BrushGeometryTest, ADoorwayIsThreeBrushesRatherThanASubtraction) {
    const std::array<Box, 3> pieces = doorwayWall(southWall(), 3.0f, 5.0f, 2.0f);

    MeshData mesh;
    for (const Box& b : pieces) appendBrush(mesh, boxBrush(b));

    EXPECT_EQ(mesh.vertices.size(), 3u * 24u);
    EXPECT_EQ(mesh.indices.size(), 3u * 36u);
    for (const uint32_t i : mesh.indices) EXPECT_LT(i, mesh.vertices.size());

    // Three closed solids appended stays closed, with no welding and no boolean. The piers
    // and the lintel meet at planes whose faces are coincident and back to back, so they
    // are edge-on from anywhere outside the wall and there is nothing for a depth buffer to
    // fight over -- which is the cost of not merging, stated rather than assumed.
    EXPECT_TRUE(everyEdgeIsMatched(mesh));

    // The doorway is a real hole: a point in the opening is outside all three boxes.
    const glm::vec3 through(4.0f, 1.0f, -kThick * 0.5f);
    for (const Box& b : pieces) {
        const bool inside = through.x > b.min.x && through.x < b.max.x && through.y > b.min.y &&
                            through.y < b.max.y && through.z > b.min.z && through.z < b.max.z;
        EXPECT_FALSE(inside);
    }
}

// -------------------------------------------------------------------------- the union

TEST(BrushGeometryTest, UnioningBrushesIsAConcatenationAndAnIndexRebase) {
    const MeshData room = roomMesh();

    // Four shell boxes, a window wall of 32 quads, three doorway pieces: seven boxes and
    // one grid, and the whole blockout is 148 triangles.
    EXPECT_EQ(room.vertices.size(), 7u * 24u + 32u * 4u);
    EXPECT_EQ(room.indices.size(), 7u * 36u + 32u * 6u);
    EXPECT_EQ(room.indices.size() / 3u, 148u);
    for (const uint32_t i : room.indices) EXPECT_LT(i, room.vertices.size());

    // The rebase is the only thing `appendBrush` can get wrong, so it is checked from the
    // far end: the last brush's triangles must index the last brush's vertices and no
    // earlier ones.
    const auto lastBase = static_cast<uint32_t>(room.vertices.size() - 24u);
    for (size_t i = room.indices.size() - 36u; i < room.indices.size(); ++i) {
        EXPECT_GE(room.indices[i], lastBase);
    }

    // Closed, because a union of closed solids is closed and nothing here welds anything.
    // Brushes that abut share their coordinates *exactly* -- the floor's top plane is the
    // literal 0.0f the walls stand on -- which is why matching by position works at all,
    // and is the same property `EveryCoordinateIsOneTheAuthorWrote` states from the front.
    EXPECT_TRUE(everyEdgeIsMatched(room));

    // What follows in a game is the one line the suite cannot reach:
    //     const auto blockout = e.createMesh(std::move(room));
    // and `e.removeModel(blockout)` unloads it (G4, C10). Coarser levels come from
    // `substrate-bake` (C17, D9), offline, which is where geometry production belongs.
    EXPECT_EQ(room.material, 0u);
    EXPECT_EQ(room.transform, glm::mat4(1.0f));
}

TEST(BrushGeometryTest, AdjacentBrushesTextureContinuously) {
    // Two brushes meeting at x = 4, which is the coplanar-face case a solver has to detect
    // and this does not have to: the projection is a function of world position, so the
    // texture runs across the seam whether or not the two brushes know about each other.
    const MeshData left = boxBrush({{0.0f, 0.0f, 0.0f}, {4.0f, kTall, kThick}});
    const MeshData right = boxBrush({{4.0f, 0.0f, 0.0f}, {kWide, kTall, kThick}});

    const auto uvAt = [](const MeshData& mesh, const glm::vec3& p, const glm::vec3& n) {
        for (const Vertex& v : mesh.vertices) {
            if (v.position == p && v.normal == n) return v.uv;
        }
        ADD_FAILURE() << "no vertex at the seam";
        return glm::vec2(0.0f);
    };

    const glm::vec3 front(0.0f, 0.0f, 1.0f);
    EXPECT_EQ(uvAt(left, {4.0f, 0.0f, kThick}, front), uvAt(right, {4.0f, 0.0f, kThick}, front));
    EXPECT_EQ(uvAt(left, {4.0f, kTall, kThick}, front), uvAt(right, {4.0f, kTall, kThick}, front));

    // And the scale is the one the game picked rather than one an engine chose for it.
    EXPECT_EQ(uvAt(left, {4.0f, 0.0f, kThick}, front), glm::vec2(4.0f * kTexelsPerUnit, 0.0f));
}

// ------------------------------------------------------------------------- degenerates

TEST(BrushGeometryTest, ADegenerateBrushIsTheAuthorsProblemAndIsVisiblyZero) {
    // A box with no thickness. A clipper meets this as a set of planes that does not bound
    // a volume and has to decide, with an epsilon, whether the solve failed. Here it is a
    // box whose four side faces have zero area, and the game sees that in the one number
    // that says so rather than in a diagnostic from a solver it has to trust.
    const MeshData flat = boxBrush({{0.0f, 0.0f, 1.0f}, {2.0f, 1.0f, 1.0f}});
    EXPECT_EQ(flat.vertices.size(), 24u);
    EXPECT_NEAR(surfaceArea(flat), 2.0 * (2.0 * 1.0), 1e-6); // the two coincident faces only

    // Inverted corners, max below min. The faces come out wound inward, which the area
    // cannot see -- so a game that takes corners from a tool sorts them, in one line, and
    // the sorted box is the box.
    const Box inverted{{2.0f, 1.0f, 4.0f}, {0.0f, 0.0f, 0.0f}};
    const Box sorted{glm::min(inverted.min, inverted.max), glm::max(inverted.min, inverted.max)};
    EXPECT_NEAR(surfaceArea(boxBrush(sorted)), 28.0, 1e-4);
    EXPECT_TRUE(everyEdgeIsMatched(boxBrush(sorted)));

    // A sliver one micron thick survives exactly, because nothing rounds it. This is the
    // near-degenerate case the card named, and the reason it is uneventful is that no
    // tolerance is ever compared against.
    const MeshData sliver = boxBrush({{0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 1e-6f}});
    EXPECT_TRUE(everyEdgeIsMatched(sliver));
    EXPECT_EQ(sliver.vertices[0].position.z, 1e-6f);

    // An empty mesh is what a game hands `createMesh` when its own generator refused, and
    // `GltfScene::createMesh` refuses it by name -- "%zu vertices and %zu indices is not a
    // mesh" -- returning `kNoModel` rather than allocating a range for nothing.
    const MeshData nothing;
    EXPECT_TRUE(nothing.vertices.empty());
    EXPECT_TRUE(nothing.indices.empty());
}

// --------------------------------------------------------------------------- collision

TEST(BrushGeometryTest, EachBrushIsOneStaticBoxCollider) {
    const std::vector<ColliderDesc> bodies = roomColliders();

    // Eight brushes, eight boxes, and not one of them carries a triangle copy. A solver's
    // single output mesh would have wanted `ColliderShape::Mesh`, which does.
    ASSERT_EQ(bodies.size(), 8u);
    EXPECT_EQ(bodies[0].halfExtent, glm::vec3(kWide * 0.5f, kThick * 0.5f, kDeep * 0.5f));
    for (const ColliderDesc& c : bodies) {
        EXPECT_EQ(c.shape, ColliderShape::Box);
        EXPECT_EQ(c.resolvedShape(), ColliderShape::Box);
        EXPECT_TRUE(c.points.empty()) << "a box collider carries no geometry copy";
        EXPECT_TRUE(c.indices.empty());
    }
}

TEST(BrushGeometryTest, ACrateDroppedInTheBlockoutLandsOnTheFloorAndStaysInside) {
    PhysicsWorld world;
    const PhysicsConfig cfg;
    world.init(cfg, 16);

    for (const ColliderDesc& c : roomColliders()) world.createBody(c);

    ColliderDesc crate;
    crate.name = "crate";
    crate.shape = ColliderShape::Box;
    crate.motion = ColliderMotion::Dynamic;
    crate.halfExtent = glm::vec3(0.25f);
    crate.transform = glm::translate(glm::mat4(1.0f), glm::vec3(kWide * 0.5f, 2.0f, kDeep * 0.5f));
    const BodyId body = world.createBody(crate);
    world.finalize();
    ASSERT_TRUE(body.valid());

    for (int i = 0; i < 180; ++i) world.step(cfg.step);

    // The blockout is a room the moment its brushes are bodies. Nothing solved anything.
    const glm::vec3 at = bodyPosition(world, body);
    EXPECT_NEAR(at.y, 0.25f, 0.05f) << "did not come to rest on the floor brush";
    EXPECT_GT(at.x, 0.0f);
    EXPECT_LT(at.x, kWide);
    EXPECT_GT(at.z, 0.0f);
    EXPECT_LT(at.z, kDeep);

    world.shutdown();
}
