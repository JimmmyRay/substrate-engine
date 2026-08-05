#include "GltfExtras.h"
#include "scene/Collider.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace core;

using namespace scene;

/**
 * @file tests/ColliderTests.cpp
 * @brief The `substrate_collider` schema.
 *
 * The parser takes bytes rather than a path or a parsed asset, which is what lets a
 * five-line document stand in for a scene here -- no device, no file, no fastgltf. The
 * properties worth pinning are the ones a wrong answer would hide rather than break:
 *
 * 1. **Every key is optional.** A collider that names two properties gets two properties
 *    and the engine's defaults for the rest, which is the rule `Config` and
 *    `ParticleEmitter` both follow and the reason a scene survives a schema that grows.
 * 2. **`auto` resolves by motion**, and it resolves in one place. A hull that became a
 *    mesh, or a character that went looking for geometry it does not have, would be a
 *    body that silently fails to exist.
 * 3. **A malformed document fails, an empty one does not.** Bytes that are not glTF are
 *    an error; a glTF file with no collider in it is Sponza.
 */

namespace {

/// A one-node document with `extras.substrate_collider` set to `body`.
std::string document(const std::string& body) {
    return R"({"asset":{"version":"2.0"},"nodes":[{"name":"n","extras":{"substrate_collider":)" + body + "}}]}";
}

std::vector<ColliderDesc> parse(const std::string& json) {
    std::vector<ColliderDesc> out;
    EXPECT_TRUE(testing_extras::parseNodes(json.data(), json.size(), out, parseSceneColliders));
    return out;
}

} // namespace

TEST(Collider, EmptyObjectTakesEveryDefault) {
    const std::vector<ColliderDesc> c = parse(document("{}"));
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0].shape, ColliderShape::Auto);
    EXPECT_EQ(c[0].motion, ColliderMotion::Static);
    EXPECT_EQ(c[0].freedom, ColliderFreedom::All);
    EXPECT_EQ(c[0].node, 0u);
    EXPECT_EQ(c[0].name, "n");
    EXPECT_FLOAT_EQ(c[0].friction, 0.2f);
    EXPECT_FLOAT_EQ(c[0].mass, 0.0f);
    EXPECT_FLOAT_EQ(c[0].gravityFactor, 1.0f);
}

TEST(Collider, NamedKeysAreReadAndTheRestAreNot) {
    const std::vector<ColliderDesc> c =
        parse(document(R"({"shape":"box","motion":"dynamic","halfExtent":[1.0,2.0,3.0],"mass":7.5})"));
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0].shape, ColliderShape::Box);
    EXPECT_EQ(c[0].motion, ColliderMotion::Dynamic);
    EXPECT_FLOAT_EQ(c[0].halfExtent.x, 1.0f);
    EXPECT_FLOAT_EQ(c[0].halfExtent.y, 2.0f);
    EXPECT_FLOAT_EQ(c[0].halfExtent.z, 3.0f);
    EXPECT_FLOAT_EQ(c[0].mass, 7.5f);
    // Untouched by a document that never mentioned it.
    EXPECT_FLOAT_EQ(c[0].restitution, 0.0f);
}

TEST(Collider, EverySpellingRoundTrips) {
    const ColliderShape shapes[] = {ColliderShape::Auto,     ColliderShape::Box,  ColliderShape::Sphere,
                                    ColliderShape::Capsule,  ColliderShape::Cylinder, ColliderShape::Hull,
                                    ColliderShape::Mesh};
    for (ColliderShape s : shapes) {
        const std::string json = document(std::string(R"({"shape":")") + colliderShapeName(s) + R"("})");
        const std::vector<ColliderDesc> c = parse(json);
        ASSERT_EQ(c.size(), 1u);
        EXPECT_EQ(c[0].shape, s) << colliderShapeName(s);
    }

    const ColliderMotion motions[] = {ColliderMotion::Static, ColliderMotion::Kinematic, ColliderMotion::Dynamic,
                                      ColliderMotion::Character};
    for (ColliderMotion m : motions) {
        // Paired with an explicit shape so the mesh-cannot-be-dynamic correction below
        // does not rewrite the one being checked.
        const std::string json = document(std::string(R"({"shape":"box","motion":")") + colliderMotionName(m) + R"("})");
        const std::vector<ColliderDesc> c = parse(json);
        ASSERT_EQ(c.size(), 1u);
        EXPECT_EQ(c[0].motion, m) << colliderMotionName(m);
    }

    const ColliderFreedom freedoms[] = {ColliderFreedom::All, ColliderFreedom::Plane2D};
    for (ColliderFreedom f : freedoms) {
        const std::string json = document(std::string(R"({"freedom":")") + colliderFreedomName(f) + R"("})");
        const std::vector<ColliderDesc> c = parse(json);
        ASSERT_EQ(c.size(), 1u);
        EXPECT_EQ(c[0].freedom, f) << colliderFreedomName(f);
    }
}

TEST(Collider, UnknownSpellingKeepsTheDefault) {
    const std::vector<ColliderDesc> c =
        parse(document(R"({"shape":"dodecahedron","motion":"floaty","freedom":"sideways"})"));
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0].shape, ColliderShape::Auto);
    EXPECT_EQ(c[0].motion, ColliderMotion::Static);
    EXPECT_EQ(c[0].freedom, ColliderFreedom::All);
}

TEST(Collider, AutoResolvesByMotion) {
    ColliderDesc c;
    c.shape = ColliderShape::Auto;

    c.motion = ColliderMotion::Static;
    EXPECT_EQ(c.resolvedShape(), ColliderShape::Mesh);
    EXPECT_TRUE(c.needsGeometry());

    c.motion = ColliderMotion::Dynamic;
    EXPECT_EQ(c.resolvedShape(), ColliderShape::Hull);
    EXPECT_TRUE(c.needsGeometry());

    c.motion = ColliderMotion::Kinematic;
    EXPECT_EQ(c.resolvedShape(), ColliderShape::Hull);

    // The one that would otherwise go looking for a mesh it has no reason to have.
    c.motion = ColliderMotion::Character;
    EXPECT_EQ(c.resolvedShape(), ColliderShape::Capsule);
    EXPECT_FALSE(c.needsGeometry());
}

TEST(Collider, PrimitiveShapesNeedNoGeometry) {
    ColliderDesc c;
    for (ColliderShape s : {ColliderShape::Box, ColliderShape::Sphere, ColliderShape::Capsule,
                            ColliderShape::Cylinder}) {
        c.shape = s;
        EXPECT_FALSE(c.needsGeometry()) << colliderShapeName(s);
    }
}

TEST(Collider, ADynamicTriangleMeshBecomesAHull) {
    // Jolt cannot give a concave mesh an inertia tensor, so the alternative to correcting
    // this at parse time is a body that fails to be created with the file looking fine.
    const std::vector<ColliderDesc> c = parse(document(R"({"shape":"mesh","motion":"dynamic"})"));
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0].shape, ColliderShape::Hull);
    EXPECT_EQ(c[0].motion, ColliderMotion::Dynamic);
}

/// C20's five rows, and the point of the test is the last two: `jumpBufferSteps` and
/// `coyoteSteps` are read as *integer steps* rather than as seconds, so a file cannot
/// author a window that has to be rounded against a clock to be used.
TEST(Collider, TheCharacterMotionRowsAreReadAndTheWindowsAreCountsOfSteps) {
    const std::vector<ColliderDesc> c = parse(document(
        R"({"motion":"character","acceleration":18.0,"deceleration":50.0,"airControl":0.25,)"
        R"("jumpBufferSteps":4,"coyoteSteps":9,"stepHeight":0.75})"));
    ASSERT_EQ(c.size(), 1u);
    EXPECT_FLOAT_EQ(c[0].acceleration, 18.0f);
    EXPECT_FLOAT_EQ(c[0].deceleration, 50.0f);
    EXPECT_FLOAT_EQ(c[0].airControl, 0.25f);
    EXPECT_FLOAT_EQ(c[0].stepHeight, 0.75f);
    EXPECT_EQ(c[0].jumpBufferSteps, 4u);
    EXPECT_EQ(c[0].coyoteSteps, 9u);

    // And a character that names none of them gets the engine's, which is the rule every
    // other row in this schema follows.
    const std::vector<ColliderDesc> d = parse(document(R"({"motion":"character"})"));
    ASSERT_EQ(d.size(), 1u);
    EXPECT_FLOAT_EQ(d[0].acceleration, 10.0f);
    EXPECT_FLOAT_EQ(d[0].deceleration, 40.0f);
    EXPECT_EQ(d[0].jumpBufferSteps, 10u);
    EXPECT_EQ(d[0].coyoteSteps, 6u);
}

TEST(Collider, AngleIsDegreesInTheFileAndRadiansInTheStruct) {
    const std::vector<ColliderDesc> c = parse(document(R"({"maxSlopeAngle":90.0})"));
    ASSERT_EQ(c.size(), 1u);
    EXPECT_NEAR(c[0].maxSlopeAngle, 1.5707963f, 1e-5f);
}

TEST(Collider, NodesWithoutOneAreSkippedAndTheIndexStillCounts) {
    const std::string json =
        R"({"asset":{"version":"2.0"},"nodes":[)"
        R"({"name":"a"},)"
        R"({"name":"b","extras":{"something_else":{}}},)"
        R"({"name":"c","extras":{"substrate_collider":{"shape":"sphere"}}}]})";
    const std::vector<ColliderDesc> c = parse(json);
    ASSERT_EQ(c.size(), 1u);
    // The node index has to be the *document's*, since that is what the scene walk uses
    // to find the transform. Numbering colliders instead would place this one at node 0.
    EXPECT_EQ(c[0].node, 2u);
    EXPECT_EQ(c[0].name, "c");
}

TEST(Collider, ADocumentWithNoColliderIsNotAFailure) {
    std::vector<ColliderDesc> out;
    const std::string json = R"({"asset":{"version":"2.0"},"nodes":[{"name":"n"}]})";
    EXPECT_TRUE(testing_extras::parseNodes(json.data(), json.size(), out, parseSceneColliders));
    EXPECT_TRUE(out.empty());

    // Nor is one with no nodes at all.
    const std::string bare = R"({"asset":{"version":"2.0"}})";
    EXPECT_TRUE(testing_extras::parseNodes(bare.data(), bare.size(), out, parseSceneColliders));
    EXPECT_TRUE(out.empty());
}

TEST(Collider, BytesThatAreNotGltfFail) {
    std::vector<ColliderDesc> out;
    const std::string garbage = "this is not json";
    EXPECT_FALSE(testing_extras::parseNodes(garbage.data(), garbage.size(), out, parseSceneColliders));
    EXPECT_FALSE(testing_extras::parseNodes(nullptr, 0, out, parseSceneColliders));

    const std::string truncated = R"({"asset":)";
    EXPECT_FALSE(testing_extras::parseNodes(truncated.data(), truncated.size(), out, parseSceneColliders));
}

TEST(Collider, GlbIsUnwrapped) {
    // 12-byte header, then a (length, type, payload) chunk whose type is 'JSON'.
    const std::string json = document(R"({"shape":"capsule","radius":0.4,"halfHeight":0.6})");
    std::vector<char> glb(20 + json.size());
    std::memcpy(glb.data(), "glTF", 4);
    const uint32_t version = 2;
    const auto total = static_cast<uint32_t>(glb.size());
    const auto chunkLength = static_cast<uint32_t>(json.size());
    const uint32_t chunkType = 0x4E4F534Au;
    std::memcpy(glb.data() + 4, &version, 4);
    std::memcpy(glb.data() + 8, &total, 4);
    std::memcpy(glb.data() + 12, &chunkLength, 4);
    std::memcpy(glb.data() + 16, &chunkType, 4);
    std::memcpy(glb.data() + 20, json.data(), json.size());

    std::vector<ColliderDesc> out;
    ASSERT_TRUE(testing_extras::parseNodes(glb.data(), glb.size(), out, parseSceneColliders));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].shape, ColliderShape::Capsule);
    EXPECT_FLOAT_EQ(out[0].radius, 0.4f);
    EXPECT_FLOAT_EQ(out[0].halfHeight, 0.6f);

    // A GLB whose first chunk claims more bytes than the file holds is malformed, and
    // reading it would be reading past the buffer.
    const uint32_t lie = chunkLength + 64;
    std::memcpy(glb.data() + 12, &lie, 4);
    out.clear();
    EXPECT_FALSE(testing_extras::parseNodes(glb.data(), glb.size(), out, parseSceneColliders));
}

TEST(Collider, AWrongTypeKeepsTheDefaultRatherThanFailing) {
    // An old file against a new build should degrade to defaults, not refuse to load.
    const std::vector<ColliderDesc> c =
        parse(document(R"({"radius":"large","halfExtent":[1.0,2.0],"friction":null})"));
    ASSERT_EQ(c.size(), 1u);
    EXPECT_FLOAT_EQ(c[0].radius, 0.5f);
    EXPECT_FLOAT_EQ(c[0].friction, 0.2f);
    // Two of three components is a different quantity than the one asked for, so the
    // whole vector keeps its default rather than being half filled.
    EXPECT_FLOAT_EQ(c[0].halfExtent.x, 0.5f);
    EXPECT_FLOAT_EQ(c[0].halfExtent.y, 0.5f);
}

/**
 * The `.collider` node-name convention. The predicate is the whole rule -- what the node
 * walk does with a true answer (synthesise a static mesh collider, and make no placement
 * so it does not draw) needs fastgltf and a real document, and is checked against
 * `arena.glb` rather than here.
 */

TEST(ColliderNode, TheSuffixIsMatchedAtTheEndAndNowhereElse) {
    EXPECT_TRUE(isColliderNode("arena.collider"));
    EXPECT_TRUE(isColliderNode("columns.collider"));

    // A stem is not required. Requiring one would make the empty case disagree with the
    // obvious reading of the rule, and buy nothing.
    EXPECT_TRUE(isColliderNode(".collider"));

    // The suffix is a suffix. A name that merely contains it, or leads with it, is a
    // node someone named badly rather than collision -- and silently colliding it would
    // be a body nobody asked for and a mesh that stopped drawing.
    EXPECT_FALSE(isColliderNode("arena.collider.visual"));
    EXPECT_FALSE(isColliderNode(".collider_lod0"));
    EXPECT_FALSE(isColliderNode("collider"));
    EXPECT_FALSE(isColliderNode("arena"));
    EXPECT_FALSE(isColliderNode(""));

    // Shorter than the suffix must not read off the end of the name.
    EXPECT_FALSE(isColliderNode("."));
    EXPECT_FALSE(isColliderNode("col"));
}

TEST(ColliderNode, TheMatchIsExactRatherThanCaseFolded) {
    // Blender preserves case and so does glTF, so folding here would make the rule
    // disagree with what the author sees in the outliner.
    EXPECT_FALSE(isColliderNode("arena.Collider"));
    EXPECT_FALSE(isColliderNode("arena.COLLIDER"));
}

TEST(ColliderNode, TheOldSpellingIsNotTheConvention) {
    // `.collision` is what Blender's own habits produce and what this engine's first
    // arena was authored with. It is deliberately not the rule: one spelling, so a file
    // that does not collide is a file whose node is visibly named wrong.
    EXPECT_FALSE(isColliderNode("arena.collision"));
}

TEST(ColliderNode, ThePredicateIsUsableAtCompileTime) {
    // `constexpr` because the constant and the predicate are a pair, the way `Cloth.h`'s
    // are -- a second copy of this rule anywhere is the drift the convention exists to
    // prevent.
    static_assert(isColliderNode("floor.collider"));
    static_assert(!isColliderNode("floor"));
    static_assert(kColliderSuffix == ".collider");
    SUCCEED();
}
