#include "scene/Physics.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <span>
#include <vector>

using namespace scene;

/**
 * @file tests/PhysicsTests.cpp
 * @brief The clock, the interpolation and the world (S4.1, S4.3, S4.4).
 *
 * Physics needs neither Vulkan nor a window, which is what puts the solver in the hosted
 * set and therefore under ASan and TSan. That is not a bonus: S4 is the first subsystem
 * in this engine with a thread pool available to it and the first with a nested library
 * allocating on its own, and the sanitizers are the only place either gets checked.
 *
 * Four properties carry the stage:
 *
 * 1. **The locked clock is the same accumulator.** Fed exactly one step it takes exactly
 *    one step and reports an alpha of exactly zero, which is what makes S4 bit-identical
 *    to the engine before it and keeps all nine golden cases valid.
 * 2. **Time is never silently lost.** Past the per-frame cap the clock drops whole steps,
 *    counts them, and can be asked how many -- 0.9's policy applied to a clock.
 * 3. **The solver is deterministic.** Two worlds fed identical descriptions and identical
 *    steps must agree bit for bit, or 5.3 stops meaning anything for any scene with a
 *    collider in it.
 * 4. **A budget refuses rather than overruns.** Jolt takes a fixed maximum at init, so
 *    the body past it has to be refused and counted rather than written past the end.
 */

namespace {

constexpr float kStep = 1.0f / 60.0f;

ColliderDesc box(const glm::vec3& position, const glm::vec3& halfExtent, ColliderMotion motion) {
    ColliderDesc c;
    c.name = "box";
    c.shape = ColliderShape::Box;
    c.motion = motion;
    c.halfExtent = halfExtent;
    c.transform = glm::translate(glm::mat4(1.0f), position);
    return c;
}

/// A world with a wide static floor at y = 0 and whatever else the caller adds.
void addFloor(PhysicsWorld& world) {
    world.createBody(box({0.0f, -0.5f, 0.0f}, {50.0f, 0.5f, 50.0f}, ColliderMotion::Static));
}

/// The showcase capsule: 1.8 m tall, its origin at its feet. Every character test below
/// starts from this and changes only the row it is about, which is what makes a pair of
/// arms that differ in one number readable as a pair.
ColliderDesc capsule(const glm::vec3& feet) {
    ColliderDesc c;
    c.name = "player";
    c.motion = ColliderMotion::Character;
    c.radius = 0.3f;
    c.halfHeight = 0.6f;
    c.offset = {0.0f, 0.9f, 0.0f};
    c.transform = glm::translate(glm::mat4(1.0f), feet);
    return c;
}

} // namespace

// ------------------------------------------------------------------- FixedClock

TEST(FixedClock, ALockedStepTakesExactlyOneStepAndNoFraction) {
    FixedClock clock(kStep);
    for (int frame = 0; frame < 100; ++frame) {
        clock.accumulate(kStep);
        EXPECT_TRUE(clock.consume());
        EXPECT_FALSE(clock.consume());
        // Exactly zero, not nearly. The whole claim that S4 changed no pixel rests on
        // this being an equality rather than a tolerance.
        EXPECT_EQ(clock.alpha(), 0.0f);
    }
    EXPECT_EQ(clock.stepCount(), 100u);
    EXPECT_EQ(clock.droppedSteps(), 0u);
}

TEST(FixedClock, AHalfStepTakesNoneAndLeavesHalfAnAlpha) {
    FixedClock clock(kStep);
    clock.accumulate(kStep * 0.5f);
    EXPECT_FALSE(clock.consume());
    EXPECT_NEAR(clock.alpha(), 0.5f, 1e-5f);

    // And the other half completes it.
    clock.accumulate(kStep * 0.5f);
    EXPECT_TRUE(clock.consume());
    EXPECT_FALSE(clock.consume());
    EXPECT_NEAR(clock.alpha(), 0.0f, 1e-5f);
}

TEST(FixedClock, ASlowFrameTakesSeveralSteps) {
    FixedClock clock(kStep, 8);
    clock.accumulate(kStep * 3.5f);
    int taken = 0;
    while (clock.consume()) ++taken;
    EXPECT_EQ(taken, 3);
    EXPECT_NEAR(clock.alpha(), 0.5f, 1e-4f);
    EXPECT_EQ(clock.droppedSteps(), 0u);
}

TEST(FixedClock, PastTheCapTimeIsDroppedAndCounted) {
    FixedClock clock(kStep, 4);
    clock.accumulate(kStep * 10.0f);

    int taken = 0;
    while (clock.consume()) ++taken;
    EXPECT_EQ(taken, 4);
    // Six were owed and refused. Counted, not carried: carrying them would only defer
    // the same overrun into the next frame and hide that it happened at all.
    EXPECT_EQ(clock.droppedSteps(), 6u);
    EXPECT_LT(clock.alpha(), 1.0f);

    // The next frame starts with a fresh budget rather than inheriting the refusal.
    clock.accumulate(kStep);
    EXPECT_TRUE(clock.consume());
}

TEST(FixedClock, NegativeTimeIsIgnored) {
    FixedClock clock(kStep);
    clock.accumulate(-1.0f);
    EXPECT_FALSE(clock.consume());
    clock.accumulate(kStep);
    EXPECT_TRUE(clock.consume());
}

TEST(FixedClock, AlphaStaysBelowOne) {
    // A frame that accumulated almost a whole extra step must not report an alpha of 1,
    // which would render the *next* state and make interpolation jump a step ahead.
    FixedClock clock(kStep, 1);
    clock.accumulate(kStep * 1.999f);
    EXPECT_TRUE(clock.consume());
    EXPECT_LT(clock.alpha(), 1.0f);
    EXPECT_GT(clock.alpha(), 0.99f);
}

// ---------------------------------------------------------------- interpolation

TEST(PhysicsInterpolation, EndpointsAreExact) {
    PhysicsState a;
    a.position = {1.0f, 2.0f, 3.0f};
    PhysicsState b;
    b.position = {5.0f, 2.0f, 3.0f};
    b.rotation = glm::angleAxis(1.0f, glm::vec3(0.0f, 1.0f, 0.0f));

    const glm::mat4 at0 = interpolateState(a, b, 0.0f);
    EXPECT_NEAR(at0[3].x, 1.0f, 1e-5f);
    const glm::mat4 at1 = interpolateState(a, b, 1.0f);
    EXPECT_NEAR(at1[3].x, 5.0f, 1e-5f);
    const glm::mat4 half = interpolateState(a, b, 0.5f);
    EXPECT_NEAR(half[3].x, 3.0f, 1e-5f);
}

TEST(PhysicsInterpolation, ARotationIsSlerpedRatherThanLerped) {
    PhysicsState a;
    PhysicsState b;
    b.rotation = glm::angleAxis(3.0f, glm::vec3(0.0f, 1.0f, 0.0f)); // most of a half turn

    const glm::mat4 half = interpolateState(a, b, 0.5f);
    // A slerp halfway through a 3-radian turn is a 1.5-radian turn, so the transformed
    // +X axis lands at (cos 1.5, 0, -sin 1.5). A component-wise lerp of the quaternions
    // would land measurably short of it.
    const glm::vec3 x = glm::vec3(half * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    EXPECT_NEAR(x.x, std::cos(1.5f), 1e-4f);
    EXPECT_NEAR(x.z, -std::sin(1.5f), 1e-4f);
}

// ------------------------------------------------------------------------ world

TEST(PhysicsWorld, AnEmptyWorldStepsAndCostsNothing) {
    PhysicsWorld world;
    world.init({}, 0);
    EXPECT_TRUE(world.empty());
    world.finalize();
    world.step(kStep);
    EXPECT_EQ(world.bodyCount(), 0u);
    EXPECT_EQ(world.characterCount(), 0u);
}

TEST(PhysicsWorld, ABoxFallsAndComesToRestOnTheFloor) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);
    const BodyId b = world.createBody(box({0.0f, 4.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    ASSERT_TRUE(b.valid());
    world.finalize();

    for (int i = 0; i < 300; ++i) world.step(kStep);

    const glm::mat4 m = world.bodyTransform(b, 0.0f);
    // Resting on a floor whose top face is y = 0, so a half-metre box sits at 0.5 --
    // less Jolt's penetration slop, which is 2 cm by default and is why the tolerance
    // here is 5 and not 1. A resting body is *meant* to be slightly inside what it rests
    // on: that is the margin the solver has to push against to stay stable, and a test
    // that demanded exactly 0.5 would be asserting the solver is configured wrongly.
    EXPECT_NEAR(m[3].y, 0.5f, 0.05f);
    EXPECT_NEAR(m[3].x, 0.0f, 0.02f);
    EXPECT_NEAR(m[3].z, 0.0f, 0.02f);
}

TEST(PhysicsWorld, AStaticBodyDoesNotMoveAndSaysSo) {
    PhysicsWorld world;
    world.init({}, 1);
    const BodyId floor = world.createBody(box({0.0f, -0.5f, 0.0f}, {5.0f, 0.5f, 5.0f}, ColliderMotion::Static));
    world.finalize();
    EXPECT_FALSE(world.bodyMoves(floor));

    const glm::mat4 before = world.bodyTransform(floor, 0.0f);
    for (int i = 0; i < 60; ++i) world.step(kStep);
    const glm::mat4 after = world.bodyTransform(floor, 0.0f);
    EXPECT_EQ(before[3].y, after[3].y);
}

TEST(PhysicsWorld, TwoWorldsFedIdenticallyAgreeBitForBit) {
    // The property 5.3 rests on for every scene with a collider in it. A solver that
    // drifted between runs would make a golden image of a physics scene meaningless, and
    // it would do so silently -- the frames would still look right.
    const auto run = [](std::vector<glm::vec3>& out) {
        PhysicsWorld world;
        world.init({}, 12);
        addFloor(world);
        std::vector<BodyId> ids;
        for (int i = 0; i < 8; ++i) {
            const float f = static_cast<float>(i);
            ids.push_back(world.createBody(box({f * 0.31f - 1.0f, 2.0f + f * 0.7f, f * 0.17f}, {0.4f, 0.4f, 0.4f},
                                            ColliderMotion::Dynamic)));
        }
        world.finalize();
        for (int i = 0; i < 240; ++i) world.step(kStep);
        for (BodyId id : ids) out.push_back(glm::vec3(world.bodyTransform(id, 0.0f)[3]));
    };

    std::vector<glm::vec3> a;
    std::vector<glm::vec3> b;
    run(a);
    run(b);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].x, b[i].x) << "body " << i;
        EXPECT_EQ(a[i].y, b[i].y) << "body " << i;
        EXPECT_EQ(a[i].z, b[i].z) << "body " << i;
    }
}

TEST(PhysicsWorld, TheWorldGrowsRatherThanRefusing) {
    // The budget is a **floor** since C40, and this arm is the whole of what that means: a
    // world told to start at three and asked for a fourth allocates a bigger one rather than
    // turning the caller away. `refusedBodies` stays at zero, which is what makes a non-zero
    // count a defect again instead of a number somebody guessed low.
    PhysicsConfig cfg;
    cfg.bodyBudget = 3;
    PhysicsWorld world;
    world.init(cfg, 0);
    ASSERT_EQ(world.bodyCapacity(), 3u);

    std::vector<BodyId> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(
            world.createBody(box({0.0f, static_cast<float>(i), 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic)));
        ASSERT_TRUE(ids.back().valid());
    }

    const BodyId fourth = world.createBody(box({0.0f, 9.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    EXPECT_TRUE(fourth.valid());
    EXPECT_EQ(world.refusedBodies(), 0u);
    EXPECT_EQ(world.bodyCount(), 4u);
    EXPECT_GT(world.bodyCapacity(), 3u);

    world.finalize();
    world.step(kStep);
}

TEST(PhysicsWorld, AHandleTakenBeforeAGrowthStillNamesTheSameBody) {
    // **The property the whole rebuild rests on.** A `BodyId` is an index and a generation
    // into `PhysicsWorld`'s own vectors; the Jolt body underneath is destroyed and remade at
    // a new raw id. A handle a game took before the growth has to keep naming the same body,
    // at the same place, or growth is a silent corruption rather than a convenience.
    PhysicsConfig cfg;
    cfg.bodyBudget = 2;
    PhysicsWorld world;
    world.init(cfg, 0);
    addFloor(world);

    const BodyId watched = world.createBody(box({3.0f, 7.0f, -2.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    ASSERT_TRUE(world.valid(watched));
    // **After `finalize`, or the baseline is zero.** `bodyTransform` reads the interpolation
    // snapshots, and nothing has filled them until the first `snapshot()` -- so a before/after
    // pair taken either side of that compares two zeroes and passes without testing anything.
    world.finalize();
    const glm::vec3 before(world.bodyTransform(watched, 1.0f)[3]);
    ASSERT_FLOAT_EQ(before.y, 7.0f);

    // Past the floor plus the watched body, so this is the call that rebuilds.
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(
            world.createBody(box({static_cast<float>(i), 20.0f, 0.0f}, {0.4f, 0.4f, 0.4f}, ColliderMotion::Dynamic))
                .valid());
    }
    ASSERT_GT(world.bodyCapacity(), 2u);

    EXPECT_TRUE(world.valid(watched));
    const glm::vec3 after(world.bodyTransform(watched, 1.0f)[3]);
    EXPECT_FLOAT_EQ(before.x, after.x);
    EXPECT_FLOAT_EQ(before.y, after.y);
    EXPECT_FLOAT_EQ(before.z, after.z);

    // And it is still a body the solver moves, rather than one carried across as furniture.
    for (int i = 0; i < 30; ++i) world.step(kStep);
    EXPECT_LT(world.bodyTransform(watched, 1.0f)[3].y, after.y);
}

TEST(PhysicsWorld, ACharacterSurvivesAGrowthWithItsPoseAndItsWindows) {
    // A `CharacterVirtual` holds the `PhysicsSystem` it queries, so it is the one thing a
    // growth cannot carry across intact -- it is destroyed and rebuilt. What must not change
    // is anything a game can observe: where it stands, how fast it is going, and the jump
    // windows, which live in `PhysicsWorld` rather than in Jolt and would be the easy thing
    // to reset by accident.
    PhysicsConfig cfg;
    cfg.bodyBudget = 2;
    PhysicsWorld world;
    world.init(cfg, 0);
    addFloor(world);

    const PhysicsCharacterId who = world.createCharacter(capsule({1.0f, 0.0f, -3.0f}));
    ASSERT_TRUE(world.valid(who));
    world.finalize();

    world.setCharacterInput(who, {1.0f, 0.0f, 0.0f}, false);
    for (int i = 0; i < 20; ++i) world.step(kStep);

    const glm::vec3 before(world.characterTransform(who, 1.0f)[3]);
    const float speed = world.characterSpeed(who);
    const bool grounded = world.characterOnGround(who);
    ASSERT_GT(speed, 0.0f);

    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(
            world.createBody(box({static_cast<float>(i), 20.0f, 0.0f}, {0.4f, 0.4f, 0.4f}, ColliderMotion::Dynamic))
                .valid());
    }
    ASSERT_GT(world.bodyCapacity(), 2u);

    EXPECT_TRUE(world.valid(who));
    const glm::vec3 after(world.characterTransform(who, 1.0f)[3]);
    EXPECT_NEAR(before.x, after.x, 1e-5f);
    EXPECT_NEAR(before.y, after.y, 1e-5f);
    EXPECT_NEAR(before.z, after.z, 1e-5f);
    EXPECT_NEAR(speed, world.characterSpeed(who), 1e-5f);
    EXPECT_EQ(grounded, world.characterOnGround(who));

    // And it keeps walking rather than standing still against a world it no longer knows.
    world.setCharacterInput(who, {1.0f, 0.0f, 0.0f}, false);
    for (int i = 0; i < 20; ++i) world.step(kStep);
    EXPECT_GT(world.characterTransform(who, 1.0f)[3].x, after.x);
    EXPECT_TRUE(world.characterOnGround(who));
}

TEST(PhysicsWorld, WithNoBudgetTheCapacityComesFromTheScene) {
    PhysicsWorld world;
    world.init({}, 40);
    // Sized from data plus stated headroom, rather than from a constant a scene can
    // exceed. The exact headroom is not the property; that it scales with the scene is.
    EXPECT_GT(world.bodyCapacity(), 40u);
}

TEST(PhysicsWorld, AHullIsBuiltFromItsPoints) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);

    ColliderDesc c;
    c.name = "hull";
    c.shape = ColliderShape::Hull;
    c.motion = ColliderMotion::Dynamic;
    c.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, 0.0f));
    for (float x : {-0.5f, 0.5f}) {
        for (float y : {-0.5f, 0.5f}) {
            for (float z : {-0.5f, 0.5f}) c.points.push_back({x, y, z});
        }
    }
    const BodyId b = world.createBody(c);
    ASSERT_TRUE(b.valid());
    world.finalize();

    for (int i = 0; i < 300; ++i) world.step(kStep);
    EXPECT_NEAR(world.bodyTransform(b, 0.0f)[3].y, 0.5f, 0.05f);
}

TEST(PhysicsWorld, AHullWithNoPointsIsRefusedRatherThanGuessedAt) {
    PhysicsWorld world;
    world.init({}, 1);
    ColliderDesc c;
    c.name = "empty hull";
    c.shape = ColliderShape::Hull;
    c.motion = ColliderMotion::Dynamic;
    EXPECT_FALSE(world.createBody(c).valid());
    EXPECT_EQ(world.bodyCount(), 0u);
}

TEST(PhysicsWorld, AMeshShapeCollides) {
    PhysicsWorld world;
    world.init({}, 2);

    // Two triangles making a 10x10 quad at y = 0, which is what a glTF floor is.
    ColliderDesc floor;
    floor.name = "floor";
    floor.shape = ColliderShape::Mesh;
    floor.motion = ColliderMotion::Static;
    floor.points = {{-5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, 5.0f}, {-5.0f, 0.0f, 5.0f}};
    floor.indices = {0, 2, 1, 0, 3, 2};
    ASSERT_TRUE(world.createBody(floor).valid());

    const BodyId b = world.createBody(box({0.0f, 3.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    world.finalize();
    for (int i = 0; i < 300; ++i) world.step(kStep);
    EXPECT_NEAR(world.bodyTransform(b, 0.0f)[3].y, 0.5f, 0.05f);
}

TEST(PhysicsWorld, ANodeScaleReachesTheShape) {
    // A glTF node's scale has nowhere to go on a Jolt body, so it has to reach the shape.
    // Without that, a floor scaled to 50x in the file is a one-metre pad the boxes miss.
    PhysicsWorld world;
    world.init({}, 2);

    ColliderDesc floor = box({0.0f, -0.5f, 0.0f}, {1.0f, 0.5f, 1.0f}, ColliderMotion::Static);
    floor.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f)) *
                      glm::scale(glm::mat4(1.0f), glm::vec3(20.0f, 1.0f, 20.0f));
    world.createBody(floor);

    const BodyId b = world.createBody(box({8.0f, 3.0f, 8.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    world.finalize();
    for (int i = 0; i < 300; ++i) world.step(kStep);
    // Landed on the scaled floor rather than falling past where an unscaled one ended.
    EXPECT_NEAR(world.bodyTransform(b, 0.0f)[3].y, 0.5f, 0.05f);
}

// ------------------------------------------------------------------- characters

TEST(PhysicsCharacter, StandsOnTheFloorRatherThanSinkingThroughIt) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);

    ColliderDesc c;
    c.name = "player";
    c.motion = ColliderMotion::Character;
    c.radius = 0.3f;
    c.halfHeight = 0.6f;
    // Authored at the feet, so the capsule's centre is a radius plus a half-height up.
    c.offset = {0.0f, 0.9f, 0.0f};
    c.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f));
    const PhysicsCharacterId id = world.createCharacter(c);
    ASSERT_TRUE(id.valid());
    EXPECT_EQ(world.characterCount(), 1u);
    world.finalize();

    for (int i = 0; i < 180; ++i) world.step(kStep);

    EXPECT_TRUE(world.characterOnGround(id));
    const glm::mat4 m = world.characterTransform(id, 0.0f);
    // The character's own origin is at its feet, so it comes to rest on the floor rather
    // than a capsule's radius into it.
    EXPECT_NEAR(m[3].y, 0.0f, 0.05f);
}

/**
 * The two interpolation snapshots have to stay the same length, and until G12 nothing
 * checked it.
 *
 * `finalize()` starts them equal and `step()` assigns one to the other, so every accessor
 * interpolates `previous[slot]` against `current[slot]` behind a bounds check on `current`
 * alone. A body created *after* load breaks the invariant those accessors rest on:
 * `createBody` grows `bodies`, `characterTransform` addresses `bodies.size() + index`, and
 * for exactly one step that slot exists in `current` and not in `previous`. The read is off
 * the end of a vector -- a heap-buffer-overflow under ASan, and in a release build a
 * character that reported having travelled `inf` metres, which is how it was found.
 *
 * Reachable from a game the day `PhysicsWorld::createBody` became something `Game::init`
 * could call, which is what G9 did fifteen times over.
 */
TEST(PhysicsWorld, ABodyCreatedAfterFinalizeKeepsBothSnapshotsTheSameLength) {
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);

    ColliderDesc c;
    c.name = "player";
    c.motion = ColliderMotion::Character;
    c.radius = 0.3f;
    c.halfHeight = 0.6f;
    c.offset = {0.0f, 0.9f, 0.0f};
    c.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.1f, 0.0f));
    const PhysicsCharacterId player = world.createCharacter(c);
    ASSERT_TRUE(player.valid());
    world.finalize();

    // What a game does in `Game::init`: the scene is loaded and finalised, and only then
    // does the game build its own props out of the same public verb.
    std::vector<BodyId> crates;
    for (int i = 0; i < 4; ++i) {
        ColliderDesc crate = box({static_cast<float>(i) * 2.0f, 3.0f, 4.0f}, glm::vec3(0.3f), ColliderMotion::Dynamic);
        crate.mass = 5.0f;
        crates.push_back(world.createBody(crate));
        ASSERT_TRUE(crates.back().valid());
    }

    // The step that grows `current` past `previous`. The read after it is the only one that
    // could go short, and every read after that is safe again.
    world.step(kStep);

    const glm::mat4 m = world.characterTransform(player, 0.0f);
    ASSERT_TRUE(std::isfinite(m[3].x) && std::isfinite(m[3].y) && std::isfinite(m[3].z));
    EXPECT_NEAR(m[3].x, 0.0f, 0.01f);
    EXPECT_NEAR(m[3].z, 0.0f, 0.01f);

    // And a body created between two steps interpolates from where it was made rather than
    // from the origin, which is what filling the new slots from `current` buys over
    // resizing them to zero.
    for (int i = 0; i < 4; ++i) {
        const glm::mat4 b = world.bodyTransform(crates[static_cast<size_t>(i)], 0.0f);
        EXPECT_NEAR(b[3].x, static_cast<float>(i) * 2.0f, 0.01f);
        EXPECT_NEAR(b[3].z, 4.0f, 0.01f);
    }
}

/**
 * Rewritten by C20, and the old tail is the reason.
 *
 * It asserted that the speed reached zero within 0.1 of the input being zeroed, which was
 * a correct pin on a controller that *assigned* the requested velocity -- and it is exactly
 * the behaviour this row removes. Deleting the assertion would have left the ramp
 * unchecked; loosening it would have left "eventually stops", which gravity satisfies. So
 * it asserts the ramp itself, in both directions, against rates chosen slow enough that a
 * step of it is visible: 6 m/s^2 puts a full second between rest and 3 m/s each way.
 */
TEST(PhysicsCharacter, MovesWhereItIsToldAndRampsIntoAndOutOfIt) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);

    ColliderDesc c = capsule({0.0f, 0.1f, 0.0f});
    c.moveSpeed = 3.0f;
    c.acceleration = 6.0f;
    c.deceleration = 6.0f;
    const PhysicsCharacterId id = world.createCharacter(c);
    world.finalize();

    for (int i = 0; i < 30; ++i) world.step(kStep); // settle

    const float startZ = world.characterTransform(id, 0.0f)[3].z;
    // A quarter of the ramp. `v = a t` and nothing else, because the request is a straight
    // line and the character is on flat ground: 6 m/s^2 for 15 steps is 1.5 m/s, which is
    // half the top speed and therefore a number the old assignment could never produce.
    for (int i = 0; i < 15; ++i) {
        world.setCharacterInput(id, {0.0f, 0.0f, 1.0f}, false);
        world.step(kStep);
    }
    EXPECT_NEAR(world.characterSpeed(id), 1.5f, 0.2f);

    for (int i = 0; i < 60; ++i) {
        world.setCharacterInput(id, {0.0f, 0.0f, 1.0f}, false);
        world.step(kStep);
    }
    const float endZ = world.characterTransform(id, 0.0f)[3].z;
    // Still arrives, and still stops there: a ramp that overshot its request would be a
    // spring, which is a different feature nobody asked for.
    EXPECT_GT(endZ - startZ, 2.0f);
    EXPECT_NEAR(world.characterSpeed(id), 3.0f, 0.1f);

    // And the far end. Half a second after the input is zeroed the character is at half
    // speed -- *not* stopped, which is what the old tail asserted.
    for (int i = 0; i < 15; ++i) {
        world.setCharacterInput(id, {0.0f, 0.0f, 0.0f}, false);
        world.step(kStep);
    }
    EXPECT_NEAR(world.characterSpeed(id), 1.5f, 0.2f);

    for (int i = 0; i < 30; ++i) {
        world.setCharacterInput(id, {0.0f, 0.0f, 0.0f}, false);
        world.step(kStep);
    }
    EXPECT_NEAR(world.characterSpeed(id), 0.0f, 0.05f);
}

/// The other half of the pair: a rate large enough to close the gap inside one step *is*
/// the assignment C20 replaced, so a game that wants the old feel authors it rather than
/// editing `engine/`. Written as a test because it is the claim the header makes.
TEST(PhysicsCharacter, AHugeAccelerationIsTheOldAssignment) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);

    ColliderDesc c = capsule({0.0f, 0.1f, 0.0f});
    c.moveSpeed = 3.0f;
    c.acceleration = 1.0e6f;
    c.deceleration = 1.0e6f;
    const PhysicsCharacterId id = world.createCharacter(c);
    world.finalize();
    for (int i = 0; i < 30; ++i) world.step(kStep);

    world.setCharacterInput(id, {0.0f, 0.0f, 1.0f}, false);
    world.step(kStep);
    EXPECT_NEAR(world.characterSpeed(id), 3.0f, 0.05f);

    world.setCharacterInput(id, {0.0f, 0.0f, 0.0f}, false);
    world.step(kStep);
    EXPECT_NEAR(world.characterSpeed(id), 0.0f, 0.05f);
}

TEST(PhysicsCharacter, AJumpIsLatchedRatherThanMissed) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);

    ColliderDesc c = capsule({0.0f, 0.1f, 0.0f});
    c.jumpSpeed = 5.0f;
    const PhysicsCharacterId id = world.createCharacter(c);
    world.finalize();
    for (int i = 0; i < 30; ++i) world.step(kStep);

    // Pressed on a frame that takes no step -- which is every other frame at 120 Hz
    // against a 60 Hz simulation -- and then released. Assigning rather than latching
    // would lose it entirely.
    world.setCharacterInput(id, {0.0f, 0.0f, 0.0f}, true);
    world.setCharacterInput(id, {0.0f, 0.0f, 0.0f}, false);

    const float restY = world.characterTransform(id, 0.0f)[3].y;
    float peak = restY;
    for (int i = 0; i < 40; ++i) {
        world.step(kStep);
        peak = std::max(peak, world.characterTransform(id, 0.0f)[3].y);
    }
    EXPECT_GT(peak - restY, 0.5f);
}

// ---------------------------------------------------------- placement (C29)

TEST(PhysicsCharacter, APlacedCharacterArrivesStoppedAndAtOnce) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);

    const PhysicsCharacterId id = world.createCharacter(capsule({0.0f, 20.0f, 0.0f}));
    world.finalize();
    // Half a second of gravity is about 5 m/s downward, which is the residual the placement
    // has to throw away.
    for (int i = 0; i < 30; ++i) world.step(kStep);
    ASSERT_LT(world.characterTransform(id, 0.0f)[3].y, 19.0f);

    world.setCharacterTransform(id, glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 6.0f, -4.0f)));

    // Every alpha, because both snapshots were written. Interpolating against the one the
    // character fell from would draw it crossing the map for the rest of the frame.
    for (const float alpha : {0.0f, 0.5f, 1.0f}) {
        const glm::vec3 at(world.characterTransform(id, alpha)[3]);
        EXPECT_NEAR(at.x, 5.0f, 1e-4f);
        EXPECT_NEAR(at.y, 6.0f, 1e-4f);
        EXPECT_NEAR(at.z, -4.0f, 1e-4f);
    }

    // One step of fall from rest is g*dt^2, about 2.7 mm. The 5 m/s it was already doing
    // would be 83 mm over the same step, so this is the assertion that the velocity did not
    // arrive with the position.
    const float before = world.characterTransform(id, 0.0f)[3].y;
    world.step(kStep);
    EXPECT_LT(before - world.characterTransform(id, 0.0f)[3].y, 0.01f);
}

TEST(PhysicsCharacter, APlacedCharacterLandsAndStandsWhereItWasPut) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);

    const PhysicsCharacterId id = world.createCharacter(capsule({0.0f, 0.1f, 0.0f}));
    world.finalize();
    for (int i = 0; i < 30; ++i) world.step(kStep);
    ASSERT_TRUE(world.characterOnGround(id));

    world.setCharacterTransform(id, glm::translate(glm::mat4(1.0f), glm::vec3(7.0f, 3.0f, -7.0f)));
    for (int i = 0; i < 120; ++i) world.step(kStep);

    EXPECT_TRUE(world.characterOnGround(id));
    const glm::vec3 at(world.characterTransform(id, 0.0f)[3]);
    EXPECT_NEAR(at.x, 7.0f, 0.05f);
    EXPECT_NEAR(at.y, 0.0f, 0.05f);
    EXPECT_NEAR(at.z, -7.0f, 0.05f);
}

/**
 * The one behaviour that needs the contacts refreshed at the placement rather than left to
 * the next step. `step()` reads the ground state *before* it sweeps, so a character
 * teleported off a floor still reports standing on it for one step -- which is a whole
 * coyote window, and long enough to spend a jump on ground that is no longer there.
 *
 * The two arms differ in the height placed at and in nothing else, so an implementation
 * that simply refused every jump after a placement fails the second.
 */
TEST(PhysicsCharacter, APlacementIntoTheAirCannotJumpOffTheGroundItLeft) {
    for (const float height : {10.0f, 0.0f}) {
        PhysicsWorld world;
        world.init({}, 2);
        addFloor(world);

        const PhysicsCharacterId id = world.createCharacter(capsule({0.0f, 0.1f, 0.0f}));
        world.finalize();
        for (int i = 0; i < 30; ++i) world.step(kStep);
        ASSERT_TRUE(world.characterOnGround(id));

        world.setCharacterTransform(id, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, height, 0.0f)));
        world.setCharacterInput(id, {0.0f, 0.0f, 0.0f}, true);

        bool launched = false;
        for (int i = 0; i < 30; ++i) {
            world.step(kStep);
            launched = launched || world.characterJumped(id);
        }
        EXPECT_EQ(launched, height == 0.0f) << "placed at " << height << " m";
    }
}

// -------------------------------------------------- a jump that forgives (C20)

namespace {

/// What one scripted jump did. Every window test below is a pair of these that differ in
/// exactly one number, which is the only shape in which "the window is what made this
/// happen" is a claim rather than an assertion.
struct JumpRun {
    int touchdown = -1;    ///< steps after the launch at which the character stood again
    int secondLaunch = -1; ///< steps after the launch at which a second launch was applied
};

/**
 * Jump, then press jump again on step `pressAt` of the flight -- or never, for `pressAt`
 * below zero.
 *
 * `characterJumped` is what is read rather than the height, because a height is satisfied
 * by a character that was still rising. The whole question is whether the *solver* applied
 * a second launch, and that is the one thing a caller could not previously ask.
 */
JumpRun jumpThenPressAgain(uint32_t bufferSteps, int pressAt) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);

    ColliderDesc c = capsule({0.0f, 0.1f, 0.0f});
    c.jumpSpeed = 5.0f;
    c.jumpBufferSteps = bufferSteps;
    const PhysicsCharacterId id = world.createCharacter(c);
    world.finalize();
    for (int i = 0; i < 30; ++i) world.step(kStep);

    world.setCharacterInput(id, {}, true);
    world.step(kStep); // the launch

    JumpRun r;
    bool airborne = false;
    for (int i = 0; i < 240; ++i) {
        world.setCharacterInput(id, {}, i == pressAt);
        world.step(kStep);
        if (world.characterJumped(id) && r.secondLaunch < 0) r.secondLaunch = i;
        // The ground state lags the launch by a sweep, so the first step of the flight
        // still reports standing. Touchdown is the return, not the departure.
        const bool onGround = world.characterOnGround(id);
        if (!onGround) airborne = true;
        if (airborne && onGround && r.touchdown < 0) r.touchdown = i;
    }
    return r;
}

} // namespace

/**
 * **The buffer, and both of its negative arms.**
 *
 * A test that presses jump while grounded proves nothing about a jump buffer: it would
 * pass against the controller this row replaced. The claim is about a press that arrives
 * at a moment the character *cannot act on*, so the press here happens in mid-air, several
 * steps before the landing, and the launch it produces happens on the ground afterwards.
 *
 * The first run is what places the press. Reading the touchdown step off a bare flight
 * rather than computing `2v/g` is deliberate: the assertion is "inside the window" and
 * "outside the window", and both of those are relative to a landing this arm measures
 * rather than predicts, so a change to the solver's landing moves the press with it
 * instead of silently invalidating the arms.
 */
TEST(PhysicsCharacter, AJumpPressedBeforeTheLandingIsHeldUntilTheLandingCanUseIt) {
    const JumpRun bare = jumpThenPressAgain(10, -1);
    ASSERT_GT(bare.touchdown, 40) << "the flight is too short to press inside";
    EXPECT_EQ(bare.secondLaunch, -1) << "nothing was pressed and something launched";

    // Five steps before touching down, against a ten-step window.
    const int inside = bare.touchdown - 5;
    const JumpRun buffered = jumpThenPressAgain(10, inside);
    ASSERT_GE(buffered.secondLaunch, 0) << "the buffered press never reached the solver";
    // And it launched *after* the press rather than on it, which is what distinguishes a
    // buffer from a controller that will launch out of thin air.
    EXPECT_GT(buffered.secondLaunch, inside);

    // Negative arm one: the identical press with the window set to zero. This is the run
    // that says the launch above came from the buffer and from nothing else.
    EXPECT_EQ(jumpThenPressAgain(0, inside).secondLaunch, -1);

    // Negative arm two: the window kept, the press moved outside it. A buffer that never
    // expired would pass the first arm and fail this one.
    EXPECT_EQ(jumpThenPressAgain(10, bare.touchdown - 25).secondLaunch, -1);
}

namespace {

/// What one walk off a ledge did.
struct LedgeRun {
    int left = -1;   ///< the step at which the ground went away
    int launch = -1; ///< the step at which a launch was applied
    float rise = 0.0f;
};

/// Walk forward off a two-metre platform with nothing under it, and press jump on step
/// `pressAtStep` -- or never, below zero.
LedgeRun walkOffLedge(uint32_t coyoteSteps, int pressAtStep) {
    PhysicsWorld world;
    world.init({}, 2);
    // A platform and no floor. A character that walks off this one keeps falling, so a
    // rise afterwards can only have come from a launch.
    world.createBody(box({0.0f, -0.5f, 0.0f}, {2.0f, 0.5f, 2.0f}, ColliderMotion::Static));

    ColliderDesc c = capsule({0.0f, 0.1f, 0.0f});
    c.moveSpeed = 3.0f;
    // The ledge is the subject here, not the ramp: an instant response keeps the departure
    // step a function of the platform's size alone.
    c.acceleration = 1.0e6f;
    c.deceleration = 1.0e6f;
    c.coyoteSteps = coyoteSteps;
    const PhysicsCharacterId id = world.createCharacter(c);
    world.finalize();
    for (int i = 0; i < 30; ++i) world.step(kStep);

    LedgeRun r;
    float atDeparture = 0.0f;
    for (int i = 0; i < 200; ++i) {
        world.setCharacterInput(id, {0.0f, 0.0f, 1.0f}, i == pressAtStep);
        world.step(kStep);
        const float y = world.characterTransform(id, 0.0f)[3].y;
        if (r.left < 0 && !world.characterOnGround(id)) {
            r.left = i;
            atDeparture = y;
        }
        if (world.characterJumped(id) && r.launch < 0) r.launch = i;
        if (r.left >= 0) r.rise = std::max(r.rise, y - atDeparture);
    }
    return r;
}

} // namespace

/**
 * **The coyote window, and both of its negative arms.**
 *
 * Same shape as the buffer's, from the other side: the press is legal here only because
 * the ground *was* there a few steps ago. Nothing catches the character, so a rise after
 * the ledge can only be a launch -- and `characterJumped` says so directly.
 */
TEST(PhysicsCharacter, AJumpPressedJustAfterTheGroundWentAwayStillLaunches) {
    const LedgeRun bare = walkOffLedge(6, -1);
    ASSERT_GE(bare.left, 0) << "the character never left the platform";
    EXPECT_EQ(bare.launch, -1);
    EXPECT_LT(bare.rise, 0.01f) << "it rose without being asked to";

    const LedgeRun inside = walkOffLedge(6, bare.left + 3);
    EXPECT_GE(inside.launch, 0) << "a press three steps into a six-step window was refused";
    EXPECT_GT(inside.rise, 0.5f);

    // Negative arm one: the window set to zero, the press unchanged.
    EXPECT_EQ(walkOffLedge(0, bare.left + 3).launch, -1);

    // Negative arm two: the window kept, the press twenty steps past it. A window that
    // never closed would pass the arm above and fail this one.
    const LedgeRun late = walkOffLedge(6, bare.left + 20);
    EXPECT_EQ(late.launch, -1);
    EXPECT_LT(late.rise, 0.01f);
}

/// The window is spent by the launch that used it, or a press held across the ledge would
/// buy a second jump out of the air -- which is a double jump, and C20 declined one.
TEST(PhysicsCharacter, TheCoyoteWindowIsSpentByTheJumpThatUsedIt) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);

    ColliderDesc c = capsule({0.0f, 0.1f, 0.0f});
    c.jumpSpeed = 5.0f;
    c.coyoteSteps = 30; // far longer than the launch takes to leave the ground
    const PhysicsCharacterId id = world.createCharacter(c);
    world.finalize();
    for (int i = 0; i < 30; ++i) world.step(kStep);

    int launches = 0;
    for (int i = 0; i < 40; ++i) {
        world.setCharacterInput(id, {}, true); // held down for the whole rise
        world.step(kStep);
        if (world.characterJumped(id)) ++launches;
    }
    EXPECT_EQ(launches, 1);
}

// -------------------------------------------------- steep ground, and steps (C20)

/**
 * A face past `maxSlopeAngle` is a third answer, and the bool could not give it.
 *
 * Sixty degrees against the collider's fifty. The character cannot stand on it, slides
 * down it, and is refused a jump -- all of which was already true. What was not is that
 * anything could tell this apart from mid-air, which is what made a game play a fall clip
 * for a character visibly in contact with a surface.
 */
TEST(PhysicsCharacter, SteepGroundIsItsOwnAnswerRatherThanMidAir) {
    PhysicsWorld world;
    world.init({}, 2);

    ColliderDesc slope = box({0.0f, 0.0f, 0.0f}, {6.0f, 0.5f, 6.0f}, ColliderMotion::Static);
    slope.name = "slope";
    slope.transform = glm::rotate(glm::mat4(1.0f), glm::radians(60.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    world.createBody(slope);

    // Just above the tilted top face, which passes through (0, 1, 0).
    ColliderDesc c = capsule({0.0f, 1.2f, 0.0f});
    const PhysicsCharacterId id = world.createCharacter(c);
    world.finalize();

    for (int i = 0; i < 30; ++i) world.step(kStep);

    EXPECT_EQ(world.characterGround(id), CharacterGround::Sliding);
    EXPECT_FALSE(world.characterOnGround(id));

    const glm::vec3 before(world.characterTransform(id, 0.0f)[3]);
    for (int i = 0; i < 60; ++i) {
        world.setCharacterInput(id, {}, true);
        world.step(kStep);
        EXPECT_FALSE(world.characterJumped(id)) << "a jump was launched off a 60-degree face";
    }
    const glm::vec3 after(world.characterTransform(id, 0.0f)[3]);
    // A face rotated +60 degrees about Z has its normal at (-sin, cos, 0), so downhill is
    // -X. The drop is the solver's, not this test's: a character that merely stood on the
    // slope would move on neither axis.
    EXPECT_LT(after.x - before.x, -0.2f);
    EXPECT_LT(after.y, before.y);
}

// ------------------------------------------------ what the ground is, not merely that it is (C30)

TEST(PhysicsCharacter, TheGroundIsABodyRatherThanAYesOrNo) {
    PhysicsWorld world;
    world.init({}, 4);
    addFloor(world);

    const glm::vec3 start(0.0f, 0.5f, 0.0f);
    const BodyId platform = world.createBody(box(start, {2.0f, 0.25f, 2.0f}, ColliderMotion::Kinematic));
    ASSERT_TRUE(platform.valid());
    const PhysicsCharacterId rider = world.createCharacter(capsule({0.0f, 0.8f, 0.0f}));
    world.finalize();
    for (int i = 0; i < 30; ++i) world.step(kStep);

    // **The platform, not the floor it sits on.** Both are `OnGround` to `characterGround`,
    // and a moving platform, a conveyor and an enemy's head are three different things to a
    // game that can only be told apart here.
    ASSERT_TRUE(world.characterOnGround(rider));
    EXPECT_TRUE(world.characterGroundBody(rider) == platform);
    EXPECT_NEAR(world.characterGroundNormal(rider).y, 1.0f, 1e-3f);

    // In the air there is no body, and the normal is straight up rather than the face the
    // character left -- Jolt keeps the last one it found, so this is filtered rather than
    // reported.
    world.setCharacterTransform(rider, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 8.0f, 0.0f)));
    world.step(kStep);
    ASSERT_FALSE(world.characterOnGround(rider));
    EXPECT_FALSE(world.characterGroundBody(rider).valid());
    EXPECT_NEAR(world.characterGroundNormal(rider).y, 1.0f, 1e-3f);
}

TEST(PhysicsCharacter, TheGroundNormalIsTheFaceRatherThanTheWorldUp) {
    PhysicsWorld world;
    world.init({}, 2);

    // Twenty degrees, which is inside the default `maxSlopeAngle`: the character stands on
    // it, so this is a normal a slope lean could be written against rather than the sliding
    // case `SteepGroundIsItsOwnAnswerRatherThanMidAir` already covers.
    constexpr float kTilt = 20.0f;
    ColliderDesc slope = box({0.0f, 0.0f, 0.0f}, {6.0f, 0.5f, 6.0f}, ColliderMotion::Static);
    slope.name = "slope";
    slope.transform = glm::rotate(glm::mat4(1.0f), glm::radians(kTilt), glm::vec3(0.0f, 0.0f, 1.0f));
    world.createBody(slope);

    const PhysicsCharacterId id = world.createCharacter(capsule({0.0f, 1.2f, 0.0f}));
    world.finalize();
    for (int i = 0; i < 60; ++i) world.step(kStep);

    ASSERT_TRUE(world.characterOnGround(id));
    // A face rotated +20 degrees about Z has its normal at (-sin, cos, 0). Reporting the
    // world up here would be a lean that never leans and a ski that never skis, and nothing
    // above this call could tell.
    const glm::vec3 n = world.characterGroundNormal(id);
    EXPECT_NEAR(n.x, -std::sin(glm::radians(kTilt)), 0.02f);
    EXPECT_NEAR(n.y, std::cos(glm::radians(kTilt)), 0.02f);
    EXPECT_NEAR(n.z, 0.0f, 0.02f);
}

namespace {

/// Walk into a riser of `riserTop` metres with `stepHeight` metres of step-up, and report
/// how high the character ended up.
float walkIntoAStep(float stepHeight, float riserTop) {
    PhysicsWorld world;
    world.init({}, 3);
    addFloor(world);
    world.createBody(box({0.0f, riserTop - 0.5f, 3.0f}, {2.0f, 0.5f, 2.0f}, ColliderMotion::Static));

    ColliderDesc c = capsule({0.0f, 0.1f, 0.0f});
    c.moveSpeed = 3.0f;
    c.stepHeight = stepHeight;
    const PhysicsCharacterId id = world.createCharacter(c);
    world.finalize();

    // Sixty steps, which puts the character a metre onto the riser's top face and stops
    // well short of walking off its far edge -- a run long enough to reach the other side
    // ends back at y = 0 and would report the climb as a failure.
    for (int i = 0; i < 60; ++i) {
        world.setCharacterInput(id, {0.0f, 0.0f, 1.0f}, false);
        world.step(kStep);
    }
    return world.characterTransform(id, 0.0f)[3].y;
}

} // namespace

/**
 * `stepHeight` was parsed out of the glTF extras, documented, and read by nothing at all:
 * the step handed every character a default `ExtendedUpdateSettings`, whose 0.4 metres of
 * step-up is an absolute number sitting two hundred lines from an `mSupportingVolume` the
 * same header scales to the capsule. So the authorable row was dead and the live one was
 * wrong for any character not roughly human-sized.
 *
 * Two arms differing in that one number, against a riser neither the capsule's radius nor
 * the contact solver can carry it over on its own.
 */
TEST(PhysicsCharacter, StepHeightIsWhatDecidesWhichStepsAreWalkedUp) {
    EXPECT_GT(walkIntoAStep(0.6f, 0.4f), 0.3f);
    EXPECT_LT(walkIntoAStep(0.05f, 0.4f), 0.1f);
}

TEST(PhysicsDebug, AnEmptyWorldDrawsNothing) {
    PhysicsWorld world;
    world.init({}, 0);
    world.finalize();
    std::vector<gfx::DebugLineVertex> lines;
    world.drawDebug(lines, {0.0f, 0.0f, 0.0f});
    EXPECT_TRUE(lines.empty());
}

TEST(PhysicsDebug, ABodyDrawsLinesInPairs) {
    PhysicsWorld world;
    world.init({}, 1);
    world.createBody(box({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, ColliderMotion::Dynamic));
    world.finalize();

    std::vector<gfx::DebugLineVertex> lines;
    world.drawDebug(lines, {0.0f, 0.0f, 10.0f});
    EXPECT_FALSE(lines.empty());
    // A line list, so anything odd means a vertex was emitted without its partner.
    EXPECT_EQ(lines.size() % 2u, 0u);
}

TEST(DebugLineColor, PacksToTheLayoutTheShaderReads) {
    EXPECT_EQ(gfx::packDebugColor({1.0f, 0.0f, 0.0f, 1.0f}), 0xFF0000FFu);
    EXPECT_EQ(gfx::packDebugColor({0.0f, 1.0f, 0.0f, 1.0f}), 0xFF00FF00u);
    EXPECT_EQ(gfx::packDebugColor({0.0f, 0.0f, 1.0f, 1.0f}), 0xFFFF0000u);
    // Clamped rather than wrapped: a caller that computed 1.2 meant white.
    EXPECT_EQ(gfx::packDebugColor({2.0f, 2.0f, 2.0f, 2.0f}), 0xFFFFFFFFu);
}

// D6. `addBody` returns kNoBody when the budget refuses a collider, and kNoBody is an
// ordinary uint32_t a caller stores and asks questions about later. Every indexed
// accessor therefore has to answer for it rather than index past the end of a vector.
TEST(PhysicsBounds, TheValueCreateBodyReturnsOnFailureIsSafeToAskAbout) {
    PhysicsWorld world;
    world.init({}, 1);
    ASSERT_TRUE(world.createBody(box({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, ColliderMotion::Dynamic)).valid());
    // A shape with no points is refused by `makeShape`, which is what a failed `createBody`
    // looks like now that the budget grows instead of turning callers away. The handle it
    // hands back is the subject here, not the reason it was handed back.
    ColliderDesc empty;
    empty.shape = ColliderShape::Hull;
    const BodyId refused = world.createBody(empty);
    ASSERT_FALSE(refused.valid()) << "nothing was refused, so this test proves nothing";
    world.finalize();

    EXPECT_EQ(world.bodyUserData(refused), 0u);
    EXPECT_FALSE(world.bodyMoves(refused));
    EXPECT_EQ(world.bodyTransform(refused, 0.5f), glm::mat4(1.0f));
}

TEST(PhysicsBounds, CharacterAccessorsAnswerForAnIndexThatIsNotACharacter) {
    PhysicsWorld world;
    world.init({}, 1);
    world.finalize();

    constexpr PhysicsCharacterId none{};
    EXPECT_EQ(world.characterUserData(none), 0u);
    EXPECT_EQ(world.characterTransform(none, 0.5f), glm::mat4(1.0f));
    EXPECT_FLOAT_EQ(world.characterSpeed(none), 0.0f);
    EXPECT_EQ(world.characterVelocity(none), glm::vec3(0.0f));
    EXPECT_FALSE(world.characterOnGround(none));
    world.setCharacterInput(none, {1.0f, 0.0f, 0.0f}, true); // must not write anything
}

// ============================================================== queries (C2)
//
// The surface `segmentBlocked` was the boolean half of. What these defend is that a hit
// reports the caller's body index rather than Jolt's, that a miss is falsy rather than a
// zeroed record, and that a truncated overlap says so.

TEST(PhysicsQueries, ARaycastReportsTheBodyPointNormalAndDistance) {
    PhysicsWorld world;
    world.init({}, 2);
    const BodyId wall = world.createBody(box({5.0f, 0.0f, 0.0f}, {1.0f, 5.0f, 5.0f}, ColliderMotion::Static));
    world.finalize();

    const PhysicsWorld::RayHit hit = world.raycast({0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f});
    ASSERT_TRUE(static_cast<bool>(hit));
    EXPECT_EQ(hit.body, wall);
    // The wall's near face is at x = 4.
    EXPECT_NEAR(hit.point.x, 4.0f, 1e-3f);
    EXPECT_NEAR(hit.distance, 4.0f, 1e-3f);
    // Pointing back out of the wall, towards where the ray came from.
    EXPECT_NEAR(hit.normal.x, -1.0f, 1e-3f);
}

TEST(PhysicsQueries, AMissIsFalsyAndCarriesNoBody) {
    PhysicsWorld world;
    world.init({}, 2);
    world.createBody(box({5.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, ColliderMotion::Static));
    world.finalize();

    // Parallel to the wall and well above it.
    const PhysicsWorld::RayHit hit = world.raycast({0.0f, 50.0f, 0.0f}, {10.0f, 50.0f, 0.0f});
    EXPECT_FALSE(static_cast<bool>(hit));
    EXPECT_FALSE(hit.body.valid());
    EXPECT_LT(hit.distance, 0.0f) << "a miss must be distinguishable from a hit at zero range";
}

TEST(PhysicsQueries, IgnoreBodyIsTheDifferenceBetweenTwoHits) {
    PhysicsWorld world;
    world.init({}, 3);
    const BodyId near = world.createBody(box({2.0f, 0.0f, 0.0f}, {0.5f, 5.0f, 5.0f}, ColliderMotion::Static));
    const BodyId far = world.createBody(box({6.0f, 0.0f, 0.0f}, {0.5f, 5.0f, 5.0f}, ColliderMotion::Static));
    world.finalize();

    EXPECT_EQ(world.raycast({0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}).body, near);
    EXPECT_EQ(world.raycast({0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, near).body, far);
}

TEST(PhysicsQueries, SegmentBlockedAgreesWithRaycast) {
    // It is now implemented over raycast, so this pins the two to each other rather than
    // testing the old implementation twice.
    PhysicsWorld world;
    world.init({}, 2);
    world.createBody(box({5.0f, 0.0f, 0.0f}, {1.0f, 5.0f, 5.0f}, ColliderMotion::Static));
    world.finalize();

    const glm::vec3 a{0.0f, 0.0f, 0.0f};
    const glm::vec3 through{10.0f, 0.0f, 0.0f};
    const glm::vec3 over{10.0f, 50.0f, 0.0f};

    EXPECT_TRUE(world.segmentBlocked(a, through));
    EXPECT_EQ(world.segmentBlocked(a, through), static_cast<bool>(world.raycast(a, through)));
    EXPECT_FALSE(world.segmentBlocked({0.0f, 50.0f, 0.0f}, over));
    EXPECT_EQ(world.segmentBlocked({0.0f, 50.0f, 0.0f}, over),
              static_cast<bool>(world.raycast({0.0f, 50.0f, 0.0f}, over)));
}

TEST(PhysicsQueries, AZeroLengthSegmentIsAMissRatherThanAnAssertion) {
    PhysicsWorld world;
    world.init({}, 2);
    addFloor(world);
    world.finalize();

    EXPECT_FALSE(static_cast<bool>(world.raycast({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f})));
    EXPECT_FALSE(world.segmentBlocked({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));
}

TEST(PhysicsQueries, AnEmptyWorldAnswersEveryQueryWithoutBeingInitialised) {
    PhysicsWorld world;
    BodyId hits[4];
    EXPECT_FALSE(static_cast<bool>(world.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f})));
    EXPECT_FALSE(static_cast<bool>(world.sphereCast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0.5f)));
    EXPECT_EQ(world.overlapSphere({0.0f, 0.0f, 0.0f}, 1.0f, hits), 0u);
}

TEST(PhysicsQueries, ASphereCastHitsWhatItSweepsInto) {
    PhysicsWorld world;
    world.init({}, 2);
    const BodyId wall = world.createBody(box({5.0f, 0.0f, 0.0f}, {1.0f, 5.0f, 5.0f}, ColliderMotion::Static));
    world.finalize();

    const PhysicsWorld::RayHit hit = world.sphereCast({0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, 0.5f);
    ASSERT_TRUE(static_cast<bool>(hit));
    EXPECT_EQ(hit.body, wall);
    // A sphere of radius 0.5 stops half a metre short of where a ray would have stopped.
    EXPECT_NEAR(hit.distance, 3.5f, 1e-2f);
    EXPECT_NEAR(hit.normal.x, -1.0f, 1e-2f);
}

TEST(PhysicsQueries, ASphereCastCatchesWhatAThinRayPassesThrough) {
    // The reason the query exists: a zero-width ray fits through a gap a character does
    // not. Two walls with a 0.2 m slot between them, and a ray aimed straight down it.
    PhysicsWorld world;
    world.init({}, 3);
    world.createBody(box({5.0f, 1.1f, 0.0f}, {1.0f, 1.0f, 5.0f}, ColliderMotion::Static));
    world.createBody(box({5.0f, -1.1f, 0.0f}, {1.0f, 1.0f, 5.0f}, ColliderMotion::Static));
    world.finalize();

    EXPECT_FALSE(static_cast<bool>(world.raycast({0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f})))
        << "the slot is not open, so this test proves nothing";
    EXPECT_TRUE(static_cast<bool>(world.sphereCast({0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, 0.5f)));
}

TEST(PhysicsQueries, OverlapSphereReportsWhatItWantedNotWhatItWrote) {
    PhysicsWorld world;
    world.init({}, 8);
    for (int i = 0; i < 4; ++i) {
        world.createBody(box({static_cast<float>(i) * 0.5f, 0.0f, 0.0f}, {0.2f, 0.2f, 0.2f}, ColliderMotion::Static));
    }
    world.finalize();

    BodyId all[8];
    const uint32_t found = world.overlapSphere({0.75f, 0.0f, 0.0f}, 5.0f, all);
    ASSERT_EQ(found, 4u);

    // The same query into storage too small for it: the count is unchanged, and exactly
    // as many indices are written as there was room for. A caller comparing the two knows
    // it was truncated, which is the whole contract.
    BodyId two[2];
    EXPECT_EQ(world.overlapSphere({0.75f, 0.0f, 0.0f}, 5.0f, two), 4u);
    EXPECT_EQ(two[0], all[0]);
    EXPECT_EQ(two[1], all[1]);

    // And nothing at all, which must not write through a zero-length span.
    EXPECT_EQ(world.overlapSphere({0.75f, 0.0f, 0.0f}, 5.0f, {}), 4u);
}

// ================================================= pause and time scale (C4)

TEST(FixedClockTimeScale, DefaultsToOneAndChangesNothing) {
    // The property the golden set rests on: at the default scale a locked clock is the
    // clock it was before C4, to the bit.
    FixedClock scaled;
    FixedClock plain;
    ASSERT_FLOAT_EQ(scaled.timeScale(), 1.0f);

    for (int i = 0; i < 10; ++i) {
        scaled.accumulate(scaled.step());
        plain.accumulate(plain.step());
        EXPECT_EQ(scaled.consume(), plain.consume());
        EXPECT_FLOAT_EQ(scaled.alpha(), plain.alpha());
    }
    EXPECT_EQ(scaled.stepCount(), plain.stepCount());
}

TEST(FixedClockTimeScale, ZeroStopsTheSimulationWithoutStoppingTheFrame) {
    FixedClock clock;
    clock.setTimeScale(0.0f);
    EXPECT_TRUE(clock.paused());

    // A hundred frames of real time arrive and none of them become a step.
    for (int i = 0; i < 100; ++i) {
        clock.accumulate(clock.step());
        EXPECT_FALSE(clock.consume());
    }
    EXPECT_EQ(clock.stepCount(), 0u);
    EXPECT_EQ(clock.droppedSteps(), 0u) << "a pause is not time the simulation lost";
}

TEST(FixedClockTimeScale, ResumingPicksUpWhereItLeftOff) {
    FixedClock clock;
    clock.accumulate(clock.step());
    EXPECT_TRUE(clock.consume());

    clock.setTimeScale(0.0f);
    for (int i = 0; i < 5; ++i) {
        clock.accumulate(clock.step());
        EXPECT_FALSE(clock.consume());
    }

    clock.setTimeScale(1.0f);
    clock.accumulate(clock.step());
    EXPECT_TRUE(clock.consume());
    EXPECT_EQ(clock.stepCount(), 2u) << "the paused frames must not be owed back";
}

TEST(FixedClockTimeScale, HalfSpeedTakesTwiceAsManyFramesPerStep) {
    FixedClock clock;
    clock.setTimeScale(0.5f);
    EXPECT_FALSE(clock.paused());

    uint32_t steps = 0;
    for (int i = 0; i < 20; ++i) {
        clock.accumulate(clock.step());
        while (clock.consume()) ++steps;
    }
    EXPECT_EQ(steps, 10u);
}

TEST(FixedClockTimeScale, ANegativeScaleClampsToPausedRatherThanRunningBackwards) {
    FixedClock clock;
    clock.setTimeScale(-2.0f);
    EXPECT_FLOAT_EQ(clock.timeScale(), 0.0f);
    EXPECT_TRUE(clock.paused());

    clock.accumulate(clock.step());
    EXPECT_FALSE(clock.consume());
}

// ========================================== lifetimes: create and destroy (C1)
//
// What the row exists for: before it, nothing in this class could be taken away. These
// defend the three properties that make a handle safer than the bare index it replaced.

TEST(PhysicsLifetime, DestroyingABodyMakesTheHandleStale) {
    PhysicsWorld world;
    world.init({}, 4);
    const BodyId body = world.createBody(box({0.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic), 42u);
    world.finalize();

    ASSERT_TRUE(world.valid(body));
    EXPECT_EQ(world.bodyUserData(body), 42u);

    world.destroy(body);

    EXPECT_TRUE(body.valid()) << "it was issued; that does not stop being true";
    EXPECT_FALSE(world.valid(body)) << "but it no longer names a live body";
    // Every accessor answers for it rather than reading a slot that is being torn down.
    EXPECT_EQ(world.bodyUserData(body), 0u);
    EXPECT_FALSE(world.bodyMoves(body));
    EXPECT_EQ(world.bodyTransform(body, 0.5f), glm::mat4(1.0f));
}

TEST(PhysicsLifetime, DestroyIsIdempotent) {
    PhysicsWorld world;
    world.init({}, 4);
    const BodyId body = world.createBody(box({0.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    world.finalize();

    world.destroy(body);
    world.destroy(body); // must not queue the Jolt body for removal twice
    world.destroy(BodyId{});
    world.step(kStep);
    SUCCEED();
}

TEST(PhysicsLifetime, AReusedSlotDoesNotAliasTheHandleThatHeldIt) {
    // The whole point of the generation counter. Without it the second create hands back
    // a value equal to the first, and the stale holder silently drives a different body.
    PhysicsWorld world;
    world.init({}, 4);
    const BodyId first = world.createBody(box({0.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic), 1u);
    world.finalize();

    world.destroy(first);
    world.step(kStep); // the reclaim boundary: only now may the slot be handed out

    const BodyId second = world.createBody(box({3.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic), 2u);
    ASSERT_TRUE(second.valid());
    EXPECT_EQ(second.index, first.index) << "the slot should have been reused, or this proves nothing";
    EXPECT_NE(second.generation, first.generation);

    EXPECT_TRUE(world.valid(second));
    EXPECT_FALSE(world.valid(first));
    EXPECT_EQ(world.bodyUserData(second), 2u);
    EXPECT_EQ(world.bodyUserData(first), 0u) << "the stale handle must not read the new body";
}

TEST(PhysicsLifetime, ASlotIsNotReusedBeforeTheStepBoundary) {
    // Jolt cannot have a body removed from under a step, so destroy() only queues. The
    // observable consequence is that the slot count does not drop until step() runs.
    PhysicsWorld world;
    world.init({}, 4);
    const BodyId body = world.createBody(box({0.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    world.finalize();
    const uint32_t before = world.bodyCount();

    world.destroy(body);
    const BodyId other = world.createBody(box({9.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    EXPECT_NE(other.index, body.index) << "the retired slot was handed out while Jolt still held it";
    EXPECT_EQ(world.bodyCount(), before + 1u);
}

TEST(PhysicsLifetime, ACharacterCanBeDestroyedToo) {
    PhysicsWorld world;
    world.init({}, 4);
    ColliderDesc desc = box({0.0f, 1.0f, 0.0f}, {0.3f, 0.9f, 0.3f}, ColliderMotion::Character);
    desc.radius = 0.3f;
    const PhysicsCharacterId character = world.createCharacter(desc, 7u);
    world.finalize();

    ASSERT_TRUE(world.valid(character));
    EXPECT_EQ(world.characterUserData(character), 7u);

    world.destroy(character);
    EXPECT_FALSE(world.valid(character));
    EXPECT_EQ(world.characterUserData(character), 0u);
    EXPECT_FLOAT_EQ(world.characterSpeed(character), 0.0f);
    EXPECT_FALSE(world.characterOnGround(character));

    world.step(kStep); // must not step a character that is gone
    SUCCEED();
}

TEST(PhysicsLifetime, CreateBodyRefusesACharacterRatherThanRoutingIt) {
    // The one behaviour C1 changed on purpose: addBody() used to route a Character motion
    // to addCharacter() and hand back an index the caller could not tell apart.
    PhysicsWorld world;
    world.init({}, 4);
    ColliderDesc desc = box({0.0f, 1.0f, 0.0f}, {0.3f, 0.9f, 0.3f}, ColliderMotion::Character);
    desc.radius = 0.3f;

    EXPECT_FALSE(world.createBody(desc).valid());
    EXPECT_EQ(world.characterCount(), 0u) << "it must not have been routed";
    EXPECT_TRUE(world.createCharacter(desc).valid());
}

TEST(PhysicsLifetime, ASpawnDespawnCycleDoesNotGrowTheWorld) {
    // "And the memory goes back", which is the verification note the roadmap attaches to
    // every lifetime row. Slot reuse is what makes it true.
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    world.finalize();
    const uint32_t base = world.bodyCount();

    for (int i = 0; i < 200; ++i) {
        const BodyId b = world.createBody(box({0.0f, 5.0f, 0.0f}, {0.4f, 0.4f, 0.4f}, ColliderMotion::Dynamic));
        ASSERT_TRUE(b.valid()) << "ran out of slots on cycle " << i << ", so nothing is being reused";
        world.destroy(b);
        world.step(kStep);
    }
    EXPECT_EQ(world.bodyCount(), base + 1u) << "one slot recycled 200 times, not 200 slots";
}

// ================================================ contacts (G7)
//
// The stream a game acts on. Four properties carry it, and each one is a way the obvious
// implementation is wrong:
//
// 1. **A contact names both bodies in the caller's terms**, with the impact it arrived
//    with -- not Jolt's `BodyID`, and not a velocity read after the solver flattened it.
// 2. **It is ordered by the scene rather than by the thread that found it.** A game whose
//    state depends on the order of the events it is handed inherits the determinism this
//    class defends everywhere else, or it does not have it at all.
// 3. **The window is the gap between two steps**, so a game may destroy a body it was just
//    told about without the rest of the list turning into handles onto something else.
// 4. **Only new contacts are events.** A settled stack is not sixty impacts a second.

namespace {

/// Drop a box from `height` onto a floor and run until it has landed, returning the
/// contacts of the step it landed on. Empty when it never touched, which is a failure the
/// caller should assert about rather than one this helper should hide.
std::vector<PhysicsWorld::Contact> stepUntilContact(PhysicsWorld& world, uint32_t maxSteps = 240) {
    for (uint32_t i = 0; i < maxSteps; ++i) {
        world.step(kStep);
        if (!world.contacts().empty()) {
            return {world.contacts().begin(), world.contacts().end()};
        }
    }
    return {};
}

} // namespace

TEST(PhysicsContacts, ALandingBoxIsReportedOnceWithTheSpeedItArrivedAt) {
    PhysicsWorld world;
    world.init({}, 4);
    addFloor(world);
    const BodyId crate = world.createBody(box({0.0f, 2.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    world.finalize();

    const std::vector<PhysicsWorld::Contact> landed = stepUntilContact(world);
    ASSERT_FALSE(landed.empty()) << "a box dropped on a floor has to touch it";

    const PhysicsWorld::Contact& c = landed.front();
    // The floor was created first, so it is the lower slot and therefore `a`.
    EXPECT_EQ(c.a.index, 0u);
    EXPECT_EQ(c.b.index, crate.index);
    EXPECT_TRUE(world.valid(c.a));
    EXPECT_TRUE(world.valid(c.b));

    // Out of `a`, which is the floor, so up.
    EXPECT_GT(c.normal.y, 0.9f);
    // It fell roughly 1 m from rest, so it arrived at about sqrt(2*9.81*1) = 4.4 m/s. The
    // bound is loose on purpose: what is being pinned is that this is the *approach*
    // speed and not the post-solve rebound, which would be a fraction of a metre a second.
    EXPECT_GT(c.speed, 3.0f);
    EXPECT_LT(c.speed, 6.0f);
    // Where the box's underside met the floor.
    EXPECT_NEAR(c.point.y, 0.0f, 0.2f);
    EXPECT_NEAR(c.point.x, 0.0f, 0.6f);
}

TEST(PhysicsContacts, AStreamIsClearedByTheNextStepRatherThanAccumulating) {
    PhysicsWorld world;
    world.init({}, 4);
    addFloor(world);
    world.createBody(box({0.0f, 1.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    world.finalize();

    ASSERT_FALSE(stepUntilContact(world).empty());

    // A settled box keeps *touching* the floor, and a persisting contact is not an event.
    // Without that distinction this loop would report a contact on every one of 120 steps
    // and a game playing a sound on each would produce a buzz.
    uint32_t stepsWithContacts = 0;
    for (int i = 0; i < 120; ++i) {
        world.step(kStep);
        if (!world.contacts().empty()) ++stepsWithContacts;
    }
    EXPECT_LT(stepsWithContacts, 20u) << "a settling box may bounce a few times; it may not report every step";
}

TEST(PhysicsContacts, TheStreamIsOrderedByTheSceneRatherThanByDiscoveryOrder) {
    // Five boxes at the same height land on the same step. Which one Jolt's broad phase
    // finds first is not a property anything should depend on, so the list is sorted by
    // the slots the caller was handed.
    PhysicsWorld world;
    world.init({}, 16);
    addFloor(world);
    for (int i = 0; i < 5; ++i) {
        world.createBody(box({static_cast<float>(i) * 3.0f, 1.4f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    }
    world.finalize();

    const std::vector<PhysicsWorld::Contact> landed = stepUntilContact(world);
    ASSERT_GE(landed.size(), 2u) << "boxes released together should land together";

    for (size_t i = 1; i < landed.size(); ++i) {
        const bool ordered = landed[i - 1].a.index < landed[i].a.index ||
                             (landed[i - 1].a.index == landed[i].a.index && landed[i - 1].b.index <= landed[i].b.index);
        EXPECT_TRUE(ordered) << "contact " << i << " came before contact " << i - 1 << " in slot order";
        // And canonical: `a` is always the lower slot, so a caller matching a pair writes
        // one test rather than two.
        EXPECT_LT(landed[i].a.index, landed[i].b.index);
    }
}

TEST(PhysicsContacts, TwoIdenticalWorldsProduceIdenticalContacts) {
    const auto build = [](PhysicsWorld& world) {
        world.init({}, 16);
        addFloor(world);
        for (int i = 0; i < 4; ++i) {
            world.createBody(box({static_cast<float>(i) * 1.1f - 1.6f, 1.0f + static_cast<float>(i) * 0.4f, 0.0f},
                                 {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
        }
        world.finalize();
    };

    PhysicsWorld a;
    PhysicsWorld b;
    build(a);
    build(b);

    uint32_t total = 0;
    for (int step = 0; step < 200; ++step) {
        a.step(kStep);
        b.step(kStep);
        ASSERT_EQ(a.contacts().size(), b.contacts().size()) << "step " << step;
        for (size_t i = 0; i < a.contacts().size(); ++i) {
            EXPECT_EQ(a.contacts()[i].a.index, b.contacts()[i].a.index);
            EXPECT_EQ(a.contacts()[i].b.index, b.contacts()[i].b.index);
            EXPECT_EQ(a.contacts()[i].point, b.contacts()[i].point);
            EXPECT_EQ(a.contacts()[i].speed, b.contacts()[i].speed);
        }
        total += static_cast<uint32_t>(a.contacts().size());
    }
    EXPECT_GT(total, 0u) << "nothing collided, so this proved nothing";
}

TEST(PhysicsContacts, ABodyDestroyedWhileTheListIsBeingWalkedStopsValidating) {
    // The pattern the row exists for: a pickup that deletes itself the moment something
    // touches it. Every contact after that one still names it, and every one of those
    // handles has to be detectably stale rather than an alias onto whatever comes next.
    PhysicsWorld world;
    world.init({}, 16);
    addFloor(world);
    std::vector<BodyId> crates;
    for (int i = 0; i < 4; ++i) {
        crates.push_back(
            world.createBody(box({static_cast<float>(i) * 3.0f, 1.4f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic)));
    }
    world.finalize();

    const std::vector<PhysicsWorld::Contact> landed = stepUntilContact(world);
    ASSERT_GE(landed.size(), 2u);

    // Destroy on the first contact, then keep reading. This is a caller draining the list
    // it was handed, which is exactly what a game does.
    const BodyId first = landed.front().b;
    ASSERT_TRUE(world.valid(first));
    world.destroy(first);

    uint32_t namingTheDead = 0;
    for (const PhysicsWorld::Contact& c : landed) {
        if (c.b.index == first.index || c.a.index == first.index) {
            ++namingTheDead;
            EXPECT_FALSE(world.valid(c.b.index == first.index ? c.b : c.a))
                << "a contact naming a destroyed body must not still validate";
        }
    }
    EXPECT_GE(namingTheDead, 1u);

    // And the slot cannot be handed out until the step boundary, so nothing in that list
    // could have started naming a different body while it was being read.
    const BodyId spawned = world.createBody(box({20.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    EXPECT_NE(spawned.index, first.index);

    world.step(kStep);
    EXPECT_TRUE(world.contacts().empty() || world.contacts().front().a.valid());
}

TEST(PhysicsContacts, AWorldWithNothingInItReportsNothing) {
    PhysicsWorld world;
    world.init({}, 4);
    world.finalize();
    world.step(kStep);
    EXPECT_TRUE(world.contacts().empty());

    // And a world that was never brought up at all answers the same, rather than crashing
    // on a null Impl -- the same do-nothing answer every other accessor here gives.
    PhysicsWorld unbuilt;
    EXPECT_TRUE(unbuilt.contacts().empty());
}

TEST(PhysicsContacts, AThreadPoolStillHandsOverAnOrderedStream) {
    // The reason the list is sorted at all, and the only claim that can honestly be made
    // across thread counts: Jolt reproduces a run for a *fixed* count, so a four-thread
    // world is not expected to agree step for step with a single-threaded one. What must
    // hold either way is that the events arrive in the scene's order rather than in
    // whichever order the jobs happened to finish -- and this is also the arm in which the
    // recorder's lock is contended, which is what ThreadSanitizer is looking at here.
    PhysicsConfig cfg;
    cfg.workerThreads = 4;

    PhysicsWorld world;
    world.init(cfg, 32);
    addFloor(world);
    for (int i = 0; i < 12; ++i) {
        world.createBody(box({static_cast<float>(i % 4) * 1.6f - 2.4f, 1.2f + static_cast<float>(i / 4) * 1.3f, 0.0f},
                             {0.5f, 0.5f, 0.5f}, ColliderMotion::Dynamic));
    }
    world.finalize();

    uint32_t total = 0;
    for (int step = 0; step < 200; ++step) {
        world.step(kStep);
        const std::span<const PhysicsWorld::Contact> contacts = world.contacts();
        total += static_cast<uint32_t>(contacts.size());
        for (size_t i = 0; i < contacts.size(); ++i) {
            EXPECT_LT(contacts[i].a.index, contacts[i].b.index) << "the pair was not canonicalised";
            if (i == 0) continue;
            const bool ordered = contacts[i - 1].a.index < contacts[i].a.index ||
                                 (contacts[i - 1].a.index == contacts[i].a.index &&
                                  contacts[i - 1].b.index <= contacts[i].b.index);
            EXPECT_TRUE(ordered) << "step " << step << ", contact " << i;
        }
    }
    EXPECT_GT(total, 10u) << "nothing collided, so this proved nothing";
}

// ================================================== motion and freedom (P7)
//
// The half of this class a game reaches for first and which, until P7, did not exist: a
// body could be made, unmade, asked about and -- if it was kinematic -- placed, and nothing
// in the engine could push one.
//
// The constraint half is checked by behaviour rather than by asking whether a flag was set,
// and that is the whole point of testing it here rather than by eye. A 2D game is a 3D
// world with an axis taken away; what has to be true is that the axis *stays* taken away
// over hundreds of steps, through gravity, contacts and impulses that push along it -- and
// the control arm below drifts off the plane under exactly the same treatment, which is
// what makes the first test a check rather than a tautology.

namespace {

/// A 1 m cube with a stated mass, so an impulse and the velocity it produces are a division
/// a reader can do in their head instead of Jolt's density times a volume.
ColliderDesc crate(const glm::vec3& position, ColliderMotion motion,
                   ColliderFreedom freedom = ColliderFreedom::All) {
    ColliderDesc c = box(position, {0.5f, 0.5f, 0.5f}, motion);
    c.mass = 10.0f;
    c.freedom = freedom;
    return c;
}

/// Where the body is now. `alpha` of exactly 1 is the state the last step ended at, and
/// `mix(x, y, 1)` is `y` exactly -- which is what lets the plane below be an equality.
glm::vec3 positionOf(const PhysicsWorld& world, BodyId id) {
    return glm::vec3(world.bodyTransform(id, 1.0f)[3]);
}

} // namespace

TEST(PhysicsMotion, AnImpulseIsMassTimesTheVelocityItProduces) {
    PhysicsWorld world;
    world.init({}, 4);
    const BodyId body = world.createBody(crate({0.0f, 5.0f, 0.0f}, ColliderMotion::Dynamic));
    world.finalize();

    ASSERT_TRUE(body.valid());
    EXPECT_EQ(world.linearVelocity(body), glm::vec3(0.0f));

    // 50 kg m/s into 10 kg. Read before any step, so damping and gravity have not touched
    // it and the arithmetic is exactly the one the doc comment claims.
    world.addImpulse(body, {50.0f, 0.0f, 0.0f});
    EXPECT_FLOAT_EQ(world.linearVelocity(body).x, 5.0f);
    EXPECT_FLOAT_EQ(world.linearVelocity(body).y, 0.0f);

    // And it accumulates rather than replacing.
    world.addImpulse(body, {50.0f, 0.0f, 0.0f});
    EXPECT_FLOAT_EQ(world.linearVelocity(body).x, 10.0f);

    const float before = positionOf(world, body).x;
    for (int i = 0; i < 30; ++i) world.step(kStep);
    EXPECT_GT(positionOf(world, body).x, before + 3.0f) << "the push did not reach the solver";
}

TEST(PhysicsMotion, SettingAVelocityReplacesWhateverTheBodyHad) {
    PhysicsWorld world;
    world.init({}, 4);
    const BodyId body = world.createBody(crate({0.0f, 5.0f, 0.0f}, ColliderMotion::Dynamic));
    world.finalize();

    world.addImpulse(body, {100.0f, 0.0f, 0.0f});
    world.setLinearVelocity(body, {0.0f, 0.0f, 2.0f});
    EXPECT_EQ(world.linearVelocity(body), glm::vec3(0.0f, 0.0f, 2.0f)) << "it accumulated instead of replacing";

    // The stop, which is the call `setBodyTransform` deliberately does not make for you.
    world.setLinearVelocity(body, {});
    EXPECT_EQ(world.linearVelocity(body), glm::vec3(0.0f));
}

TEST(PhysicsMotion, AKinematicBodyTakesAVelocityAndRefusesAnImpulse) {
    // The two are not the same refusal. An impulse divided by an infinite mass is nothing,
    // so a kinematic body cannot be pushed; a velocity is exactly how one is driven, and it
    // is what makes a lift carry what stands on it.
    PhysicsWorld world;
    world.init({}, 4);
    const BodyId lift = world.createBody(crate({0.0f, 1.0f, 0.0f}, ColliderMotion::Kinematic));
    world.finalize();

    world.addImpulse(lift, {500.0f, 0.0f, 0.0f});
    EXPECT_EQ(world.linearVelocity(lift), glm::vec3(0.0f)) << "an impulse moved a kinematic body";

    world.setLinearVelocity(lift, {0.0f, 1.0f, 0.0f});
    const float before = positionOf(world, lift).y;
    for (int i = 0; i < 60; ++i) world.step(kStep);
    EXPECT_NEAR(positionOf(world, lift).y, before + 1.0f, 0.05f) << "a second of travel at 1 m/s";
}

TEST(PhysicsMotion, AStaticBodyAndAStaleHandleMoveNothing) {
    PhysicsWorld world;
    world.init({}, 4);
    const BodyId floor = world.createBody(box({0.0f, -0.5f, 0.0f}, {5.0f, 0.5f, 5.0f}, ColliderMotion::Static));
    const BodyId gone = world.createBody(crate({0.0f, 5.0f, 0.0f}, ColliderMotion::Dynamic));
    world.finalize();

    const glm::vec3 floorAt = positionOf(world, floor);
    world.addImpulse(floor, {500.0f, 0.0f, 0.0f});
    world.setLinearVelocity(floor, {5.0f, 0.0f, 0.0f});
    world.setBodyTransform(floor, glm::translate(glm::mat4(1.0f), {9.0f, 9.0f, 9.0f}));
    world.step(kStep);
    EXPECT_EQ(positionOf(world, floor), floorAt) << "the static body was moved by one of the three";
    EXPECT_EQ(world.linearVelocity(floor), glm::vec3(0.0f));

    world.destroy(gone);
    world.addImpulse(gone, {500.0f, 0.0f, 0.0f});
    world.setLinearVelocity(gone, {5.0f, 0.0f, 0.0f});
    world.setBodyTransform(gone, glm::mat4(1.0f));
    EXPECT_EQ(world.linearVelocity(gone), glm::vec3(0.0f));

    // And a world that was never brought up answers the same rather than reaching a null
    // Impl, which is the contract every other accessor here already keeps.
    PhysicsWorld unbuilt;
    unbuilt.addImpulse(BodyId{}, {1.0f, 0.0f, 0.0f});
    unbuilt.setLinearVelocity(BodyId{}, {1.0f, 0.0f, 0.0f});
    unbuilt.setBodyTransform(BodyId{}, glm::mat4(1.0f));
    EXPECT_EQ(unbuilt.linearVelocity(BodyId{}), glm::vec3(0.0f));
}

TEST(PhysicsMotion, ADynamicBodyCanBePlacedAndKeepsTheVelocityItHad) {
    // P7 widened `setBodyTransform` from kinematic-only to anything the solver moves. What
    // the widening must not do is invent a policy about velocity: a teleport that zeroed it
    // would make a portal impossible, and stopping the body is one more call.
    PhysicsWorld world;
    world.init({}, 4);
    const BodyId body = world.createBody(crate({0.0f, 5.0f, 0.0f}, ColliderMotion::Dynamic));
    world.finalize();

    world.setLinearVelocity(body, {3.0f, 0.0f, 0.0f});
    world.setBodyTransform(body, glm::translate(glm::mat4(1.0f), {-20.0f, 5.0f, 0.0f}));

    const glm::vec3 at = positionOf(world, body);
    EXPECT_FLOAT_EQ(at.x, -20.0f);
    EXPECT_FLOAT_EQ(at.y, 5.0f);
    EXPECT_FLOAT_EQ(world.linearVelocity(body).x, 3.0f) << "the teleport took a decision that is the caller's";

    // Both snapshots, so the frame it was placed on does not smear it back from where it
    // came -- the property G3 wrote this method for, now asked of a dynamic body.
    EXPECT_EQ(glm::vec3(world.bodyTransform(body, 0.0f)[3]), at);
}

// ---------------------------------------------------------------- the plane

TEST(PhysicsFreedom, APlanarBodyNeverLeavesItsPlane) {
    // The headline check, and it is deliberately hostile: gravity pulls it down, a floor
    // hits it back, and every thirtieth step it is pushed hard along the axis it is not
    // allowed to move on. Three hundred steps of that must leave the plane coordinate
    // *exactly* where it started, because the constraint is the solver's inverse mass and
    // not a correction applied afterwards -- an implementation that clamped the position
    // each step would pass a tolerance and fail an equality.
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    const BodyId body = world.createBody(crate({0.0f, 3.0f, 3.0f}, ColliderMotion::Dynamic, ColliderFreedom::Plane2D));
    world.finalize();
    ASSERT_TRUE(body.valid());

    const glm::vec3 start = positionOf(world, body);
    ASSERT_FLOAT_EQ(start.z, 3.0f);

    for (int i = 0; i < 300; ++i) {
        if (i % 30 == 0) world.addImpulse(body, {40.0f, 20.0f, 90.0f});
        world.step(kStep);
        ASSERT_EQ(positionOf(world, body).z, start.z) << "left the plane on step " << i;
        ASSERT_EQ(world.linearVelocity(body).z, 0.0f) << "gained velocity off the plane on step " << i;
    }

    // Rotation is confined too: the only spin allowed is about the plane's normal, so the
    // body's local Z axis is still the world's. Columns are glm's, so `m[2]` is where local
    // Z ended up and the third row of the other two is what would tilt it.
    const glm::mat4 m = world.bodyTransform(body, 1.0f);
    EXPECT_NEAR(m[2][0], 0.0f, 1e-5f);
    EXPECT_NEAR(m[2][1], 0.0f, 1e-5f);
    EXPECT_NEAR(m[2][2], 1.0f, 1e-5f);
    EXPECT_NEAR(m[0][2], 0.0f, 1e-5f);
    EXPECT_NEAR(m[1][2], 0.0f, 1e-5f);

    // And it did move in the plane, or the constraint proved nothing by holding a body that
    // was never going anywhere.
    EXPECT_GT(std::abs(positionOf(world, body).x - start.x), 1.0f);
}

TEST(PhysicsFreedom, TheSameBodyWithoutTheConstraintDoesLeaveThePlane) {
    // The control arm. Identical world, identical impulses, `freedom` left at its default:
    // if this one also stayed on the plane, the test above would be measuring nothing.
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    const BodyId body = world.createBody(crate({0.0f, 3.0f, 3.0f}, ColliderMotion::Dynamic));
    world.finalize();

    for (int i = 0; i < 300; ++i) {
        if (i % 30 == 0) world.addImpulse(body, {40.0f, 20.0f, 90.0f});
        world.step(kStep);
    }
    EXPECT_GT(positionOf(world, body).z, 4.0f) << "an unconstrained body did not drift, so the plane test is vacuous";
}

TEST(PhysicsFreedom, APlanarBodyStillCollidesAndSettles) {
    // Taking two axes away must not take the simulation with them. A confined box dropped
    // on a floor falls, lands, and comes to rest on top of it -- the same behaviour the 3D
    // world already had, which is the whole argument for constraining Jolt rather than
    // introducing a second solver beside it.
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    const BodyId body = world.createBody(crate({0.0f, 4.0f, 0.0f}, ColliderMotion::Dynamic, ColliderFreedom::Plane2D));
    world.finalize();

    bool touched = false;
    for (int i = 0; i < 240; ++i) {
        world.step(kStep);
        if (!world.contacts().empty()) touched = true;
    }
    EXPECT_TRUE(touched) << "a confined box never reported hitting the floor";
    EXPECT_NEAR(positionOf(world, body).y, 0.5f, 0.05f) << "it did not come to rest on the floor";
    EXPECT_LT(std::abs(world.linearVelocity(body).y), 0.1f);
}

TEST(PhysicsFreedom, TwoIdenticalWorldsUnderIdenticalImpulsesAgree) {
    // Determinism is this class's standing claim and a new way to change a body is a new
    // way to lose it. Bit for bit, like the contact stream's own determinism test.
    const auto run = [](std::vector<glm::vec3>& out) {
        PhysicsWorld world;
        world.init({}, 8);
        addFloor(world);
        const BodyId a = world.createBody(crate({-1.0f, 3.0f, 0.0f}, ColliderMotion::Dynamic, ColliderFreedom::Plane2D));
        const BodyId b = world.createBody(crate({1.0f, 3.0f, 0.0f}, ColliderMotion::Dynamic, ColliderFreedom::Plane2D));
        world.finalize();
        for (int i = 0; i < 120; ++i) {
            if (i % 20 == 0) {
                world.addImpulse(a, {30.0f, 10.0f, 0.0f});
                world.addImpulse(b, {-30.0f, 10.0f, 0.0f});
            }
            world.step(kStep);
            out.push_back(positionOf(world, a));
            out.push_back(positionOf(world, b));
        }
    };

    std::vector<glm::vec3> first;
    std::vector<glm::vec3> second;
    run(first);
    run(second);
    ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i) EXPECT_EQ(first[i], second[i]) << "diverged at sample " << i;
}

/**
 * A rider on a kinematic platform moves with it.
 *
 * `CharacterVirtual` does not carry itself: `PhysicsWorld::step` adds
 * `GetGroundVelocity()` to whatever the controller asked for, and that reads the velocity
 * of the body under the character's feet. A kinematic body written with
 * `SetPositionAndRotation` has none -- it teleports, arriving with the velocity it already
 * had, which is zero for ever -- so the platform slid out from under its rider and the
 * rider stood still. `setBodyTransform` uses `MoveKinematic` for exactly this.
 *
 * Asserted as a fraction of the platform's travel rather than as a position, because the
 * rider is not glued: friction and the one-step lag between asking and arriving mean it
 * tracks closely rather than exactly, and pinning the exact offset would be pinning Jolt's
 * solver rather than the behaviour.
 */
TEST(PhysicsCharacter, RidesAKinematicPlatformInsteadOfStandingStillWhileItLeaves) {
    PhysicsWorld world;
    world.init({}, 4);
    addFloor(world);

    // Waist-high, so the rider stands on the platform and not on the floor beside it.
    const glm::vec3 start(0.0f, 0.5f, 0.0f);
    const glm::vec3 half(2.0f, 0.25f, 2.0f);
    const BodyId platform = world.createBody(box(start, half, ColliderMotion::Kinematic));
    ASSERT_TRUE(platform.valid());

    ColliderDesc c = capsule({0.0f, start.y + half.y, 0.0f});
    const PhysicsCharacterId rider = world.createCharacter(c);
    ASSERT_TRUE(rider.valid());
    world.finalize();

    // Settle, so the ground state is "on the platform" before it starts moving.
    for (int i = 0; i < 30; ++i) world.step(kStep);
    ASSERT_TRUE(world.characterOnGround(rider));
    const float before = world.characterTransform(rider, 0.0f)[3].z;

    // Two seconds of steady travel, the platform written once per step the way a scene node
    // attached to it would write it.
    constexpr float kSpeed = 1.5f;
    float z = start.z;
    for (int i = 0; i < 120; ++i) {
        z += kSpeed * kStep;
        world.setBodyTransform(platform, glm::translate(glm::mat4(1.0f), glm::vec3(start.x, start.y, z)));
        world.step(kStep);
    }

    const float travelled = world.characterTransform(rider, 0.0f)[3].z - before;
    EXPECT_NEAR(world.bodyTransform(platform, 0.0f)[3].z, z, 0.05f) << "the platform itself did not arrive";
    EXPECT_GT(travelled, (z - start.z) * 0.8f) << "the rider was left behind by the platform";
    EXPECT_TRUE(world.characterOnGround(rider)) << "the rider fell off";
}

/**
 * The other half of riding one: arriving somewhere is not walking there. `characterSpeed` is
 * what a locomotion machine blends on, so a rider who pressed nothing has to read as still --
 * otherwise the platform animates a walk cycle nobody asked for, legs sliding over a surface
 * that is carrying them.
 *
 * The third phase is the same claim about *direction*, and it is what `characterVelocity`
 * exists for. A game with a mesh to turn has one obvious source for a heading -- difference
 * `characterTransform` across a frame -- and on a platform that source is the platform's
 * travel: the demo's character, parked on the sliding one with nothing pressed, faced the way
 * the platform was going and swung round again at each end of its run.
 *
 * So this walks the rider *against* a platform that outruns it. World displacement and the
 * character's own motion then point opposite ways, which is the one arrangement an accessor
 * reading either quantity for the other cannot pass. `moveSpeed` is 1 m/s here and nowhere
 * else in this file for exactly that reason.
 */
TEST(PhysicsCharacter, RidingAPlatformIsNeitherWalkingNorAHeading) {
    PhysicsWorld world;
    world.init({}, 4);
    addFloor(world);

    const glm::vec3 start(0.0f, 0.5f, 0.0f);
    const glm::vec3 half(2.0f, 0.25f, 2.0f);
    const BodyId platform = world.createBody(box(start, half, ColliderMotion::Kinematic));
    ASSERT_TRUE(platform.valid());

    ColliderDesc c = capsule({0.0f, start.y + half.y, 0.0f});
    // Slower than the platform, so phase three has the two disagreeing about which way the
    // character is going rather than merely about how fast.
    c.moveSpeed = 1.0f;
    const PhysicsCharacterId rider = world.createCharacter(c);
    ASSERT_TRUE(rider.valid());
    world.finalize();

    for (int i = 0; i < 30; ++i) world.step(kStep);
    ASSERT_TRUE(world.characterOnGround(rider));

    constexpr float kSpeed = 1.5f;
    float z = start.z;
    for (int i = 0; i < 60; ++i) {
        z += kSpeed * kStep;
        world.setBodyTransform(platform, glm::translate(glm::mat4(1.0f), glm::vec3(start.x, start.y, z)));
        world.step(kStep);
    }

    ASSERT_TRUE(world.characterOnGround(rider)) << "the rider fell off";
    EXPECT_NEAR(world.characterSpeed(rider), 0.0f, 0.15f)
        << "a rider who pressed nothing reads as moving at the platform's speed";
    // The same number as a vector, and therefore no heading either -- which is what the demo
    // turned its character by.
    EXPECT_NEAR(glm::length(world.characterVelocity(rider)), 0.0f, 0.15f);

    // And walking on it still reads as walking: the ground is subtracted, not the input.
    world.setCharacterInput(rider, {0.0f, 0.0f, 1.0f}, false);
    for (int i = 0; i < 60; ++i) {
        z += kSpeed * kStep;
        world.setBodyTransform(platform, glm::translate(glm::mat4(1.0f), glm::vec3(start.x, start.y, z)));
        world.step(kStep);
    }
    EXPECT_GT(world.characterSpeed(rider), 0.5f) << "walking along a moving platform reads as standing still";

    // ------------------------------------------------- and which way, with the two disagreeing
    const float turned = world.characterTransform(rider, 0.0f)[3].z;
    for (int i = 0; i < 90; ++i) {
        z += kSpeed * kStep;
        world.setBodyTransform(platform, glm::translate(glm::mat4(1.0f), glm::vec3(start.x, start.y, z)));
        world.setCharacterInput(rider, {0.0f, 0.0f, -1.0f}, false);
        world.step(kStep);
    }

    // Walking one way at 1 m/s on a platform going the other at 1.5, so the world carries the
    // rider forwards while the rider walks backwards. Both signs are asserted, because either
    // one alone is satisfied by an accessor that returns the other quantity.
    EXPECT_GT(world.characterTransform(rider, 0.0f)[3].z - turned, 0.0f)
        << "the platform did not outrun the rider, so this phase tests nothing";
    EXPECT_NEAR(world.characterVelocity(rider).z, -c.moveSpeed, 0.1f)
        << "the heading is the world's rather than the character's";
    EXPECT_NEAR(world.characterVelocity(rider).x, 0.0f, 0.05f);
    // Horizontal only: a rider on a rising lift is not walking upward.
    EXPECT_FLOAT_EQ(world.characterVelocity(rider).y, 0.0f);
}
