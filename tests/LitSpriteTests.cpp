#include "scene/LitSprite.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

/**
 * @file tests/LitSpriteTests.cpp
 * @brief The lit sprite's arithmetic, which is the half of it a device cannot check (P6).
 *
 * The row's image-level check is a *silhouette*: a lit sprite's value is shaded and cannot
 * be held against its source file, so `scripts/readback.sh`'s tenth case asserts coverage
 * instead. What that case cannot do is say *which* number was wrong when a corner lands in
 * the wrong place -- so the corners, the pivot, the winding, the UVs and the flip are pinned
 * here, computed from the input, exactly as `SpriteTableTests` pins the unlit path's.
 */
namespace {

using scene::QuadDesc;
using scene::quadMesh;

constexpr float kEps = 1e-5f;

/// The four corners in the order `quadMesh` emits them: bottom-left, bottom-right,
/// top-right, top-left, seen from +Z.
enum Corner : size_t { kBottomLeft = 0, kBottomRight = 1, kTopRight = 2, kTopLeft = 3 };

TEST(LitSprite, CentredPivotPutsTheQuadAroundTheOrigin) {
    const scene::MeshData m = quadMesh({.size = {2.0f, 4.0f}, .pivot = {0.5f, 0.5f}});

    ASSERT_EQ(m.vertices.size(), 4u);
    ASSERT_EQ(m.indices.size(), 6u);

    EXPECT_NEAR(m.vertices[kBottomLeft].position.x, -1.0f, kEps);
    EXPECT_NEAR(m.vertices[kBottomLeft].position.y, -2.0f, kEps);
    EXPECT_NEAR(m.vertices[kTopRight].position.x, 1.0f, kEps);
    EXPECT_NEAR(m.vertices[kTopRight].position.y, 2.0f, kEps);
    for (const scene::Vertex& v : m.vertices) EXPECT_NEAR(v.position.z, 0.0f, kEps);
}

TEST(LitSprite, PivotIsMeasuredFromTheTopLeftLikeASprite) {
    // `{0.5, 1.0}` is a character's feet in `SpriteDesc` and has to mean the same here, or
    // a game moving a sprite between the two paths sees it jump by its own height.
    const scene::MeshData feet = quadMesh({.size = {2.0f, 4.0f}, .pivot = {0.5f, 1.0f}});
    EXPECT_NEAR(feet.vertices[kBottomLeft].position.y, 0.0f, kEps);
    EXPECT_NEAR(feet.vertices[kTopLeft].position.y, 4.0f, kEps);

    // The other extreme: a pivot at the top-left corner hangs the whole quad below and to
    // the right of the point, which is what the readback case places.
    const scene::MeshData corner = quadMesh({.size = {64.0f, 48.0f}, .pivot = {0.0f, 0.0f}});
    EXPECT_NEAR(corner.vertices[kTopLeft].position.x, 0.0f, kEps);
    EXPECT_NEAR(corner.vertices[kTopLeft].position.y, 0.0f, kEps);
    EXPECT_NEAR(corner.vertices[kBottomRight].position.x, 64.0f, kEps);
    EXPECT_NEAR(corner.vertices[kBottomRight].position.y, -48.0f, kEps);
}

TEST(LitSprite, UvsAreTheUnitCornerAndVGrowsDownwards) {
    // Not the texel rect: that lives on the material, so a frame change is one
    // `setLitSpriteUv` rather than new geometry. glTF's UV origin is the top-left, so the
    // *top* of the quad is v = 0 -- getting this backwards draws the sprite upside down and
    // changes nothing else, which is exactly what a silhouette check would miss on a
    // vertically symmetric shape.
    const scene::MeshData m = quadMesh({.size = {1.0f, 1.0f}});
    EXPECT_NEAR(m.vertices[kTopLeft].uv.x, 0.0f, kEps);
    EXPECT_NEAR(m.vertices[kTopLeft].uv.y, 0.0f, kEps);
    EXPECT_NEAR(m.vertices[kBottomRight].uv.x, 1.0f, kEps);
    EXPECT_NEAR(m.vertices[kBottomRight].uv.y, 1.0f, kEps);
}

TEST(LitSprite, FlipMovesTheUvsAndNotThePositions) {
    const scene::MeshData plain = quadMesh({.size = {3.0f, 5.0f}, .pivot = {0.25f, 0.75f}});
    const scene::MeshData flipped =
        quadMesh({.size = {3.0f, 5.0f}, .pivot = {0.25f, 0.75f}, .flipX = true, .flipY = true});

    for (size_t i = 0; i < 4; ++i) {
        // The anchor must not move: a character with its pivot at its feet and its sword
        // arm off-centre faces the other way without stepping sideways. That is the
        // convention every 2D tool uses and `SpriteDesc::flipX` states it in the same words.
        EXPECT_NEAR(flipped.vertices[i].position.x, plain.vertices[i].position.x, kEps);
        EXPECT_NEAR(flipped.vertices[i].position.y, plain.vertices[i].position.y, kEps);
        EXPECT_NEAR(flipped.vertices[i].uv.x, 1.0f - plain.vertices[i].uv.x, kEps);
        EXPECT_NEAR(flipped.vertices[i].uv.y, 1.0f - plain.vertices[i].uv.y, kEps);
    }
}

TEST(LitSprite, FacesPositiveZAndIsWoundAnticlockwiseFromIt) {
    const scene::MeshData m = quadMesh({.size = {2.0f, 2.0f}});
    for (const scene::Vertex& v : m.vertices) {
        EXPECT_NEAR(v.normal.z, 1.0f, kEps);
        EXPECT_NEAR(v.tangent.x, 1.0f, kEps);
        EXPECT_NEAR(v.tangent.w, 1.0f, kEps);
    }

    // The winding decides which way the geometric normal points, and the variant turns
    // culling off -- so a reversed winding would not show as a missing sprite, it would show
    // as one lit from behind. Cross the first triangle's edges and check the sign.
    const glm::vec3 a = m.vertices[m.indices[0]].position;
    const glm::vec3 b = m.vertices[m.indices[1]].position;
    const glm::vec3 c = m.vertices[m.indices[2]].position;
    EXPECT_GT(glm::cross(b - a, c - a).z, 0.0f);
}

TEST(LitSprite, BoundsAreTheQuadRatherThanTheWholeWorld) {
    // Stated rather than derived, so this is the only place the two can disagree. A wrong
    // box is invisible until the sprite is culled while on screen.
    const scene::MeshData m = quadMesh({.size = {6.0f, 2.0f}, .pivot = {0.0f, 1.0f}});
    EXPECT_NEAR(m.localMin.x, 0.0f, kEps);
    EXPECT_NEAR(m.localMin.y, 0.0f, kEps);
    EXPECT_NEAR(m.localMax.x, 6.0f, kEps);
    EXPECT_NEAR(m.localMax.y, 2.0f, kEps);
    EXPECT_NEAR(m.localMin.z, 0.0f, kEps);
    EXPECT_NEAR(m.localMax.z, 0.0f, kEps);
}

TEST(LitSprite, CarriesTheMaterialTheMaskAndThePlacementThrough) {
    const glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
    const scene::MeshData m = quadMesh({.material = 7u, .transform = t});
    EXPECT_EQ(m.material, 7u);
    // ALPHA_MODE MASK by default, which is what puts `sprite_lit_shadow.frag` in the shadow
    // pass rather than letting a lit sprite cast a solid rectangle.
    EXPECT_TRUE(m.masked);
    EXPECT_FALSE(m.blended);
    EXPECT_NEAR(m.transform[3].x, 1.0f, kEps);
    EXPECT_NEAR(m.transform[3].z, 3.0f, kEps);
}

TEST(LitSprite, ADegenerateSizeIsStillAWellFormedMesh) {
    // Nothing aborts: a sprite that did not appear is a smaller problem than a game that
    // did not run, which is the rule `SpriteTable::create` already follows.
    const scene::MeshData m = quadMesh({.size = {0.0f, 0.0f}});
    EXPECT_EQ(m.vertices.size(), 4u);
    EXPECT_EQ(m.indices.size(), 6u);
    for (const uint32_t i : m.indices) EXPECT_LT(i, m.vertices.size());
}

} // namespace
