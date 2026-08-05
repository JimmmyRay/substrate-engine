#include "scene/Simulation.h"

#include "Modules.h"
#include "physics/PhysicsModule.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

using namespace scene;

/**
 * @file tests/SimulationTests.cpp
 * @brief The step order, run with no device.
 *
 * `scene/Simulation.cpp` is the one place the order of a step is written, and it is hosted —
 * so the thing a headless server would run is the thing this file runs, rather than a
 * stand-in for it. The engine's `simulate` is one line delegating here.
 *
 * The long arm is deliberately long. Ten thousand steps is not a stress test: it is the case
 * a batch job or a dedicated server is, and the failures it exists to catch — a leak, an
 * accumulator that drifts, a subsystem that grows a vector every step — are all invisible at
 * sixty.
 */

namespace {

constexpr float kStep = 1.0f / 60.0f;

ColliderDesc floorBox() {
    ColliderDesc c;
    c.name = "floor";
    c.shape = ColliderShape::Box;
    c.motion = ColliderMotion::Static;
    c.halfExtent = {50.0f, 0.5f, 50.0f};
    c.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f));
    return c;
}

ColliderDesc capsuleAt(const glm::vec3& feet) {
    ColliderDesc c;
    c.name = "player";
    c.motion = ColliderMotion::Character;
    c.radius = 0.3f;
    c.halfHeight = 0.6f;
    c.offset = {0.0f, 0.9f, 0.0f};
    c.moveSpeed = 3.0f;
    c.transform = glm::translate(glm::mat4(1.0f), feet);
    return c;
}

ColliderDesc crateAt(const glm::vec3& at) {
    ColliderDesc c;
    c.name = "crate";
    c.shape = ColliderShape::Box;
    c.motion = ColliderMotion::Dynamic;
    c.halfExtent = {0.25f, 0.25f, 0.25f};
    c.transform = glm::translate(glm::mat4(1.0f), at);
    return c;
}

} // namespace

TEST(Simulation, StepsAWorldWithNoDeviceAndNoWindow) {
    // The row's claim, at its smallest: a world built, stepped, and read back, in a binary
    // that links neither Vulkan nor GLFW. If this file ever needs a device to compile, the
    // link of `substrate_tests` and `substrate-sim` is what says so.
    Simulation sim;
    // The world `sim.step` moves is the module's, not one of this file's: linking physics is
    // what puts a solver behind `modules::physics`, and building a second world here would
    // step the empty one and assert about this one.
    physics::PhysicsWorld& world = physics::world();
    world.init({}, 8);
    world.createBody(floorBox());
    const PhysicsCharacterId player = world.createCharacter(capsuleAt({0.0f, 2.0f, 0.0f}));
    world.finalize();

    ASSERT_TRUE(player.valid());
    // Dropped from two metres, so "it fell" and "it landed" are two different assertions.
    EXPECT_FALSE(world.characterOnGround(player));

    for (int i = 0; i < 240; ++i) sim.step(kStep);

    EXPECT_TRUE(world.characterOnGround(player));
    const glm::mat4 at = world.characterTransform(player, 0.0f);
    EXPECT_NEAR(at[3].y, 0.0f, 0.05f);
}

TEST(Simulation, TenThousandStepsReachASteadyStateAndStayThere) {
    // **The case a server is.** A loop meant to run for hours is not tested by running it for
    // one second, and the three things that break over a long run — a growing vector, an
    // accumulator that drifts, a solver that never settles — all look fine at sixty steps.
    Simulation sim;
    physics::PhysicsWorld& world = physics::world();
    world.init({}, 16);
    world.createBody(floorBox());
    world.createBody(crateAt({0.0f, 3.0f, 0.0f}));
    const PhysicsCharacterId player = world.createCharacter(capsuleAt({4.0f, 0.0f, 0.0f}));
    world.finalize();

    const uint32_t bodies = world.bodyCount();
    const uint32_t characters = world.characterCount();

    for (int i = 0; i < 2000; ++i) sim.step(kStep);
    const glm::mat4 settled = world.characterTransform(player, 0.0f);
    const BodyId crate = world.bodyAt(1);
    const glm::mat4 crateSettled = world.bodyTransform(crate, 0.0f);

    for (int i = 0; i < 8000; ++i) sim.step(kStep);

    // Nothing was asked to move after it settled, so eight thousand further steps is eight
    // thousand chances to drift. A millimetre is the tolerance because the solver puts a
    // resting body to sleep rather than freezing it exactly.
    const glm::mat4 later = world.characterTransform(player, 0.0f);
    EXPECT_NEAR(later[3].x, settled[3].x, 1e-3f);
    EXPECT_NEAR(later[3].y, settled[3].y, 1e-3f);
    EXPECT_NEAR(later[3].z, settled[3].z, 1e-3f);
    EXPECT_NEAR(world.bodyTransform(crate, 0.0f)[3].y, crateSettled[3].y, 1e-3f);

    // **The table did not grow.** A step that appends a slot per iteration is the leak this
    // arm is really watching for, and it is invisible in a position.
    EXPECT_EQ(world.bodyCount(), bodies);
    EXPECT_EQ(world.characterCount(), characters);
}

TEST(Simulation, AnEmptyWorldStepsAndCostsNothing) {
    // Every subsystem is called unconditionally, so an empty one has to be safe rather than
    // merely cheap — that is what lets the profile carry a named zero per system instead of a
    // missing row. The engine relies on it for a scene that authors none of them.
    Simulation sim;
    physics::PhysicsWorld& world = physics::world();
    world.init({}, 1);
    world.finalize();

    for (int i = 0; i < 600; ++i) sim.step(kStep);

    EXPECT_EQ(world.bodyCount(), 0u);
    EXPECT_EQ(world.characterCount(), 0u);
    // The step ran against `modules::Anim::empty` -- this binary links no animation module,
    // and that is the arrangement a headless server without one is in.
    EXPECT_TRUE(modules::anim->stats().empty);
}

TEST(Simulation, TheStepIsFixedAndTwoRunsOfItAgree) {
    // Determinism is the property a batch tuning job rests on: the same scene stepped the
    // same number of times has to give the same answer, or a sweep over one parameter is
    // measuring the machine. The world is rebuilt between the two arms rather than reused,
    // because a teardown that quietly kept state would pass the second and fail this.
    auto run = [](int steps) {
        Simulation sim;
        physics::PhysicsWorld& world = physics::world();
        world.init({}, 8);
        world.createBody(floorBox());
        world.createBody(crateAt({0.1f, 4.0f, -0.2f}));
        const PhysicsCharacterId p = world.createCharacter(capsuleAt({1.0f, 1.0f, 0.0f}));
        world.finalize();
        world.setCharacterInput(p, {0.0f, 0.0f, 1.0f}, false);
        for (int i = 0; i < steps; ++i) sim.step(kStep);
        return world.characterTransform(p, 0.0f)[3];
    };

    const glm::vec4 first = run(600);
    const glm::vec4 second = run(600);
    EXPECT_FLOAT_EQ(first.x, second.x);
    EXPECT_FLOAT_EQ(first.y, second.y);
    EXPECT_FLOAT_EQ(first.z, second.z);
}
