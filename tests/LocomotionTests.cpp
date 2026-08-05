#include "anim/Locomotion.h"

#include "anim/SceneAnimator.h"
#include "core/Input.h"
#include "scene/Physics.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <vector>

using namespace core;
using namespace scene;

using namespace anim;

/**
 * @file tests/LocomotionTests.cpp
 * @brief The driver between the character controller and the state machine.
 *
 * `Physics.cpp` and `SceneAnimator.cpp` both link with no device, so the pairing and every
 * parameter it writes are checkable without one — including the case this row is really about,
 * which is **two characters on two machines fed from two colliders**. A driver that worked
 * for one rig is what the row replaced, and one rig is exactly what a demo can demonstrate.
 *
 * Every assertion here reads a parameter back out of the animator, and **the driver never
 * sees an input map** -- a locomotion layer that played `run` because a key was down would
 * satisfy "the character animates" and prove nothing. One test declares a map anyway
 * (G17/C26): two players read one action and hand their own answers to their own
 * controllers, which is the arrangement that replaced the engine's single player slot. The
 * map is on the game's side of `setCharacterInput` there, which is the point of including it.
 */

namespace {

constexpr float kStep = 1.0f / 60.0f;

/// A rig with one node and one clip. Nothing here samples a pose; what the tests need is a
/// `SceneAnimator` that will hand out characters and hold a machine.
AnimationRig oneNodeRig() {
    AnimationRig rig;
    rig.bind.nodes.resize(1);
    rig.nodeNames.assign(1, "root");
    AnimationClip clip;
    clip.name = "idle";
    clip.duration = 1.0f;
    rig.clips.push_back(std::move(clip));
    return rig;
}

/// `speed`, `airborne` and a `jump` trigger, which is the set the driver looks for. The
/// states are irrelevant to the driver and are here so `stepStateMachine` has somewhere
/// to be.
AnimationStateMachine threeParameterMachine() {
    AnimationStateMachine m;
    m.states = {{"idle", 0, LoopMode::Loop, 1.0f}};
    m.parameters = {{"speed", false}, {"airborne", false}, {"jump", true}};
    return m;
}

/// The same machine with `airborne` left out, which is a rig that does not care whether it
/// is in the air rather than a misconfiguration.
AnimationStateMachine speedOnlyMachine() {
    AnimationStateMachine m;
    m.states = {{"idle", 0, LoopMode::Loop, 1.0f}};
    m.parameters = {{"speed", false}};
    return m;
}

/// A rig whose parameters are spelled somebody else's way.
AnimationStateMachine renamedMachine() {
    AnimationStateMachine m;
    m.states = {{"idle", 0, LoopMode::Loop, 1.0f}};
    m.parameters = {{"gait", false}, {"flying", false}, {"leap", true}};
    return m;
}

ColliderDesc capsule(const glm::vec3& feet, float moveSpeed) {
    ColliderDesc c;
    c.name = "player";
    c.motion = ColliderMotion::Character;
    c.radius = 0.3f;
    c.halfHeight = 0.6f;
    c.offset = {0.0f, 0.9f, 0.0f};
    c.moveSpeed = moveSpeed;
    c.transform = glm::translate(glm::mat4(1.0f), feet);
    return c;
}

void addFloor(PhysicsWorld& world) {
    ColliderDesc c;
    c.name = "floor";
    c.shape = ColliderShape::Box;
    c.motion = ColliderMotion::Static;
    c.halfExtent = {50.0f, 0.5f, 50.0f};
    c.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f));
    world.createBody(c);
}

/// Step the world and then the driver, which is the order `Engine::simulate` uses and the
/// one the driver's contract names.
void stepBoth(PhysicsWorld& world, SceneAnimator& animator, LocomotionDriver& driver, int steps) {
    for (int i = 0; i < steps; ++i) {
        world.step(kStep);
        driver.update(characterMotionSource(world), animator);
    }
}

} // namespace

TEST(LocomotionDriver, SpeedIsNormalisedAgainstTheCollidersOwnTopSpeed) {
    // **The row's whole argument as one assertion.** A game writing `speed / 4.0` asserts
    // that the machine's thresholds sit at 4 m/s; this collider tops out at 3.2, so the
    // parameter could never exceed 0.8 and the top fifth of every blend was unreachable.
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    const PhysicsCharacterId player = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 3.2f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId rig = anim.create();
    anim.setStateMachine(rig, threeParameterMachine());

    LocomotionDriver driver;
    driver.pair(player, rig);

    world.setCharacterInput(player, {0.0f, 0.0f, 1.0f}, false);
    // Long enough to reach the top speed through C20's acceleration ramp.
    stepBoth(world, anim, driver, 240);

    const uint32_t speed = anim.stateMachine(rig).findParameter("speed");
    ASSERT_NE(speed, kAnyState);
    // At the collider's top speed the parameter is 1, not 0.8.
    EXPECT_NEAR(anim.parameter(rig, speed), 1.0f, 0.02f);
    EXPECT_NEAR(world.characterSpeed(player), 3.2f, 0.05f);
}

TEST(LocomotionDriver, SpeedIsClampedToOneAndZeroAtRest) {
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    const PhysicsCharacterId player = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 2.0f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId rig = anim.create();
    anim.setStateMachine(rig, threeParameterMachine());

    LocomotionDriver driver;
    driver.pair(player, rig);
    stepBoth(world, anim, driver, 60);

    const uint32_t speed = anim.stateMachine(rig).findParameter("speed");
    EXPECT_NEAR(anim.parameter(rig, speed), 0.0f, 1e-3f);
}

TEST(LocomotionDriver, TwoCharactersOnTwoMachinesFedFromTwoColliders) {
    // The case the row exists for, and the one a demo with one rig cannot show. Two
    // colliders with different top speeds, two rigs with different parameter *indices* --
    // the second machine spells `speed` last rather than first -- and the driver has to get
    // both right at once.
    PhysicsWorld world;
    world.init({}, 16);
    addFloor(world);
    const PhysicsCharacterId fast = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 4.0f));
    const PhysicsCharacterId slow = world.createCharacter(capsule({6.0f, 0.0f, 0.0f}, 1.0f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId fastRig = anim.create();
    const AnimatorId slowRig = anim.create();
    anim.setStateMachine(fastRig, threeParameterMachine());

    AnimationStateMachine reordered;
    reordered.states = {{"idle", 0, LoopMode::Loop, 1.0f}};
    reordered.parameters = {{"airborne", false}, {"jump", true}, {"speed", false}};
    anim.setStateMachine(slowRig, reordered);

    LocomotionDriver driver;
    driver.pair(fast, fastRig);
    driver.pair(slow, slowRig);
    ASSERT_EQ(driver.pairCount(), 2u);

    // Only the slow one is asked to move, and it is asked to move flat out.
    world.setCharacterInput(slow, {0.0f, 0.0f, 1.0f}, false);
    stepBoth(world, anim, driver, 240);

    const uint32_t fastSpeed = anim.stateMachine(fastRig).findParameter("speed");
    const uint32_t slowSpeed = anim.stateMachine(slowRig).findParameter("speed");
    ASSERT_NE(fastSpeed, kAnyState);
    ASSERT_NE(slowSpeed, kAnyState);
    // The indices genuinely differ, or this is one machine tested twice.
    EXPECT_NE(fastSpeed, slowSpeed);

    EXPECT_NEAR(anim.parameter(fastRig, fastSpeed), 0.0f, 1e-3f);
    // Its own 1.0 m/s over its own 1.0 m/s -- not 1.0 over the other one's 4.0.
    EXPECT_NEAR(anim.parameter(slowRig, slowSpeed), 1.0f, 0.02f);
}

TEST(LocomotionDriver, TwoPlayersOnTwoPadsDriveTwoCharactersToTwoDifferentGaits) {
    // **G17's whole claim, with no `Engine` and no device.** Two players, two
    // controllers, two rigs, two locomotion pairs -- driven from two pads and arriving at two
    // different `speed` values. The engine used to hold one `playerCharacterIndex` latched
    // from the first `Character` collider it walked, so the second of these was unnameable
    // however plural the tables underneath already were.
    //
    // The rigs are identical on purpose. Anything that differs here differs because the
    // *players* did, which is what makes this a statement about the pairing rather than
    // about the machines.
    PhysicsWorld world;
    world.init({}, 16);
    addFloor(world);
    const PhysicsCharacterId one = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 4.0f));
    const PhysicsCharacterId two = world.createCharacter(capsule({6.0f, 0.0f, 0.0f}, 4.0f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId rigOne = anim.create();
    const AnimatorId rigTwo = anim.create();
    anim.setStateMachine(rigOne, threeParameterMachine());
    anim.setStateMachine(rigTwo, threeParameterMachine());

    LocomotionDriver driver;
    driver.pair(one, rigOne);
    driver.pair(two, rigTwo);
    ASSERT_EQ(driver.pairCount(), 2u);

    core::input::InputMap in;
    const core::input::ActionId forward = in.declare("Move.Forward", "Pad.LeftY-");
    in.setPlayerCount(2);
    in.setPlayerDevices(0, {false, 1u << 0});
    in.setPlayerDevices(1, {false, 1u << 1});

    // Pad 0 all the way forward, pad 1 at a third of its travel. Both are past the deadzone,
    // so the difference that arrives at the two rigs is the difference between the sticks.
    //
    // A stick past the deadzone is **rescaled, not passed through**: the map returns the
    // fraction of the travel *outside* the dead band, so 0.33 against a 0.15 deadzone is
    // (0.33 - 0.15) / (1 - 0.15) = 0.2118 and not 0.33. That is what makes a stick that has
    // only just left the band read as barely moving rather than as a third of top speed.
    core::input::GamepadState padOne;
    padOne.connected = true;
    padOne.axes[static_cast<int>(core::input::PadAxis::LeftY)] = -1.0f;
    core::input::GamepadState padTwo;
    padTwo.connected = true;
    padTwo.axes[static_cast<int>(core::input::PadAxis::LeftY)] = -0.33f;
    in.setGamepad(padOne, 0);
    in.setGamepad(padTwo, 1);
    in.beginFrame();

    // A game reads one action twice, once per player, and hands each answer to that player's
    // own controller. There is no engine-side "the player" between the two.
    const float requestOne = in.value(forward, 0);
    const float requestTwo = in.value(forward, 1);
    EXPECT_NEAR(requestOne, 1.0f, 1e-4f);
    EXPECT_NEAR(requestTwo, 0.2118f, 1e-3f);

    world.setCharacterInput(one, glm::vec3(0.0f, 0.0f, 1.0f) * requestOne, false);
    world.setCharacterInput(two, glm::vec3(0.0f, 0.0f, 1.0f) * requestTwo, false);
    stepBoth(world, anim, driver, 240);

    const uint32_t speed = anim.stateMachine(rigOne).findParameter("speed");
    ASSERT_NE(speed, kAnyState);
    EXPECT_NEAR(anim.parameter(rigOne, speed), 1.0f, 0.02f);
    EXPECT_NEAR(anim.parameter(rigTwo, speed), 0.2118f, 0.05f);
    // Both are animating -- a second player reading zero because nothing reached it is the
    // failure this is watching for, and it looks identical to a player standing still.
    EXPECT_GT(anim.parameter(rigTwo, speed), 0.1f);
}

TEST(LocomotionDriver, AParameterAMachineDoesNotHaveIsSkippedRatherThanRefused) {
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    const PhysicsCharacterId player = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 3.2f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId rig = anim.create();
    anim.setStateMachine(rig, speedOnlyMachine());

    LocomotionDriver driver;
    driver.pair(player, rig);
    world.setCharacterInput(player, {0.0f, 0.0f, 1.0f}, true);
    stepBoth(world, anim, driver, 120);

    // `speed` still arrives; `airborne` and `jump` simply have nowhere to go.
    const uint32_t speed = anim.stateMachine(rig).findParameter("speed");
    ASSERT_NE(speed, kAnyState);
    EXPECT_GT(anim.parameter(rig, speed), 0.5f);
    EXPECT_EQ(anim.stateMachine(rig).findParameter("airborne"), kAnyState);
    EXPECT_EQ(driver.pairCount(), 1u);
}

TEST(LocomotionDriver, TheNamesAreTheRigsAndCanBeReplaced) {
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    const PhysicsCharacterId player = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 3.2f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId rig = anim.create();
    anim.setStateMachine(rig, renamedMachine());

    LocomotionDriver driver;
    driver.pair(player, rig);
    driver.setParameters({.speed = "gait", .airborne = "flying", .jump = "leap"});

    world.setCharacterInput(player, {0.0f, 0.0f, 1.0f}, false);
    stepBoth(world, anim, driver, 240);

    const uint32_t gait = anim.stateMachine(rig).findParameter("gait");
    ASSERT_NE(gait, kAnyState);
    EXPECT_NEAR(anim.parameter(rig, gait), 1.0f, 0.02f);
}

TEST(LocomotionDriver, AirborneIsWrittenFromTheSolverRatherThanFromTheRequest) {
    // Dropped from two metres up: airborne while it falls, grounded once it lands, and
    // nothing was pressed either way.
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    const PhysicsCharacterId player = world.createCharacter(capsule({0.0f, 2.0f, 0.0f}, 3.2f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId rig = anim.create();
    anim.setStateMachine(rig, threeParameterMachine());

    LocomotionDriver driver;
    driver.pair(player, rig);

    const uint32_t airborne = anim.stateMachine(rig).findParameter("airborne");
    ASSERT_NE(airborne, kAnyState);

    stepBoth(world, anim, driver, 2);
    EXPECT_FLOAT_EQ(anim.parameter(rig, airborne), 1.0f);

    stepBoth(world, anim, driver, 120);
    EXPECT_FLOAT_EQ(anim.parameter(rig, airborne), 0.0f);
}

TEST(LocomotionDriver, PairingTheSameRigTwiceReplacesRatherThanDuplicates) {
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    const PhysicsCharacterId first = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 3.2f));
    const PhysicsCharacterId second = world.createCharacter(capsule({6.0f, 0.0f, 0.0f}, 1.0f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId rig = anim.create();
    anim.setStateMachine(rig, threeParameterMachine());

    LocomotionDriver driver;
    driver.pair(first, rig);
    driver.pair(second, rig);
    EXPECT_EQ(driver.pairCount(), 1u);

    // The second controller is the one driving it, so its top speed is the divisor.
    world.setCharacterInput(second, {0.0f, 0.0f, 1.0f}, false);
    stepBoth(world, anim, driver, 240);
    const uint32_t speed = anim.stateMachine(rig).findParameter("speed");
    EXPECT_NEAR(anim.parameter(rig, speed), 1.0f, 0.02f);
}

TEST(LocomotionDriver, ADeadPairIsDroppedRatherThanCarried) {
    // A world that destroys characters must not accumulate dead rows, and a stale handle
    // must not be asked anything.
    PhysicsWorld world;
    world.init({}, 8);
    addFloor(world);
    const PhysicsCharacterId player = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 3.2f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId rig = anim.create();
    anim.setStateMachine(rig, threeParameterMachine());

    LocomotionDriver driver;
    driver.pair(player, rig);
    stepBoth(world, anim, driver, 4);
    EXPECT_EQ(driver.pairCount(), 1u);

    anim.destroy(rig);
    stepBoth(world, anim, driver, 4);
    EXPECT_EQ(driver.pairCount(), 0u);
}

TEST(LocomotionDriver, AnInvalidHandleIsNotPaired) {
    LocomotionDriver driver;
    driver.pair(PhysicsCharacterId{}, AnimatorId{});
    EXPECT_EQ(driver.pairCount(), 0u);
}

// ------------------------------------------- a vocabulary per rig, not per driver

/**
 * The row's whole claim, and the case the singular `Parameters` could not express: a human
 * and a horse from two exporters, in one scene, both blending. Before this, naming the
 * second rig's parameters un-named the first's -- so the first stopped animating on the call
 * that made the second start, and nothing anywhere said so.
 */
TEST(LocomotionDriver, TwoRigsSpellingTheirParametersDifferentlyBothBlend) {
    PhysicsWorld world;
    world.init({}, 16);
    addFloor(world);
    const PhysicsCharacterId human = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 3.2f));
    const PhysicsCharacterId horse = world.createCharacter(capsule({6.0f, 0.0f, 0.0f}, 3.2f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId humanRig = anim.create();
    const AnimatorId horseRig = anim.create();
    anim.setStateMachine(humanRig, threeParameterMachine()); // speed / airborne / jump
    anim.setStateMachine(horseRig, renamedMachine());        // gait / flying / leap

    LocomotionDriver driver;
    driver.pair(human, humanRig);
    driver.pair(horse, horseRig, {.speed = "gait", .airborne = "flying", .jump = "leap"});

    world.setCharacterInput(human, {0.0f, 0.0f, 1.0f}, false);
    world.setCharacterInput(horse, {0.0f, 0.0f, 1.0f}, false);
    stepBoth(world, anim, driver, 240);

    const uint32_t speed = anim.stateMachine(humanRig).findParameter("speed");
    const uint32_t gait = anim.stateMachine(horseRig).findParameter("gait");
    ASSERT_NE(speed, kAnyState);
    ASSERT_NE(gait, kAnyState);
    // Both, which is the point. One name each was reachable before and the pair was not.
    EXPECT_NEAR(anim.parameter(humanRig, speed), 1.0f, 0.02f);
    EXPECT_NEAR(anim.parameter(horseRig, gait), 1.0f, 0.02f);

    // And each pair reports the vocabulary it is actually driven by rather than a driver-wide
    // one that would have to be wrong for one of them.
    EXPECT_EQ(driver.parameters(humanRig).speed, "speed");
    EXPECT_EQ(driver.parameters(horseRig).speed, "gait");
}

TEST(LocomotionDriver, NamingOneRigsParametersLeavesTheOthersAlone) {
    PhysicsWorld world;
    world.init({}, 16);
    addFloor(world);
    const PhysicsCharacterId human = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 3.2f));
    const PhysicsCharacterId horse = world.createCharacter(capsule({6.0f, 0.0f, 0.0f}, 3.2f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId humanRig = anim.create();
    const AnimatorId horseRig = anim.create();
    anim.setStateMachine(humanRig, threeParameterMachine());
    anim.setStateMachine(horseRig, renamedMachine());

    LocomotionDriver driver;
    driver.pair(human, humanRig);
    driver.pair(horse, horseRig);
    // Both are on the default vocabulary, so the horse is not being driven at all yet.
    driver.setParameters(horseRig, {.speed = "gait", .airborne = "flying", .jump = "leap"});

    world.setCharacterInput(human, {0.0f, 0.0f, 1.0f}, false);
    world.setCharacterInput(horse, {0.0f, 0.0f, 1.0f}, false);
    stepBoth(world, anim, driver, 240);

    EXPECT_NEAR(anim.parameter(humanRig, anim.stateMachine(humanRig).findParameter("speed")), 1.0f, 0.02f);
    EXPECT_NEAR(anim.parameter(horseRig, anim.stateMachine(horseRig).findParameter("gait")), 1.0f, 0.02f);
    EXPECT_EQ(driver.parameters(humanRig).speed, "speed") << "one rig's names reached another's";
}

TEST(LocomotionDriver, TheDriverWideCallStillReachesEveryPairWhicheverOrderItIsMadeIn) {
    // What `SceneAnimator::setStateMachine` does, for the reason it does it: a one-rig scene
    // stays a one-call scene, and the answer does not depend on whether a game pairs before
    // or after it names its parameters. A template-only version would differ between the two.
    PhysicsWorld world;
    world.init({}, 16);
    addFloor(world);
    const PhysicsCharacterId before = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 3.2f));
    const PhysicsCharacterId after = world.createCharacter(capsule({6.0f, 0.0f, 0.0f}, 3.2f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId early = anim.create();
    const AnimatorId late = anim.create();
    anim.setStateMachine(early, renamedMachine());
    anim.setStateMachine(late, renamedMachine());

    LocomotionDriver driver;
    driver.pair(before, early);
    driver.setParameters({.speed = "gait", .airborne = "flying", .jump = "leap"});
    driver.pair(after, late);

    world.setCharacterInput(before, {0.0f, 0.0f, 1.0f}, false);
    world.setCharacterInput(after, {0.0f, 0.0f, 1.0f}, false);
    stepBoth(world, anim, driver, 240);

    EXPECT_NEAR(anim.parameter(early, anim.stateMachine(early).findParameter("gait")), 1.0f, 0.02f);
    EXPECT_NEAR(anim.parameter(late, anim.stateMachine(late).findParameter("gait")), 1.0f, 0.02f);
}

TEST(LocomotionDriver, RePairingARigKeepsTheVocabularyItWasGiven) {
    // A rig handed a new body -- a respawn into a fresh controller -- must not quietly move
    // back onto the default names, which is the one way a pair could lose them.
    PhysicsWorld world;
    world.init({}, 16);
    addFloor(world);
    const PhysicsCharacterId first = world.createCharacter(capsule({0.0f, 0.0f, 0.0f}, 3.2f));
    const PhysicsCharacterId second = world.createCharacter(capsule({6.0f, 0.0f, 0.0f}, 3.2f));
    world.finalize();

    SceneAnimator anim;
    anim.init(oneNodeRig());
    const AnimatorId rig = anim.create();
    anim.setStateMachine(rig, renamedMachine());

    LocomotionDriver driver;
    driver.pair(first, rig, {.speed = "gait", .airborne = "flying", .jump = "leap"});
    driver.pair(second, rig);
    ASSERT_EQ(driver.pairCount(), 1u);
    EXPECT_EQ(driver.parameters(rig).speed, "gait");

    world.setCharacterInput(second, {0.0f, 0.0f, 1.0f}, false);
    stepBoth(world, anim, driver, 240);
    EXPECT_NEAR(anim.parameter(rig, anim.stateMachine(rig).findParameter("gait")), 1.0f, 0.02f);
}
