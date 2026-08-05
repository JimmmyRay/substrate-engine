#include "scene/Animation.h"
#include "scene/InstanceTable.h"
#include "scene/SceneTypes.h"

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <vector>

using namespace core;

using namespace scene;

/**
 * @file tests/InstanceTableTests.cpp
 * @brief Slot stability, the free list and world bounds (5.1).
 *
 * 4.1 calls slot stability "the load-bearing property": motion vectors, the TLAS, GPU
 * culling and residency all key off a slot index, so a slot that moves under a live
 * object is four bugs at once and none of them look like this file. The generation
 * counter is the other half -- it is what makes a stale handle detectable rather than a
 * silent alias onto whatever object landed in the slot afterwards.
 */

namespace {

constexpr float kEps = 1e-4f;

InstanceDesc unitCube(const glm::mat4& transform = glm::mat4(1.0f)) {
    InstanceDesc d;
    d.localMin = glm::vec3(-1.0f);
    d.localMax = glm::vec3(1.0f);
    d.transform = transform;
    d.firstIndex = 12;
    d.indexCount = 36;
    d.baseVertex = 4;
    d.vertexCount = 8;
    return d;
}

void expectVecNear(const glm::vec3& a, const glm::vec3& b, float eps = kEps) {
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

} // namespace

// ============================================================ create/destroy

TEST(InstanceTable, CreateProducesALiveHandle) {
    InstanceTable table;
    const InstanceId id = table.create(unitCube());

    EXPECT_TRUE(table.valid(id));
    EXPECT_EQ(table.liveCount(), 1u);
    EXPECT_EQ(table.slotCount(), 1u);
    EXPECT_NE(id.index, InstanceId::kInvalid);
    EXPECT_NE(table.flags(id) & kInstanceLive, 0u);
}

TEST(InstanceTable, DestroyInvalidatesTheHandleWithoutShrinkingTheArrays) {
    // No compaction, ever: a swap-and-pop would move a live object's slot, which is
    // precisely what 4.1b rejects `entt::basic_storage` for.
    InstanceTable table;
    const InstanceId a = table.create(unitCube());
    const InstanceId b = table.create(unitCube());

    table.destroy(a);

    EXPECT_FALSE(table.valid(a));
    EXPECT_TRUE(table.valid(b));
    EXPECT_EQ(b.index, 1u) << "b must not have been moved into a's slot";
    EXPECT_EQ(table.liveCount(), 1u);
    EXPECT_EQ(table.slotCount(), 2u);
}

TEST(InstanceTable, DestroyIsIdempotent) {
    InstanceTable table;
    const InstanceId id = table.create(unitCube());

    table.destroy(id);
    table.destroy(id); // must not free the slot twice
    const InstanceId reused = table.create(unitCube());

    EXPECT_EQ(reused.index, id.index);
    EXPECT_EQ(table.liveCount(), 1u);
    EXPECT_EQ(table.slotCount(), 1u) << "a double free would have put the slot in the list twice";
}

TEST(InstanceTable, AFreedSlotIsHandedBackAndTheOldHandleStaysInvalid) {
    InstanceTable table;
    const InstanceId first = table.create(unitCube());
    table.destroy(first);
    const InstanceId second = table.create(unitCube());

    EXPECT_EQ(second.index, first.index) << "the free list must recycle rather than grow";
    EXPECT_NE(second.generation, first.generation);
    EXPECT_TRUE(table.valid(second));
    // The whole point of the generation: `destroy(a); b = create();` leaves `a` invalid
    // even though `b` occupies `a`'s slot.
    EXPECT_FALSE(table.valid(first));
}

TEST(InstanceTable, TheFreeListIsLastInFirstOut) {
    InstanceTable table;
    const InstanceId a = table.create(unitCube());
    const InstanceId b = table.create(unitCube());
    const InstanceId c = table.create(unitCube());
    (void)c;

    table.destroy(a);
    table.destroy(b);

    EXPECT_EQ(table.create(unitCube()).index, b.index);
    EXPECT_EQ(table.create(unitCube()).index, a.index);
    EXPECT_EQ(table.slotCount(), 3u);
}

TEST(InstanceTable, ADeadSlotFailsEveryFrustumTest) {
    // Not a zero box, which is a point at the origin a cull dispatch would happily
    // accept -- an inverted one fails every overlap test there is.
    InstanceTable table;
    const InstanceId id = table.create(unitCube());
    table.destroy(id);

    const GpuInstanceBounds& b = table.slotBounds(0);
    EXPECT_GT(b.worldMin.x, b.worldMax.x);
    EXPECT_GT(b.worldMin.y, b.worldMax.y);
    EXPECT_GT(b.worldMin.z, b.worldMax.z);
    EXPECT_EQ(table.slot(0).meta.z & kInstanceLive, 0u);
}

TEST(InstanceTable, MutatingThroughAStaleHandleDoesNothing) {
    InstanceTable table;
    const InstanceId stale = table.create(unitCube());
    table.destroy(stale);
    const InstanceId live = table.create(unitCube());

    const uint64_t before = table.revision();
    table.setTransform(stale, glm::translate(glm::mat4(1.0f), glm::vec3(100.0f)));
    table.setFlags(stale, kInstanceBlended, 0);

    EXPECT_EQ(table.revision(), before) << "a rejected mutation must not bump the revision either";
    expectVecNear(glm::vec3(table.transform(live)[3]), glm::vec3(0.0f));
}

// =================================================================== bounds

TEST(InstanceTable, WorldBoundsFollowTheTransform) {
    InstanceTable table;
    const InstanceId id = table.create(unitCube(glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f))));

    const GpuInstanceBounds& b = table.slotBounds(id.index);
    expectVecNear(glm::vec3(b.worldMin), glm::vec3(4.0f, -1.0f, -1.0f));
    expectVecNear(glm::vec3(b.worldMax), glm::vec3(6.0f, 1.0f, 1.0f));
}

TEST(InstanceTable, RotationIsHandledByTransformingAllEightCorners) {
    // Transforming min and max alone is wrong the moment the matrix rotates: a unit cube
    // turned 45 degrees about Y has a world box sqrt(2) wide in x and z, and the naive
    // two-corner version produces the original box back.
    const glm::mat4 m = glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    InstanceTable table;
    const InstanceId id = table.create(unitCube(m));

    const GpuInstanceBounds& b = table.slotBounds(id.index);
    const float diag = std::sqrt(2.0f);
    expectVecNear(glm::vec3(b.worldMin), glm::vec3(-diag, -1.0f, -diag), 1e-3f);
    expectVecNear(glm::vec3(b.worldMax), glm::vec3(diag, 1.0f, diag), 1e-3f);
}

TEST(InstanceTable, BoundingSphereRadiusIsPackedIntoTheMinPadding) {
    InstanceTable table;
    const InstanceId id = table.create(unitCube());

    // Half the diagonal of a 2x2x2 box.
    EXPECT_NEAR(table.slotBounds(id.index).worldMin.w, std::sqrt(3.0f), 1e-4f);
}

TEST(InstanceTable, SetTransformRefreshesBoundsAndTheNormalMatrix) {
    // The reason setTransform is a call and not a mutable reference: a caller writing
    // `table.transform(id) = m` would leave both of these stale.
    InstanceTable table;
    const InstanceId id = table.create(unitCube());

    const glm::mat4 m = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, 0.0f)), glm::vec3(2.0f));
    table.setTransform(id, m);

    const GpuInstanceBounds& b = table.slotBounds(id.index);
    expectVecNear(glm::vec3(b.worldMin), glm::vec3(-2.0f, 1.0f, -2.0f));
    expectVecNear(glm::vec3(b.worldMax), glm::vec3(2.0f, 5.0f, 2.0f));

    // inverse(transpose(mat3)) of a uniform 2x scale is a uniform 0.5 scale.
    const GpuInstance& g = table.slot(id.index);
    EXPECT_NEAR(g.normal0.x, 0.5f, kEps);
    EXPECT_NEAR(g.normal1.y, 0.5f, kEps);
    EXPECT_NEAR(g.normal2.z, 0.5f, kEps);
}

// ===================================================================== flags

// Unlike `blended`, nothing counts masked instances -- the shadow pass partitions its
// command list on the flag rather than sizing anything from a total. So this checks the
// bit alone, and specifically that it is independent of `blended`: a primitive is opaque,
// masked or blended, and the two bits must never be confused for one another because they
// send a draw to entirely different passes.
TEST(InstanceTable, MaskedInstancesCarryTheirOwnFlagIndependentOfBlended) {
    InstanceTable table;
    InstanceDesc masked = unitCube();
    masked.masked = true;

    const InstanceId a = table.create(masked);
    const InstanceId plain = table.create(unitCube());

    EXPECT_NE(table.flags(a) & kInstanceMasked, 0u);
    EXPECT_EQ(table.flags(a) & kInstanceBlended, 0u);
    EXPECT_EQ(table.flags(plain) & kInstanceMasked, 0u);

    // A masked instance is not blended, so it must not have been counted as one -- the
    // forward pass sizes its sort off that number.
    EXPECT_EQ(table.blendedCount(), 0u);

    InstanceDesc both = unitCube();
    both.masked = true;
    both.blended = true;
    const InstanceId b = table.create(both);
    EXPECT_NE(table.flags(b) & kInstanceMasked, 0u);
    EXPECT_NE(table.flags(b) & kInstanceBlended, 0u);
}

TEST(InstanceTable, BlendedInstancesAreCountedSoTheForwardPassCanSizeItsSort) {
    InstanceTable table;
    InstanceDesc blended = unitCube();
    blended.blended = true;

    const InstanceId a = table.create(blended);
    table.create(unitCube());
    EXPECT_EQ(table.blendedCount(), 1u);
    EXPECT_NE(table.flags(a) & kInstanceBlended, 0u);

    table.setFlags(a, 0, kInstanceBlended);
    EXPECT_EQ(table.blendedCount(), 0u);

    table.setFlags(a, kInstanceBlended, 0);
    EXPECT_EQ(table.blendedCount(), 1u);

    table.destroy(a);
    EXPECT_EQ(table.blendedCount(), 0u);
}

TEST(InstanceTable, SetFlagsRefusesToTouchTheLiveBit) {
    // Clearing it would leave a slot every consumer skips but the free list has never
    // heard of, which is a leak wearing a deletion's clothes.
    InstanceTable table;
    const InstanceId id = table.create(unitCube());

    table.setFlags(id, 0, kInstanceLive);

    EXPECT_TRUE(table.valid(id));
    EXPECT_EQ(table.liveCount(), 1u);
}

TEST(InstanceTable, ASkinnedInstanceIsDynamicWithoutBeingAskedTwice) {
    // Its vertices are rebuilt every frame, so the tiers 3.4 and 3.9 select on apply to
    // it whether or not its node transform ever changes.
    InstanceTable table;
    InstanceDesc d = unitCube();
    d.skin = 2;
    d.character = 2;
    d.skinOffset = 64;

    const InstanceId id = table.create(d);

    EXPECT_NE(table.flags(id) & kInstanceSkinned, 0u);
    EXPECT_NE(table.flags(id) & kInstanceDeformed, 0u);
    EXPECT_NE(table.flags(id) & kInstanceDynamic, 0u);
    EXPECT_EQ(table.characterOf(id.index), 2u);
    EXPECT_EQ(table.drawRanges()[id.index].skinOffset, 64u);
}

TEST(InstanceTable, RigidInstancesCarryNoSkin) {
    InstanceTable table;
    const InstanceId id = table.create(unitCube());

    EXPECT_EQ(table.characterOf(id.index), 0xFFFFFFFFu);
    EXPECT_EQ(table.flags(id) & kInstanceDeformed, 0u);
    EXPECT_EQ(table.flags(id) & kInstanceDynamic, 0u);
}

TEST(InstanceTable, AMorphedInstanceIsDeformedWithoutBeingSkinned) {
    // A face rig with no skeleton takes the same dispatch and the same output buffer,
    // which is the whole reason `kInstanceDeformed` is a mask over two independent
    // causes rather than a third flag.
    InstanceTable table;
    InstanceDesc d = unitCube();
    d.morphTargets = 3;
    d.morphOffset = 128;
    d.morphWeightOffset = 5;
    d.character = 0;

    const InstanceId id = table.create(d);

    EXPECT_NE(table.flags(id) & kInstanceMorphed, 0u);
    EXPECT_EQ(table.flags(id) & kInstanceSkinned, 0u);
    EXPECT_NE(table.flags(id) & kInstanceDeformed, 0u);
    EXPECT_NE(table.flags(id) & kInstanceDynamic, 0u);
    EXPECT_EQ(table.drawRanges()[id.index].morphTargets, 3u);
    EXPECT_EQ(table.drawRanges()[id.index].morphOffset, 128u);
    EXPECT_EQ(table.drawRanges()[id.index].morphWeightOffset, 5u);
}

TEST(AccelTier, AnInstanceThatMovesWithoutDeformingGetsItsOwnTlasInstance) {
    // The bug this exists to keep fixed: the acceleration structure split its tiers on
    // `kInstanceDeformed` alone, so a crate a solver pushes -- dynamic, but rigid -- was
    // welded into the static BLAS with its load-time transform baked in. It drew in its
    // new place and traced in its old one, which reads as a shadow that stays behind
    // after the thing casting it has been knocked over.
    EXPECT_EQ(accelTier(kInstanceLive | kInstanceDynamic, true), AccelTier::Rigid);
    EXPECT_EQ(accelTier(kInstanceLive | kInstanceDynamic, false), AccelTier::Rigid);
}

TEST(AccelTier, GeometryThatNeverMovesStaysInTheBakedTier) {
    // What the static BLAS is *for*: one structure over a whole flattened hierarchy, with
    // each geometry's transform baked at build time and never rebuilt.
    EXPECT_EQ(accelTier(kInstanceLive, true), AccelTier::Static);
    EXPECT_EQ(accelTier(kInstanceLive | kInstanceMasked | kInstanceBlended, true), AccelTier::Static);
}

TEST(MovedSinceBake, AnInstanceLeftWhereTheStructureBakedItIsNotStale) {
    const glm::mat4 baked = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 1.0f, -2.0f));
    EXPECT_FALSE(movedSinceBake(baked, baked));

    // The tolerance earns its place here: a node writes its instance through a
    // compose(decompose(m)) round trip, and an exact comparison would call every scene in
    // the engine stale on the first sweep and rebuild its acceleration structure for it.
    glm::mat4 nudged = baked;
    nudged[3].x += 1e-6f;
    nudged[3].y -= 2e-6f;
    EXPECT_FALSE(movedSinceBake(baked, nudged));
}

TEST(MovedSinceBake, AnInstanceASceneNodeMovedAfterTheBuildIsStale) {
    // The brazier case: the instance was created at the identity, the acceleration
    // structure baked that, and the node it was attached to gave it its real place on the
    // first sweep -- leaving a traced copy at the origin with nothing drawn there.
    const glm::mat4 baked(1.0f);
    EXPECT_TRUE(movedSinceBake(baked, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.01f))));
    // A rotation with no translation counts too: the basis is baked into the BLAS as well.
    EXPECT_TRUE(movedSinceBake(baked, glm::rotate(glm::mat4(1.0f), 0.05f, glm::vec3(0.0f, 1.0f, 0.0f))));
}

TEST(AccelTier, DeformedGeometryOutranksMerelyMoving) {
    // A skinned instance is dynamic too (see ASkinnedInstanceIsDynamicWithoutBeingAskedTwice),
    // so the two bits are not exclusive and the deformed test has to come first -- a
    // character sorted into the rigid tier would trace its bind pose.
    EXPECT_EQ(accelTier(kInstanceLive | kInstanceSkinned | kInstanceDynamic, true), AccelTier::Deformed);
    EXPECT_EQ(accelTier(kInstanceLive | kInstanceMorphed | kInstanceDynamic, true), AccelTier::Deformed);
    EXPECT_EQ(accelTier(kInstanceLive | kInstanceCloth | kInstanceDynamic, true), AccelTier::Deformed);
}

TEST(AccelTier, DeformedGeometryWithNowhereToReadItsVerticesStaysStatic) {
    // Pinned because rigid is the tempting answer and is measurably worse. See the note on
    // `accelTier`: the two are equivalent for this case, it only ever arises as a
    // load-time transient, and sending it through the rigid tier moved the golden `skin`
    // frame. The deformed bit is what must not reach `Rigid`, whatever else is set.
    EXPECT_EQ(accelTier(kInstanceLive | kInstanceSkinned | kInstanceDynamic, false), AccelTier::Static);
    EXPECT_EQ(accelTier(kInstanceLive | kInstanceCloth | kInstanceDynamic, false), AccelTier::Static);
}

TEST(InstanceTable, SetCharacterRepointsAnInstanceAtAnotherPose) {
    // What a game calls when it places a second copy of one skinned mesh: same
    // primitive, same influences, same skin, a different pose deforming it.
    InstanceTable table;
    InstanceDesc d = unitCube();
    d.skin = 0;
    d.character = 0;
    d.skinOffset = 0;

    const InstanceId id = table.create(d);
    const uint64_t before = table.revision();

    table.setCharacter(id, 4);
    EXPECT_EQ(table.characterOf(id.index), 4u);
    EXPECT_GT(table.revision(), before);

    const uint64_t after = table.revision();
    table.setCharacter(id, 4);
    EXPECT_EQ(table.revision(), after) << "setting the character it already has is not a mutation";
}

// ================================================================= revision

TEST(InstanceTable, EveryMutationBumpsTheRevisionAndNoQueryDoes) {
    // The renderer compares this against what each frame-in-flight buffer last saw, so
    // a static scene uploads once rather than every frame.
    InstanceTable table;
    const uint64_t start = table.revision();

    const InstanceId id = table.create(unitCube());
    const uint64_t afterCreate = table.revision();
    EXPECT_GT(afterCreate, start);

    table.setTransform(id, glm::translate(glm::mat4(1.0f), glm::vec3(1.0f)));
    const uint64_t afterMove = table.revision();
    EXPECT_GT(afterMove, afterCreate);

    table.setFlags(id, kInstanceVisible, 0);
    EXPECT_GT(table.revision(), afterMove);

    const uint64_t settled = table.revision();
    (void)table.transform(id);
    (void)table.flags(id);
    (void)table.slotBounds(id.index);
    EXPECT_EQ(table.revision(), settled);

    // A no-op setFlags is not a mutation either.
    table.setFlags(id, kInstanceVisible, 0);
    EXPECT_EQ(table.revision(), settled);
}

// ================================================================== history

TEST(InstanceTable, ANewInstanceHasNoVelocity) {
    // Seeding the history with the instance's own transform is what makes "it did not
    // move" the default, so an object does not smear on the frame it appears.
    InstanceTable table;
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(9.0f, 0.0f, 0.0f));
    const InstanceId id = table.create(unitCube(m));

    EXPECT_EQ(table.previousTransform(id), table.transform(id));
}

TEST(InstanceTable, EndFrameRollsTheCurrentTransformIntoTheHistory) {
    InstanceTable table;
    const InstanceId id = table.create(unitCube());

    const glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 4.0f, 0.0f));
    table.setTransform(id, moved);

    // Before the roll, history is still the creation transform -- which is what makes
    // the pair a velocity rather than two copies of the same thing.
    EXPECT_NE(table.previousTransform(id), table.transform(id));

    table.endFrame();
    EXPECT_EQ(table.previousTransform(id), moved);
}

// =========================================== motion history and dynamic count (3.4)

TEST(InstanceTable, DynamicCountTracksTheConjunctionOfTwoBits) {
    InstanceTable table;

    InstanceDesc still = unitCube();
    InstanceDesc moving = unitCube();
    moving.dynamic = true;
    InstanceDesc movingGlass = unitCube();
    movingGlass.dynamic = true;
    movingGlass.blended = true;

    table.create(still);
    const InstanceId m = table.create(moving);
    const InstanceId glass = table.create(movingGlass);

    // The velocity pass draws opaque geometry, so a blended dynamic instance is not one
    // of its draws -- the forward pass owns that surface and writes no G-buffer depth
    // for it to test against.
    EXPECT_EQ(table.dynamicCount(), 1u);

    // Clearing BLENDED on the glass adds it without DYNAMIC having moved, which is the
    // case a counter watching either bit alone gets wrong.
    table.setFlags(glass, 0, kInstanceBlended);
    EXPECT_EQ(table.dynamicCount(), 2u);

    table.destroy(m);
    EXPECT_EQ(table.dynamicCount(), 1u);
}

TEST(InstanceTable, TheRollIsNotAMutation) {
    InstanceTable table;
    const glm::mat4 a = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const InstanceId id = table.create(unitCube(a));

    // The order the renderer uses is roll-then-simulate, which
    // EndFrameRollsTheCurrentTransformIntoTheHistory already pins. What is asserted here
    // is the half that is invisible in the transforms: bumping the revision would make
    // the renderer re-upload the whole shading array every frame of a scene where
    // nothing moved, since the roll runs unconditionally.
    table.endFrame();
    const uint64_t before = table.revision();
    table.setTransform(id, glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f)));
    EXPECT_GT(table.revision(), before);

    const uint64_t moved = table.revision();
    table.endFrame();
    EXPECT_EQ(table.revision(), moved);
}

TEST(InstanceTable, TheHistoryArrayUploadsAsOneFlatCopy) {
    InstanceTable table;
    InstanceId third{};
    for (int i = 0; i < 4; ++i) {
        const InstanceId id = table.create(unitCube());
        if (i == 2) third = id;
    }
    EXPECT_EQ(table.previousBytes(), 4u * sizeof(glm::mat4));
    ASSERT_NE(table.previousData(), nullptr);
    // Contiguous and slot-indexed, the same property the shading array is asserted for
    // above -- which is what lets the renderer memcpy it in one range.
    EXPECT_EQ(table.previousData()[2], table.previousTransform(third));
}

// ============================================================ bulk and reset

TEST(InstanceTable, ArraysStayFlatAndUploadableAtTheirStatedSizes) {
    InstanceTable table;
    table.reserve(8);
    for (int i = 0; i < 5; ++i) table.create(unitCube());

    EXPECT_EQ(table.slotCount(), 5u);
    EXPECT_EQ(table.shadingBytes(), 5u * sizeof(GpuInstance));
    EXPECT_EQ(table.boundsBytes(), 5u * sizeof(GpuInstanceBounds));
    ASSERT_NE(table.shadingData(), nullptr);
    ASSERT_NE(table.boundsData(), nullptr);

    // Contiguous, which is the property that lets each array upload in one copy.
    EXPECT_EQ(&table.shadingData()[4], &table.slot(4));
    EXPECT_EQ(&table.boundsData()[4], &table.slotBounds(4));
}

TEST(InstanceTable, DrawRangesComeStraightFromTheDescriptor) {
    InstanceTable table;
    const InstanceId id = table.create(unitCube());

    const InstanceTable::DrawRange& r = table.drawRanges()[id.index];
    EXPECT_EQ(r.firstIndex, 12u);
    EXPECT_EQ(r.indexCount, 36u);
    EXPECT_EQ(r.baseVertex, 4u);
    EXPECT_EQ(r.vertexCount, 8u);
}

TEST(InstanceTable, ClearEmptiesEverythingAndBumpsTheRevision) {
    InstanceTable table;
    InstanceDesc blended = unitCube();
    blended.blended = true;
    table.create(blended);
    table.create(unitCube());

    const uint64_t before = table.revision();
    table.clear();

    EXPECT_EQ(table.slotCount(), 0u);
    EXPECT_EQ(table.liveCount(), 0u);
    EXPECT_EQ(table.blendedCount(), 0u);
    EXPECT_EQ(table.shadingBytes(), 0u);
    EXPECT_GT(table.revision(), before);
}

// ================================================== the handle itself

namespace {
struct TestTagA;
struct TestTagB;
} // namespace

TEST(HandleType, AZeroedHandleIsInvalid) {
    // The property the design turns on. Handles get memset, copied out of freshly
    // resized vectors and default-constructed into aggregates; under an index sentinel
    // every one of those reads as a live handle to slot 0, which is the first slot every
    // subsystem hands out.
    Handle<TestTagA> zeroed{};
    EXPECT_FALSE(zeroed.valid());

    Handle<TestTagA> slotZero{0u, 0u};
    EXPECT_FALSE(slotZero.valid()) << "generation 0 is reserved for 'never issued'";

    Handle<TestTagA> issued{0u, 1u};
    EXPECT_TRUE(issued.valid());
}

TEST(HandleType, DefaultIndexIsTheSentinelRatherThanSlotZero) {
    const Handle<TestTagA> h{};
    EXPECT_EQ(h.index, Handle<TestTagA>::kInvalid);
}

TEST(HandleType, TwoTagsAreUnrelatedTypes) {
    // The compile error is the feature, so what can be asserted here is the negative:
    // these are distinct types, which is what makes passing one where the other belongs
    // fail to build rather than read a different subsystem's array.
    static_assert(!std::is_same_v<Handle<TestTagA>, Handle<TestTagB>>);
    static_assert(!std::is_convertible_v<Handle<TestTagA>, Handle<TestTagB>>);
    SUCCEED();
}

TEST(InstanceTableHandles, AFreshlyCreatedIdIsValidAndADefaultOneIsNot) {
    InstanceTable table;
    const InstanceId fresh = table.create({});
    EXPECT_TRUE(fresh.valid()) << "generation must start past zero, or every new id is invalid";
    EXPECT_TRUE(table.valid(fresh));

    const InstanceId none{};
    EXPECT_FALSE(none.valid());
    EXPECT_FALSE(table.valid(none));
}

TEST(InstanceTableHandles, ADestroyedIdStaysIssuedButStopsBeingLive) {
    // The two questions Handle::valid() and InstanceTable::valid() answer are different,
    // and this is the case that separates them.
    InstanceTable table;
    const InstanceId id = table.create({});
    table.destroy(id);

    EXPECT_TRUE(id.valid()) << "it was issued; that does not stop being true";
    EXPECT_FALSE(table.valid(id)) << "but it no longer names a live object";
}

// ======================= where a morphed instance's data actually is
//
// A mesh made in code and one loaded from a file end up in the *same* delta array and the
// *same* flat weight buffer, addressed by the same two expressions. These check that the
// second producer lands where the first left off and that neither can read the other's
// run -- the shape of defect G12 found in `PhysicsWorld::snapshot`.
//
// **What is deliberately not proved here.** The weighted sum itself is `skinning.comp`,
// and running it needs a device the unit suite does not have. `morphedVertex` below is a
// transcription of that loop, so these cases pin the *addressing* and the data model; the
// arithmetic is checked by the demo's banner rendering and by a validation-layer run, and
// there is no hosted substitute for that.

namespace {

/// The four numbers the deformation dispatch is handed for one instance, computed exactly
/// as `Renderer::updateInstances` computes them: the primitive's run in the delta array,
/// and the placement's run in the animator's flat weight buffer offset by its character's.
struct MorphAddress {
    uint32_t morphOffset = 0;
    uint32_t morphTargets = 0;
    uint32_t vertexCount = 0;
    uint32_t weightBase = 0;
};

MorphAddress addressOf(const InstanceTable& table, const scene::SceneAnimator& anim, InstanceId id) {
    const InstanceTable::DrawRange& r = table.drawRanges()[id.index];
    const uint32_t character = table.characterOf(id.index);
    const bool posed = character < anim.characterCount();
    return {r.morphOffset, r.morphTargets, r.vertexCount,
            (posed ? anim.weightOffset(character) : 0u) + r.morphWeightOffset};
}

/// One vertex through the morph half of `skinning.comp`, transcribed. Target-major, so
/// target `t`'s displacement of vertex `v` is at `morphOffset + t * vertexCount + v`;
/// getting that expression wrong is a mesh that reads the next target's first rows, which
/// is a wrong shape rather than a crash.
glm::vec3 morphedPosition(const std::vector<scene::Vertex>& source, const std::vector<scene::MorphDelta>& deltas,
                          const std::vector<float>& weights, const InstanceTable::DrawRange& range,
                          const MorphAddress& at, uint32_t v) {
    glm::vec3 position = source[range.baseVertex + v].position;
    for (uint32_t t = 0; t < at.morphTargets; ++t) {
        const float w = weights[at.weightBase + t];
        if (w == 0.0f) continue;
        position += w * deltas[at.morphOffset + t * at.vertexCount + v].position;
    }
    return position;
}

/// The whole weight region, assembled the way the renderer assembles it: every character's
/// run memcpy'd to its own base.
std::vector<float> flatWeights(const scene::SceneAnimator& anim) {
    std::vector<float> out(anim.totalWeights(), 0.0f);
    for (uint32_t c = 0; c < anim.characterCount(); ++c) {
        const std::vector<float>& w = anim.morphWeights(c);
        std::copy(w.begin(), w.end(), out.begin() + anim.weightOffset(c));
    }
    return out;
}

constexpr uint32_t kMorphVerts = 3;

/**
 * @brief One scene holding a file-authored morphed primitive and a code-made one.
 *
 * Both are three vertices at the same three positions and carry the same two targets, so
 * "the two producers agree" is a statement about the *addressing* and nothing else. The
 * code-made half is appended behind the file's, which is what `GltfScene::createMesh`
 * does to `morphData` and the reason its offset is 6 rather than 0.
 */
struct MorphScene {
    std::vector<scene::Vertex> vertices;
    std::vector<scene::MorphDelta> deltas;
    scene::SceneAnimator animator;
    InstanceTable table;
    InstanceId fromFile;
    InstanceId fromCode;

    MorphScene() {
        for (uint32_t half = 0; half < 2; ++half) {
            for (uint32_t v = 0; v < kMorphVerts; ++v) {
                scene::Vertex vertex{};
                vertex.position = glm::vec3(static_cast<float>(v), 0.0f, 0.0f);
                vertices.push_back(vertex);
            }
            // Target 0 lifts along +Y by the vertex index; target 1 pushes along +Z by one.
            // Two targets that move different axes is what makes a swapped lane visible.
            for (uint32_t v = 0; v < kMorphVerts; ++v) {
                scene::MorphDelta d{};
                d.position = glm::vec3(0.0f, static_cast<float>(v + 1), 0.0f);
                deltas.push_back(d);
            }
            for (uint32_t v = 0; v < kMorphVerts; ++v) {
                scene::MorphDelta d{};
                d.position = glm::vec3(0.0f, 0.0f, 1.0f);
                deltas.push_back(d);
            }
        }

        // The rig declares the file's two weights and nothing else, which is exactly the
        // state that leaves a code-made mesh with nowhere to put its own.
        scene::AnimationRig rig;
        rig.bind.nodes.resize(1);
        rig.bind.nodes[0].firstWeight = 0;
        rig.bind.nodes[0].weightCount = 2;
        rig.bind.weights.assign(2, 0.0f);
        animator.init(std::move(rig));

        const scene::AnimatorId code = animator.createMorphed(2);

        InstanceDesc file;
        file.vertexCount = kMorphVerts;
        file.indexCount = 3;
        file.baseVertex = 0;
        file.morphOffset = 0;
        file.morphTargets = 2;
        file.morphWeightOffset = 0; ///< the node's `firstWeight`
        file.character = 0;
        fromFile = table.create(file);

        InstanceDesc made = file;
        made.baseVertex = kMorphVerts;
        made.morphOffset = 2 * kMorphVerts; ///< appended behind the file's targets
        made.character = code.index;
        fromCode = table.create(made);
    }

    /// Drive the file's weights the only way a file can: through the rig's own numbering.
    void setFileWeights(float a, float b) {
        // Character 0's pose weights are the rig's; writing them through the animator's
        // public surface is what a `weights` channel does, so the test reaches the same
        // block by the same offset rather than by a back door.
        fileWeights = {a, b};
    }

    std::vector<float> weightBuffer() const {
        std::vector<float> out = flatWeights(animator);
        std::copy(fileWeights.begin(), fileWeights.end(), out.begin() + animator.weightOffset(0));
        return out;
    }

    glm::vec3 filePosition(uint32_t v) const {
        return morphedPosition(vertices, deltas, weightBuffer(), table.drawRanges()[fromFile.index],
                               addressOf(table, animator, fromFile), v);
    }
    glm::vec3 codePosition(uint32_t v) const {
        return morphedPosition(vertices, deltas, weightBuffer(), table.drawRanges()[fromCode.index],
                               addressOf(table, animator, fromCode), v);
    }

    std::vector<float> fileWeights{0.0f, 0.0f};
};

} // namespace

TEST(MorphAddressing, TheCodeMadeRunSitsBehindTheFilesAndTheyDoNotOverlap) {
    const MorphScene s;
    const MorphAddress file = addressOf(s.table, s.animator, s.fromFile);
    const MorphAddress code = addressOf(s.table, s.animator, s.fromCode);

    EXPECT_EQ(file.morphOffset, 0u);
    EXPECT_EQ(code.morphOffset, file.morphOffset + file.morphTargets * file.vertexCount);
    EXPECT_EQ(s.deltas.size(), code.morphOffset + code.morphTargets * code.vertexCount);

    EXPECT_EQ(file.weightBase, 0u);
    EXPECT_EQ(code.weightBase, 2u) << "the code-made block starts where the file's ends";
    EXPECT_EQ(s.animator.totalWeights(), 4u);
}

TEST(MorphAddressing, ZeroWeightIsTheBaseMesh) {
    const MorphScene s;
    for (uint32_t v = 0; v < kMorphVerts; ++v) {
        EXPECT_EQ(s.codePosition(v), glm::vec3(static_cast<float>(v), 0.0f, 0.0f));
        EXPECT_EQ(s.filePosition(v), s.codePosition(v));
    }
}

TEST(MorphAddressing, FullWeightIsTheTarget) {
    MorphScene s;
    s.animator.setMorphWeight(s.animator.characterAt(s.table.characterOf(s.fromCode.index)), 0, 1.0f);
    for (uint32_t v = 0; v < kMorphVerts; ++v) {
        EXPECT_EQ(s.codePosition(v), glm::vec3(static_cast<float>(v), static_cast<float>(v + 1), 0.0f));
    }
}

TEST(MorphAddressing, AWeightBetweenThemInterpolatesLinearly) {
    MorphScene s;
    const scene::AnimatorId code = s.animator.characterAt(s.table.characterOf(s.fromCode.index));
    s.animator.setMorphWeight(code, 0, 0.25f);
    for (uint32_t v = 0; v < kMorphVerts; ++v) {
        EXPECT_FLOAT_EQ(s.codePosition(v).y, 0.25f * static_cast<float>(v + 1));
    }
}

TEST(MorphAddressing, TwoTargetsSum) {
    MorphScene s;
    const scene::AnimatorId code = s.animator.characterAt(s.table.characterOf(s.fromCode.index));
    s.animator.setMorphWeight(code, 0, 0.5f);
    s.animator.setMorphWeight(code, 1, 2.0f);
    for (uint32_t v = 0; v < kMorphVerts; ++v) {
        const glm::vec3 p = s.codePosition(v);
        EXPECT_FLOAT_EQ(p.y, 0.5f * static_cast<float>(v + 1)) << "target 0 alone drives Y";
        EXPECT_FLOAT_EQ(p.z, 2.0f) << "target 1 alone drives Z, and a weight past 1 is legal";
    }
}

TEST(MorphAddressing, TheTwoProducersAgreeOnTheSameWeights) {
    MorphScene s;
    const scene::AnimatorId code = s.animator.characterAt(s.table.characterOf(s.fromCode.index));
    s.animator.setMorphWeight(code, 0, 0.6f);
    s.animator.setMorphWeight(code, 1, -0.3f);
    s.setFileWeights(0.6f, -0.3f);

    for (uint32_t v = 0; v < kMorphVerts; ++v) {
        EXPECT_EQ(s.filePosition(v), s.codePosition(v))
            << "same geometry, same targets, same weights, two producers -- vertex " << v;
    }
}

TEST(MorphAddressing, AndDisagreeTheMomentTheirWeightsDo) {
    // The negative control the case above needs: if the two instances read the same block,
    // every comparison in this file passes for an implementation that has one weight run
    // and pretends it has two.
    MorphScene s;
    s.setFileWeights(1.0f, 0.0f);
    for (uint32_t v = 0; v < kMorphVerts; ++v) {
        EXPECT_NE(s.filePosition(v), s.codePosition(v)) << "vertex " << v;
    }
}
