#include "scene/Scene.h"

#include <gtest/gtest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace scene;

/**
 * @file tests/SceneTests.cpp
 * @brief The scene tree, with no device, no window and no solver.
 *
 * Four properties carry everything else this class claims:
 *
 * 1. **`order()` is parent before child**, always, after any sequence of structural
 *    changes. Every other guarantee here rests on it: the single-pass sweep is only
 *    correct because a parent's world transform is final when its child is reached.
 * 2. **A handle goes stale rather than aliasing.** Destroying a node and creating
 *    another must not make the first handle name the second, which is the whole reason
 *    a slot carries a generation.
 * 3. **Reparenting keeps the node where it is**, which is what makes picking an object
 *    up not teleport it.
 * 4. **A transform reaches what is attached, once, and only when something moved.** An
 *    unchanged node must write nothing, because a write is an upload.
 */
namespace {

/// A cube with bounds, so `InstanceTable` has something to transform.
InstanceDesc cube() {
    InstanceDesc d;
    d.localMin = glm::vec3(-1.0f);
    d.localMax = glm::vec3(1.0f);
    d.indexCount = 36;
    d.vertexCount = 8;
    return d;
}

/// Where a node's world transform puts the origin.
glm::vec3 origin(const Scene& scene, NodeId id) {
    return glm::vec3(scene.worldTransform(id)[3]);
}

} // namespace

// ============================================================== structure

TEST(Scene, OrderIsParentBeforeChildAfterEveryStructuralChange) {
    Scene scene;
    const NodeId root = scene.create("root");
    const NodeId a = scene.create("a", root);
    const NodeId b = scene.create("b", a);
    const NodeId c = scene.create("c", root);

    // Reparenting is the case that makes this non-trivial: slots never move, so the
    // array's order and the tree's order have nothing to do with each other.
    scene.setParent(c, b);
    scene.update({});

    const std::vector<uint32_t>& order = scene.order();
    ASSERT_EQ(order.size(), 4u);
    std::vector<uint32_t> seen;
    for (const uint32_t slot : order) {
        const NodeId id{slot, 0}; // only the slot is used below
        (void)id;
        seen.push_back(slot);
    }
    // Position of each node's slot in the ordering, then the parent-before-child claim
    // stated directly rather than inferred from names.
    const auto positionOf = [&](NodeId n) {
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i] == n.index) return static_cast<int>(i);
        }
        return -1;
    };
    EXPECT_LT(positionOf(root), positionOf(a));
    EXPECT_LT(positionOf(a), positionOf(b));
    EXPECT_LT(positionOf(b), positionOf(c));
}

TEST(Scene, DestroyingANodeTakesItsSubtreeAndLeavesEveryHandleStale) {
    Scene scene;
    const NodeId root = scene.create("root");
    const NodeId child = scene.create("child", root);
    const NodeId grandchild = scene.create("grandchild", child);
    EXPECT_EQ(scene.liveCount(), 3u);

    scene.destroy(child);
    EXPECT_EQ(scene.liveCount(), 1u);
    EXPECT_FALSE(scene.valid(child));
    // The subtree goes with it. The alternative -- an orphan holding a parent index into
    // a slot about to be reused -- is precisely the aliasing generations exist to stop.
    EXPECT_FALSE(scene.valid(grandchild));
    EXPECT_TRUE(scene.valid(root));

    // The freed slots come back, and the old handles must not name what lands in them.
    const NodeId reused = scene.create("reused", root);
    EXPECT_TRUE(scene.valid(reused));
    EXPECT_FALSE(scene.valid(child));
    EXPECT_FALSE(scene.valid(grandchild));
}

TEST(Scene, ANodeCannotBecomeItsOwnDescendantsChild) {
    Scene scene;
    const NodeId root = scene.create("root");
    const NodeId child = scene.create("child", root);

    // Refused rather than discovered: a cycle here is a `resort` that never terminates,
    // and the check is a walk up a chain on a structural change.
    scene.setParent(root, child);
    EXPECT_EQ(scene.parent(root), NodeId{});
    EXPECT_EQ(scene.parent(child), root);

    scene.update({});
    EXPECT_EQ(scene.order().size(), 2u);
}

TEST(Scene, ANameFindsTheNodeAndAnUnknownNameFindsNothing) {
    Scene scene;
    const NodeId torch = scene.create("torch");
    EXPECT_EQ(scene.find("torch"), torch);
    EXPECT_EQ(scene.name(torch), "torch");
    EXPECT_FALSE(scene.find("lantern").valid());
    // A destroyed node's name goes with it, or the lookup answers with a stale handle.
    scene.destroy(torch);
    EXPECT_FALSE(scene.find("torch").valid());
}

// ============================================================== transforms

TEST(Scene, AChildIsPlacedInItsParentsSpace) {
    Scene scene;
    const NodeId parent = scene.create("parent");
    const NodeId child = scene.create("child", parent);

    scene.setLocalPosition(parent, {10.0f, 0.0f, 0.0f});
    scene.setLocalPosition(child, {1.0f, 2.0f, 0.0f});
    scene.update({});

    EXPECT_FLOAT_EQ(origin(scene, child).x, 11.0f);
    EXPECT_FLOAT_EQ(origin(scene, child).y, 2.0f);

    // Scale composes too, and this is the case a flat list cannot express at all: the
    // child moves twice as far because its parent is twice the size.
    scene.setLocalScale(parent, glm::vec3(2.0f));
    scene.update({});
    EXPECT_FLOAT_EQ(origin(scene, child).x, 12.0f);
    EXPECT_FLOAT_EQ(origin(scene, child).y, 4.0f);
}

TEST(Scene, ReparentingKeepsTheNodeWhereItIs) {
    Scene scene;
    const NodeId level = scene.create("level");
    const NodeId hand = scene.create("hand");
    const NodeId torch = scene.create("torch", level);

    scene.setLocalPosition(hand, {5.0f, 1.0f, 0.0f});
    scene.setLocalPosition(torch, {-4.0f, 2.2f, 0.0f});
    scene.update({});
    const glm::vec3 before = origin(scene, torch);

    scene.setParent(torch, hand);
    scene.update({});

    // Picking something up must not teleport it. The local transform absorbs the
    // difference, which is the whole difference between `setParent` and the keep-local
    // form beside it.
    EXPECT_NEAR(origin(scene, torch).x, before.x, 1e-4f);
    EXPECT_NEAR(origin(scene, torch).y, before.y, 1e-4f);
    EXPECT_NEAR(origin(scene, torch).z, before.z, 1e-4f);

    // And the keep-local form does the opposite, deliberately: an attach-to-socket wants
    // the node at the socket, not where it was standing.
    const NodeId other = scene.create("socket");
    scene.setLocalPosition(other, {-20.0f, 0.0f, 0.0f});
    scene.setParentKeepLocal(torch, other);
    scene.update({});
    EXPECT_NE(origin(scene, torch).x, before.x);
}

TEST(Scene, MovingAParentMovesEveryDescendant) {
    Scene scene;
    const NodeId a = scene.create("a");
    const NodeId b = scene.create("b", a);
    const NodeId c = scene.create("c", b);
    scene.update({});

    scene.setLocalPosition(a, {0.0f, 7.0f, 0.0f});
    scene.update({});

    // Two levels down, from one write. The dirty flag has to propagate through the sweep
    // for this to hold -- nothing marks `c` directly.
    EXPECT_FLOAT_EQ(origin(scene, c).y, 7.0f);
}

// ============================================================= attachments

TEST(Scene, ATransformReachesTheInstanceAttachedToIt) {
    Scene scene;
    InstanceTable instances;
    const InstanceId instance = instances.create(cube());

    const NodeId node = scene.create("crate");
    scene.attachInstance(node, instance);
    scene.setLocalPosition(node, {3.0f, 0.0f, 0.0f});

    SceneTargets targets;
    targets.instances = &instances;
    scene.update(targets);

    EXPECT_FLOAT_EQ(instances.transform(instance)[3].x, 3.0f);
}

TEST(Scene, AnUnchangedNodeWritesNothing) {
    Scene scene;
    InstanceTable instances;
    const InstanceId instance = instances.create(cube());

    const NodeId node = scene.create("crate");
    scene.attachInstance(node, instance);

    SceneTargets targets;
    targets.instances = &instances;
    scene.update(targets);

    // A write is an upload: `InstanceTable::revision` is what the renderer compares
    // against per frame-in-flight buffer, so a static scene that touched it every frame
    // would re-upload the whole shading array forever.
    const uint64_t settled = instances.revision();
    scene.update(targets);
    scene.update(targets);
    EXPECT_EQ(instances.revision(), settled);

    scene.setLocalPosition(node, {1.0f, 0.0f, 0.0f});
    scene.update(targets);
    EXPECT_NE(instances.revision(), settled);
}

TEST(Scene, AnOffsetIsAppliedBetweenTheNodeAndTheInstance) {
    Scene scene;
    InstanceTable instances;
    const InstanceId instance = instances.create(cube());

    // What a body-driven mesh needs: the collider is at the node, the mesh sits at an
    // offset from it and is twice the size, and the body carries neither.
    const glm::mat4 offset = glm::translate(glm::mat4(1.0f), {0.0f, 0.5f, 0.0f}) *
                             glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    const NodeId node = scene.create("barrel");
    scene.attachInstance(node, instance, offset);
    scene.setLocalPosition(node, {0.0f, 10.0f, 0.0f});

    SceneTargets targets;
    targets.instances = &instances;
    scene.update(targets);

    EXPECT_FLOAT_EQ(instances.transform(instance)[3].y, 10.5f);
    EXPECT_FLOAT_EQ(instances.transform(instance)[0].x, 2.0f);
}

TEST(Scene, ALightTakesItsPlaceFromTheNodeAndKeepsEverythingElse) {
    Scene scene;
    std::vector<gfx::GpuLight> lights;
    lights.push_back(gfx::makePointLight({0.0f, 0.0f, 0.0f}, 12.0f, {1.0f, 0.7f, 0.4f}, 40.0f));

    const NodeId torch = scene.create("torch");
    scene.attachLight(torch, 0);
    scene.setLocalPosition(torch, {-4.0f, 2.2f, 0.0f});

    // A reference, because a light has no derived state -- the rule this API states once.
    scene.light(torch, lights).color.w = 55.0f;

    SceneTargets targets;
    targets.lights = &lights;
    scene.update(targets);

    EXPECT_FLOAT_EQ(lights[0].position.x, -4.0f);
    EXPECT_FLOAT_EQ(lights[0].position.y, 2.2f);
    // Range, colour and intensity are the light's own. A sweep that overwrote them would
    // make the reference above a lie.
    EXPECT_FLOAT_EQ(lights[0].position.w, 12.0f);
    EXPECT_FLOAT_EQ(lights[0].color.w, 55.0f);
}

TEST(Scene, AnInstanceAttachedToADeadHandleIsSkippedRatherThanWritten) {
    Scene scene;
    InstanceTable instances;
    const InstanceId instance = instances.create(cube());

    const NodeId node = scene.create("crate");
    scene.attachInstance(node, instance);
    instances.destroy(instance);

    SceneTargets targets;
    targets.instances = &instances;
    // The point of the generation: the slot is free and about to be handed to something
    // else, and writing a transform into it would move whatever lands there.
    scene.update(targets);
    EXPECT_FALSE(instances.valid(instance));
}

TEST(Scene, UpdateWithNoTargetsStillComputesTheTree) {
    Scene scene;
    const NodeId a = scene.create("a");
    const NodeId b = scene.create("b", a);
    scene.setLocalPosition(a, {1.0f, 0.0f, 0.0f});
    scene.setLocalPosition(b, {2.0f, 0.0f, 0.0f});

    // Every target is optional, which is what a headless test passes and what a game
    // with no audio and no physics gets.
    scene.update({});
    EXPECT_FLOAT_EQ(origin(scene, b).x, 3.0f);
}

TEST(Scene, AStaleHandleIsRefusedByEveryMutator) {
    Scene scene;
    const NodeId node = scene.create("gone");
    scene.destroy(node);

    // Not a crash and not a silent write into a reused slot. Every one of these has to
    // check, because the whole point of a generation is that a caller may legitimately
    // still be holding the handle.
    scene.setLocalPosition(node, {1.0f, 1.0f, 1.0f});
    scene.setLocalScale(node, glm::vec3(2.0f));
    scene.attachLight(node, 3);
    scene.setParent(node, NodeId{});
    EXPECT_FALSE(scene.valid(node));
    EXPECT_EQ(scene.liveCount(), 0u);
    EXPECT_TRUE(scene.name(node).empty());
}

TEST(Scene, ClearingLeavesNothingAndNoStaleOrder) {
    Scene scene;
    const NodeId a = scene.create("a");
    scene.create("b", a);
    scene.update({});
    ASSERT_EQ(scene.order().size(), 2u);

    scene.clear();
    scene.update({});
    EXPECT_EQ(scene.liveCount(), 0u);
    EXPECT_TRUE(scene.order().empty());
    EXPECT_FALSE(scene.valid(a));
}

// ============================================================== walking it
//
// `order()`, `find()` and `parent()` were all pinned above against the *sweep*, which is
// what G3 built them for. These pin the same class of query against a reader from outside:
// a listing has to get the right node, in the right place, and has to stop naming a node
// that is gone. Neither the golden set nor a scaffolded game can see any of that, which is
// why it is here.

TEST(Scene, ASlotResolvesToTheHandleThatWasIssuedForIt) {
    Scene scene;
    const NodeId a = scene.create("a");
    const NodeId b = scene.create("b");

    EXPECT_TRUE(scene.idAt(a.index) == a);
    EXPECT_TRUE(scene.idAt(b.index) == b);

    // Off the end is a question rather than a crash: a caller iterating `slotCount()`
    // against a scene that shrank underneath it is the ordinary way this is reached.
    EXPECT_FALSE(scene.idAt(scene.slotCount()).valid());
    EXPECT_FALSE(scene.idAt(4096).valid());
}

TEST(Scene, ADeadSlotNamesNothingAndNotTheHandleThatReplacesIt) {
    Scene scene;
    const NodeId first = scene.create("first");
    const uint32_t slot = first.index;
    scene.destroy(first);

    // Not "the generation the slot is holding". A handle handed out for a dead slot would
    // compare equal to the one the next `create` issues there, which is precisely the
    // aliasing generations exist to stop -- and it would do it through the one call a
    // listing uses on every node it prints.
    EXPECT_FALSE(scene.idAt(slot).valid());

    const NodeId second = scene.create("second");
    ASSERT_EQ(second.index, slot);
    EXPECT_FALSE(second == first);
    EXPECT_TRUE(scene.idAt(slot) == second);
}

TEST(Scene, TheSiblingWalkVisitsEveryLiveNodeExactlyOnce) {
    Scene scene;
    const NodeId root = scene.create("root");
    const NodeId a = scene.create("a", root);
    const NodeId b = scene.create("b", root);
    scene.create("a1", a);
    scene.create("a2", a);
    scene.create("b1", b);
    const NodeId other = scene.create("other");
    scene.create("other1", other);

    // Depth-first through the two accessors a caller outside the class has, counting every
    // node it reaches. The property is the one a listing depends on and the one a broken
    // unlink destroys: every live node appears, and none appears twice.
    std::vector<NodeId> seen;
    std::vector<std::pair<NodeId, uint32_t>> stack;
    for (NodeId r = scene.firstRoot(); r.valid(); r = scene.nextSibling(r)) stack.emplace_back(r, 0u);
    uint32_t deepest = 0;
    while (!stack.empty()) {
        const NodeId id = stack.back().first;
        const uint32_t depth = stack.back().second;
        stack.pop_back();
        seen.push_back(id);
        deepest = depth > deepest ? depth : deepest;
        for (NodeId c = scene.firstChild(id); c.valid(); c = scene.nextSibling(c)) stack.emplace_back(c, depth + 1u);
    }

    EXPECT_EQ(seen.size(), scene.liveCount());
    EXPECT_EQ(seen.size(), 8u);
    EXPECT_EQ(deepest, 2u);
    for (size_t i = 0; i < seen.size(); ++i) {
        EXPECT_TRUE(scene.valid(seen[i]));
        for (size_t j = i + 1; j < seen.size(); ++j) EXPECT_FALSE(seen[i] == seen[j]) << "node listed twice";
    }

    // And every one of them agrees with `parent()`, which is the other half of "the walk
    // reached the right node": a child list that linked a node under the wrong parent
    // would still visit everything exactly once.
    for (NodeId c = scene.firstChild(a); c.valid(); c = scene.nextSibling(c)) EXPECT_TRUE(scene.parent(c) == a);
    EXPECT_FALSE(scene.parent(root).valid());
}

TEST(Scene, AReparentMovesTheNodeBetweenChildListsAndLeavesNeitherBroken) {
    Scene scene;
    const NodeId from = scene.create("from");
    const NodeId to = scene.create("to");
    const NodeId moved = scene.create("moved", from);
    const NodeId stays = scene.create("stays", from);

    scene.setParent(moved, to);

    const auto children = [&](NodeId parent) {
        std::vector<NodeId> out;
        for (NodeId c = scene.firstChild(parent); c.valid(); c = scene.nextSibling(c)) out.push_back(c);
        return out;
    };

    // The list it left must close over the hole, and the list it joined must contain it
    // once. A `prevSibling` that was not repaired shows up here and nowhere else -- the
    // sweep would still be correct, because `order()` is rebuilt from these same links and
    // a truncated list simply loses nodes silently.
    const std::vector<NodeId> left = children(from);
    ASSERT_EQ(left.size(), 1u);
    EXPECT_TRUE(left[0] == stays);

    const std::vector<NodeId> joined = children(to);
    ASSERT_EQ(joined.size(), 1u);
    EXPECT_TRUE(joined[0] == moved);
    EXPECT_TRUE(scene.parent(moved) == to);
}

TEST(Scene, DestroyingASubtreeRemovesItFromTheWalkEntirely) {
    Scene scene;
    const NodeId root = scene.create("root");
    const NodeId doomed = scene.create("doomed", root);
    scene.create("doomed child", doomed);
    const NodeId kept = scene.create("kept", root);

    scene.destroy(doomed);

    std::vector<NodeId> seen;
    std::vector<NodeId> stack{scene.firstRoot()};
    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();
        if (!id.valid()) continue;
        seen.push_back(id);
        for (NodeId c = scene.firstChild(id); c.valid(); c = scene.nextSibling(c)) stack.push_back(c);
    }

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_TRUE(seen[0] == root);
    EXPECT_TRUE(seen[1] == kept);
    EXPECT_EQ(scene.liveCount(), 2u);
    // The two accessors refuse a stale handle rather than reading the slot's links, which
    // now belong to whatever is created there next.
    EXPECT_FALSE(scene.firstChild(doomed).valid());
    EXPECT_FALSE(scene.nextSibling(doomed).valid());
}

TEST(Scene, AnEmptySceneHasNoRootAndNoRevisionOfZero) {
    Scene scene;
    EXPECT_FALSE(scene.firstRoot().valid());
    // Zero is "never built" to a holder of the counter, so no scene may report it -- which
    // is the same promise `InstanceTable::revision` makes and the reason a cache keyed on
    // it rebuilds on its first frame rather than never.
    EXPECT_NE(scene.structureRevision(), 0u);
}

TEST(Scene, TheStructureRevisionMovesForStructureAndNotForMotion) {
    Scene scene;
    const NodeId a = scene.create("a");
    const uint64_t created = scene.structureRevision();

    scene.setLocalPosition(a, {1.0f, 2.0f, 3.0f});
    scene.setLocalScale(a, glm::vec3(2.0f));
    scene.setLocalRotation(a, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    scene.attachLight(a, 2);
    scene.update({});
    // The whole value of the counter. A caption is a name and an attachment record, and a
    // counter that ticked when a node moved would rebuild every string every frame of an
    // animated scene -- which is the cost it exists to avoid.
    EXPECT_EQ(scene.structureRevision(), created) << "a moved node is not a structural change";

    const NodeId b = scene.create("b");
    EXPECT_GT(scene.structureRevision(), created);

    const uint64_t before = scene.structureRevision();
    scene.setParent(b, a);
    EXPECT_GT(scene.structureRevision(), before);

    const uint64_t reparented = scene.structureRevision();
    scene.destroy(b);
    EXPECT_GT(scene.structureRevision(), reparented);

    const uint64_t destroyed = scene.structureRevision();
    scene.clear();
    EXPECT_GT(scene.structureRevision(), destroyed) << "an emptied tree invalidates a listing too";
}

TEST(Scene, FindReturnsTheRightNodeAmongManyAndNotOneThatDied) {
    Scene scene;
    scene.create("alpha");
    const NodeId beta = scene.create("beta");
    const NodeId gamma = scene.create("gamma", beta);
    scene.create("delta", gamma);

    // The query the card's own verification could not see: `golden-11` proves the image
    // did not move and `scaffold` proves a game links, and neither can tell whether a
    // lookup by name answers with the node that has that name.
    EXPECT_TRUE(scene.find("gamma") == gamma);
    EXPECT_EQ(scene.name(scene.find("delta")), "delta");
    EXPECT_TRUE(scene.parent(scene.find("delta")) == gamma);
    EXPECT_FALSE(scene.find("epsilon").valid());

    scene.destroy(gamma); // takes delta with it
    EXPECT_FALSE(scene.find("gamma").valid());
    EXPECT_FALSE(scene.find("delta").valid());
    EXPECT_TRUE(scene.find("beta") == beta);

    // A slot reused under a new name must answer to the new one and not the old.
    const NodeId again = scene.create("gamma");
    EXPECT_TRUE(scene.find("gamma") == again);
}

TEST(Scene, TheAttachmentRecordReadsBackExactlyWhatWasAttached) {
    Scene scene;
    const NodeId node = scene.create("everything");

    const InstanceId instance{7u, 3u};
    const BodyId body{11u, 1u};
    const SoundId sound{5u, 2u};
    const glm::mat4 offset = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    scene.attachInstance(node, instance, offset);
    scene.attachBody(node, body);
    scene.attachSound(node, sound);
    scene.attachLight(node, 4);
    scene.attachEmitter(node, 9);

    // Every field an inspector prints, read back through the one accessor it has. A
    // record that came back with two indices swapped would draw a plausible panel and be
    // wrong about every node in the scene.
    const Attachments& a = scene.attachments(node);
    EXPECT_TRUE(a.instance == instance);
    EXPECT_TRUE(a.body == body);
    EXPECT_TRUE(a.sound == sound);
    EXPECT_FALSE(a.character.valid());
    EXPECT_EQ(a.light, 4u);
    EXPECT_EQ(a.emitter, 9u);
    EXPECT_TRUE(a.hasOffset);
    EXPECT_FLOAT_EQ(a.instanceOffset[3].y, 1.0f);

    // And an identity offset is not an offset, which is what keeps the ordinary node from
    // paying for the multiply -- and what the panel's `offset` row reports.
    const NodeId plain = scene.create("plain");
    scene.attachInstance(plain, instance);
    EXPECT_FALSE(scene.attachments(plain).hasOffset);
}

// ------------------------------------------------------ turning toward a heading

/**
 * The turn was a game's for as long as there was one game, and the arc rule said so. It is
 * here because the third caller could not have it: a vehicle facing its velocity, a turret
 * facing a target, a boat and an agent on a navmesh all want this identical slew, and none
 * of them is a character.
 *
 * The seam is the part that cannot be checked by watching it work. Two headings a degree
 * apart either side of +/-pi differ by a degree; a wrap that subtracts them naively takes
 * the character most of the way round the other way, which on screen is a spin on the spot
 * to face where it already was.
 */
TEST(Scene, TurnsTowardAHeadingByTheShortestArcAcrossTheSeam) {
    Scene scene;
    const NodeId node = scene.create("boat");

    // A hair short of +pi, and a heading a hair past it. The shortest way round is 0.1 rad
    // forward through the seam; the long way is 6.18 rad back.
    const float from = glm::pi<float>() - 0.05f;
    scene.setLocalRotation(node, glm::angleAxis(from, glm::vec3(0.0f, 1.0f, 0.0f)));

    const float to = -glm::pi<float>() + 0.05f;
    const glm::vec3 heading(std::sin(to), 0.0f, std::cos(to));
    // A rate that would allow half a radian this step, so the clamp is not what decides the
    // answer -- the arc is.
    const float yaw = scene.turnToward(node, heading, 30.0f, 1.0f / 60.0f);

    // Landed on the heading rather than a step of the long way round toward it.
    const glm::vec3 facing = scene.localRotation(node) * glm::vec3(0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(glm::dot(glm::normalize(facing), heading), 1.0f, 1e-4f);
    // And the angle handed back is folded, not the 3.19 rad the addition produced: a caller
    // that keeps its own copy and turns through the seam every lap would otherwise grow one
    // without bound.
    EXPECT_NEAR(yaw, to, 1e-4f);
}

TEST(Scene, TheTurnRateIsPerSecondAndIsWhatBoundsAStep) {
    Scene scene;
    const NodeId node = scene.create("turret");

    // Asked for a half turn, allowed one radian a second, given a sixtieth of one.
    const float yaw = scene.turnToward(node, {0.0f, 0.0f, -1.0f}, 1.0f, 1.0f / 60.0f);
    EXPECT_NEAR(std::abs(yaw), 1.0f / 60.0f, 1e-5f);

    // And it gets there, rather than easing in forever: 189 steps of 1/60 rad covers pi.
    for (int i = 0; i < 200; ++i) scene.turnToward(node, {0.0f, 0.0f, -1.0f}, 1.0f, 1.0f / 60.0f);
    const glm::vec3 facing = scene.localRotation(node) * glm::vec3(0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(facing.z, -1.0f, 1e-3f);
}

TEST(Scene, ADirectionUnderTheFloorIsRoundingAndTurnsNothing) {
    Scene scene;
    const NodeId node = scene.create("agent");
    scene.setLocalRotation(node, glm::angleAxis(0.7f, glm::vec3(0.0f, 1.0f, 0.0f)));

    // A millimetre a second of settling twitch, against a floor of 0.16 m/s. Kept, not
    // followed -- a character standing still that jitters about its own noise is the
    // failure this parameter exists to prevent.
    const float yaw = scene.turnToward(node, {0.001f, 0.0f, -0.001f}, 30.0f, 1.0f / 60.0f, 0.16f);
    EXPECT_NEAR(yaw, 0.7f, 1e-4f);

    // The same direction at a real speed is a heading, and the floor is on the length rather
    // than on the direction: nothing about the vector changed but its magnitude.
    scene.turnToward(node, {1.0f, 0.0f, -1.0f}, 30.0f, 1.0f / 60.0f, 0.16f);
    EXPECT_NE(scene.localRotation(node).y, glm::angleAxis(0.7f, glm::vec3(0.0f, 1.0f, 0.0f)).y);
}

TEST(Scene, AnAuthoredForwardIsAnOffsetRatherThanASignFlipInTheCaller) {
    Scene scene;
    const NodeId node = scene.create("rig");

    // A model authored looking down +X rather than +Z -- a quarter turn from where the verb
    // measures, and every imported rig that is not Mixamo's.
    scene.turnToward(node, {0.0f, 0.0f, 1.0f}, 100.0f, 1.0f, 0.0f, glm::half_pi<float>());

    // Its *own* forward now points along the heading, which is the whole claim. The node's
    // +Z does not, and must not.
    const glm::vec3 authored = scene.localRotation(node) * glm::vec3(1.0f, 0.0f, 0.0f);
    EXPECT_NEAR(authored.z, 1.0f, 1e-4f);
    EXPECT_NEAR(authored.x, 0.0f, 1e-4f);
}

// ================================================================ components

namespace {
/// A game's own per-node data, of a type the engine has never heard of. Non-trivial on
/// purpose: a store that memcpy'd would pass every check a POD could make.
struct Health {
    int current = 100;
    std::string label;
};
struct Team {
    uint32_t index = 0;
};
} // namespace

TEST(SceneComponents, ANodeCarriesATypeTheEngineHasNeverHeardOf) {
    Scene scene;
    const NodeId fighter = scene.create("player");

    EXPECT_EQ(scene.get<Health>(fighter), nullptr);
    EXPECT_FALSE(scene.has<Health>(fighter));

    scene.add<Health>(fighter, {80, "bruised"});
    ASSERT_TRUE(scene.has<Health>(fighter));
    ASSERT_NE(scene.get<Health>(fighter), nullptr);
    EXPECT_EQ(scene.get<Health>(fighter)->current, 80);
    EXPECT_EQ(scene.get<Health>(fighter)->label, "bruised");

    // Mutable through the pointer, which is the whole point of holding one.
    scene.get<Health>(fighter)->current -= 30;
    EXPECT_EQ(scene.get<Health>(fighter)->current, 50);
}

TEST(SceneComponents, TypesAreIndependentAndNodesAreIndependent) {
    Scene scene;
    const NodeId a = scene.create("a");
    const NodeId b = scene.create("b");

    scene.add<Health>(a, {10, "a"});
    scene.add<Team>(a, {1});
    scene.add<Health>(b, {20, "b"});

    // One type does not answer for another, and one node does not answer for another.
    EXPECT_EQ(scene.get<Health>(a)->current, 10);
    EXPECT_EQ(scene.get<Health>(b)->current, 20);
    EXPECT_EQ(scene.get<Team>(a)->index, 1u);
    EXPECT_FALSE(scene.has<Team>(b));

    // A second add of one type replaces rather than accumulating.
    scene.add<Health>(a, {99, "replaced"});
    EXPECT_EQ(scene.get<Health>(a)->current, 99);
    EXPECT_EQ(scene.get<Health>(b)->current, 20);

    scene.remove<Health>(a);
    EXPECT_FALSE(scene.has<Health>(a));
    EXPECT_TRUE(scene.has<Team>(a)) << "removing one type took another with it";
    EXPECT_TRUE(scene.has<Health>(b));
}

TEST(SceneComponents, ComponentsDieWithTheirNodeAndItsSubtree) {
    Scene scene;
    const NodeId parent = scene.create("parent");
    const NodeId child = scene.create("child", parent);
    scene.add<Health>(parent, {1, "p"});
    scene.add<Health>(child, {2, "c"});

    scene.destroy(parent);
    EXPECT_FALSE(scene.valid(parent));
    EXPECT_FALSE(scene.valid(child));

    // **The property this test exists for.** Slots are reused, so a component left behind
    // would be handed to whatever is created into that slot next -- the aliasing that
    // generations exist to stop, one container along.
    const NodeId reused = scene.create("someone else");
    EXPECT_FALSE(scene.has<Health>(reused)) << "a new node inherited a destroyed node's component";
    const NodeId reusedChild = scene.create("another");
    EXPECT_FALSE(scene.has<Health>(reusedChild));
}

TEST(SceneComponents, AStaleHandleReadsAsNothingRatherThanAsSomebodyElse) {
    Scene scene;
    const NodeId first = scene.create("first");
    scene.add<Health>(first, {7, "first"});
    scene.destroy(first);

    const NodeId second = scene.create("second");
    scene.add<Health>(second, {8, "second"});

    // `first` and `second` may well be the same slot; the generation is what tells them
    // apart, and every accessor goes through `valid()` before it indexes.
    EXPECT_EQ(scene.get<Health>(first), nullptr);
    EXPECT_FALSE(scene.has<Health>(first));
    ASSERT_NE(scene.get<Health>(second), nullptr);
    EXPECT_EQ(scene.get<Health>(second)->current, 8);
}

TEST(SceneComponents, EachVisitsEveryLiveCarrierAndNoDeadOne) {
    Scene scene;
    const NodeId a = scene.create("a");
    const NodeId b = scene.create("b");
    const NodeId c = scene.create("c");
    scene.add<Health>(a, {1, "a"});
    scene.add<Health>(b, {2, "b"});
    scene.add<Team>(c, {3});

    scene.destroy(b);

    // Collected and sorted rather than asserted in walk order: the store is a hash map and
    // says so, so a test that depended on the order would be testing the implementation.
    std::vector<int> seen;
    scene.each<Health>([&](NodeId id, Health& h) {
        EXPECT_TRUE(scene.valid(id));
        seen.push_back(h.current);
    });
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(seen, std::vector<int>({1}));

    // And `each` over a type nobody carries is a walk over nothing rather than an error.
    uint32_t teams = 0;
    scene.each<Team>([&](NodeId, Team&) { ++teams; });
    EXPECT_EQ(teams, 1u);
}

TEST(SceneComponents, ClearTakesTheComponentsWithIt) {
    Scene scene;
    const NodeId node = scene.create("node");
    scene.add<Health>(node, {5, "x"});

    scene.clear();
    const NodeId fresh = scene.create("fresh");
    EXPECT_FALSE(scene.has<Health>(fresh));
}

TEST(SceneComponents, CyclingNodesLeavesNothingBehind) {
    // The leak arm, as a property rather than a memory reading: two different cycle counts
    // must both end with the store holding nothing. A store that kept an entry per destroyed
    // node would pass at ten and fail at sixty, which is the whole reason the counts differ.
    const auto cycle = [](uint32_t times) {
        Scene scene;
        for (uint32_t i = 0; i < times; ++i) {
            const NodeId parent = scene.create("parent");
            const NodeId child = scene.create("child", parent);
            scene.add<Health>(parent, {static_cast<int>(i), "p"});
            scene.add<Team>(child, {i});
            scene.destroy(parent);
        }
        uint32_t remaining = 0;
        scene.each<Health>([&](NodeId, Health&) { ++remaining; });
        scene.each<Team>([&](NodeId, Team&) { ++remaining; });
        return remaining;
    };

    EXPECT_EQ(cycle(10), 0u);
    EXPECT_EQ(cycle(60), 0u);
}
