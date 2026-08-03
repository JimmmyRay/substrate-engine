#include "ai/Planner.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace ai;

/**
 * @file tests/PlannerTests.cpp
 * @brief The decision layer (C24).
 *
 * **The tests that matter are the ones about *derived* routes rather than authored ones.**
 * A planner that only ever returned the single action whose effects match the goal would
 * pass a suite of one-step plans and would be a lookup table with a search bolted on. What
 * separates it from a state machine is that nobody wrote `unarmed -> draw -> attack` down,
 * so that is what most of this file is about.
 *
 * Pure CPU and hosted, which is why it runs under ASan and TSan as well.
 */

namespace {

/// The card's worked example: an attack reachable only through a draw, which is reachable
/// only from being unarmed. No transition anywhere says so.
struct Combat {
    Planner planner;
    uint32_t unarmed = 0;
    uint32_t armed = 0;
    uint32_t enemyDead = 0;

    Combat() {
        unarmed = planner.declare("unarmed");
        armed = planner.declare("armed");
        enemyDead = planner.declare("enemy_dead");

        Action draw;
        draw.name = "draw";
        draw.prerequisites.set(unarmed, true);
        draw.effects.set(unarmed, false);
        draw.effects.set(armed, true);
        draw.cost = 1.0f;
        planner.add(draw);

        Action attack;
        attack.name = "attack";
        attack.prerequisites.set(armed, true);
        attack.effects.set(enemyDead, true);
        attack.cost = 1.0f;
        planner.add(attack);
    }

    [[nodiscard]] std::vector<std::string> names(const std::vector<uint32_t>& plan) const {
        std::vector<std::string> out;
        out.reserve(plan.size());
        for (const uint32_t a : plan) out.push_back(planner.action(a).name);
        return out;
    }
};

} // namespace

// ================================================================= WorldState

TEST(WorldState, AnUnknownIsNotAFalse) {
    // The distinction a single bitmask cannot make, and the reason there are two. An action
    // requiring `door_open == false` must not be satisfied by a state that has never heard
    // of the door.
    WorldState nothingKnown;
    WorldState wantsShut;
    wantsShut.set(3, false);
    EXPECT_FALSE(nothingKnown.satisfies(wantsShut));

    WorldState knowsShut;
    knowsShut.set(3, false);
    EXPECT_TRUE(knowsShut.satisfies(wantsShut));
}

TEST(WorldState, AGoalSaysNothingAboutWhatItDoesNotName) {
    WorldState world;
    world.set(0, true);
    world.set(1, false);
    world.set(2, true);

    WorldState goal;
    goal.set(2, true);
    EXPECT_TRUE(world.satisfies(goal));
}

TEST(WorldState, EffectsOverwriteAndClearForgets) {
    WorldState world;
    world.set(0, true);
    world.set(1, true);

    WorldState effects;
    effects.set(1, false);
    const WorldState after = world.after(effects);
    EXPECT_TRUE(after.get(0));
    EXPECT_TRUE(after.knows(1));
    EXPECT_FALSE(after.get(1));

    WorldState forgotten = world;
    forgotten.clear(1);
    EXPECT_FALSE(forgotten.knows(1));
}

// ==================================================================== Planner

TEST(Planner, DeclaringTheSameNameTwiceIsTheSameProperty) {
    Planner p;
    EXPECT_EQ(p.declare("hungry"), p.declare("hungry"));
    EXPECT_EQ(p.propertyCount(), 1u);
    EXPECT_EQ(p.find("hungry"), 0u);
    EXPECT_EQ(p.find("never_declared"), kNoProperty);
}

TEST(Planner, TheSixtyFifthPropertyIsRefusedRatherThanAliased) {
    Planner p;
    for (uint32_t i = 0; i < kMaxProperties; ++i) EXPECT_NE(p.declare("p" + std::to_string(i)), kNoProperty);
    EXPECT_EQ(p.declare("one too many"), kNoProperty);
    EXPECT_EQ(p.propertyCount(), kMaxProperties);
}

TEST(Planner, AGoalReachableOnlyThroughAnActionAnotherProduces) {
    // **The whole point of the layer.** Nothing authored `unarmed -> draw -> attack`: the
    // goal names a dead enemy, and `draw` is in the plan because `attack` needs what it
    // produces.
    const Combat c;
    WorldState world;
    world.set(c.unarmed, true);
    world.set(c.armed, false);
    world.set(c.enemyDead, false);

    WorldState goal;
    goal.set(c.enemyDead, true);

    std::vector<uint32_t> plan;
    ASSERT_TRUE(c.planner.plan(world, goal, plan));
    EXPECT_EQ(c.names(plan), (std::vector<std::string>{"draw", "attack"}));
}

TEST(Planner, AnActionAddedLaterIsReachableWithNoTransitionEdited) {
    // The property that makes a planner worth having in proportion to the number of
    // actions. `sheathe` did not exist when `draw` was written and nothing about `draw`
    // changes to make the round trip reachable.
    Combat c;
    const uint32_t sheathed = c.planner.declare("sheathed");
    Action sheathe;
    sheathe.name = "sheathe";
    sheathe.prerequisites.set(c.armed, true);
    sheathe.effects.set(sheathed, true);
    sheathe.effects.set(c.armed, false);
    sheathe.effects.set(c.unarmed, true);
    c.planner.add(sheathe);

    WorldState world;
    world.set(c.unarmed, true);
    world.set(c.armed, false);
    world.set(sheathed, false);

    WorldState goal;
    goal.set(sheathed, true);

    std::vector<uint32_t> plan;
    ASSERT_TRUE(c.planner.plan(world, goal, plan));
    EXPECT_EQ(c.names(plan), (std::vector<std::string>{"draw", "sheathe"}));
}

TEST(Planner, TwoRoutesAndTheCheaperOneWins) {
    Planner p;
    const uint32_t there = p.declare("at_the_gate");

    Action walk;
    walk.name = "walk";
    walk.effects.set(there, true);
    walk.cost = 10.0f;
    p.add(walk);

    Action ride;
    ride.name = "ride";
    ride.effects.set(there, true);
    ride.cost = 2.0f;
    p.add(ride);

    WorldState world;
    world.set(there, false);
    WorldState goal;
    goal.set(there, true);

    std::vector<uint32_t> plan;
    ASSERT_TRUE(p.plan(world, goal, plan));
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(p.action(plan[0]).name, "ride");
}

TEST(Planner, TheCheaperRouteIsCheaperOverallAndNotPerStep) {
    // Two steps at 1 each beats one step at 5, which is what says the search sums the path
    // rather than picking the cheapest available action.
    Planner p;
    const uint32_t half = p.declare("halfway");
    const uint32_t done = p.declare("done");

    Action leap;
    leap.name = "leap";
    leap.effects.set(done, true);
    leap.cost = 5.0f;
    p.add(leap);

    Action step1;
    step1.name = "step1";
    step1.effects.set(half, true);
    step1.cost = 1.0f;
    p.add(step1);

    Action step2;
    step2.name = "step2";
    step2.prerequisites.set(half, true);
    step2.effects.set(done, true);
    step2.cost = 1.0f;
    p.add(step2);

    WorldState world;
    world.set(half, false);
    world.set(done, false);
    WorldState goal;
    goal.set(done, true);

    std::vector<uint32_t> plan;
    ASSERT_TRUE(p.plan(world, goal, plan));
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(p.action(plan[0]).name, "step1");
    EXPECT_EQ(p.action(plan[1]).name, "step2");
}

TEST(Planner, NoRouteReturnsNothingRatherThanAPartialPlan) {
    // The failure this is really about: a caller handed the half of a plan that *was*
    // reachable would execute it and leave the world somewhere nobody asked for.
    Combat c;
    const uint32_t flying = c.planner.declare("flying");
    WorldState world;
    world.set(c.unarmed, true);
    world.set(flying, false);

    WorldState goal;
    goal.set(c.enemyDead, true);
    goal.set(flying, true);

    std::vector<uint32_t> plan;
    EXPECT_FALSE(c.planner.plan(world, goal, plan));
    EXPECT_TRUE(plan.empty()) << "a failed plan left " << plan.size() << " steps behind";
}

TEST(Planner, AGoalAlreadyMetSucceedsWithNoSteps) {
    // True with an empty plan, which is a different answer from false with an empty plan.
    // A caller cannot tell them apart from the size, so the return value carries it.
    const Combat c;
    WorldState world;
    world.set(c.enemyDead, true);
    WorldState goal;
    goal.set(c.enemyDead, true);

    std::vector<uint32_t> plan;
    EXPECT_TRUE(c.planner.plan(world, goal, plan));
    EXPECT_TRUE(plan.empty());
}

TEST(Planner, AnActionThatChangesNothingIsNotWalkedThrough) {
    // A self-loop makes the frontier grow while the plan does not, which is how a search
    // spends its whole budget going nowhere.
    Planner p;
    const uint32_t lit = p.declare("lit");

    Action idle;
    idle.name = "idle";
    idle.effects.set(lit, false);
    idle.cost = 0.1f;
    p.add(idle);

    Action light;
    light.name = "light";
    light.effects.set(lit, true);
    p.add(light);

    WorldState world;
    world.set(lit, false);
    WorldState goal;
    goal.set(lit, true);

    std::vector<uint32_t> plan;
    ASSERT_TRUE(p.plan(world, goal, plan));
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(p.action(plan[0]).name, "light");
}

// ====================================================================== Agent

TEST(Agent, FollowsThePlanOneStepAtATime) {
    const Combat c;
    WorldState world;
    world.set(c.unarmed, true);
    world.set(c.armed, false);
    world.set(c.enemyDead, false);

    WorldState goal;
    goal.set(c.enemyDead, true);

    Agent agent;
    agent.setGoal(goal);

    const uint32_t first = agent.advance(c.planner, world);
    ASSERT_NE(first, Agent::kNoAction);
    EXPECT_EQ(c.planner.action(first).name, "draw");
    EXPECT_TRUE(agent.replanned());

    // Asking again with nothing changed gives the same step and does not search.
    EXPECT_EQ(agent.advance(c.planner, world), first);
    EXPECT_FALSE(agent.replanned());

    // The draw happened.
    world = world.after(c.planner.action(first).effects);
    const uint32_t second = agent.advance(c.planner, world);
    ASSERT_NE(second, Agent::kNoAction);
    EXPECT_EQ(c.planner.action(second).name, "attack");
    EXPECT_FALSE(agent.replanned()) << "the plan was still valid and was thrown away anyway";

    world = world.after(c.planner.action(second).effects);
    EXPECT_EQ(agent.advance(c.planner, world), Agent::kNoAction);
    EXPECT_TRUE(agent.plan().empty());
}

TEST(Agent, APlanInvalidatedMidExecutionIsReplanned) {
    // The card's fourth case. The world moves out from under the plan -- something
    // disarmed the character between the draw and the attack -- and the agent has to
    // notice rather than run an action whose prerequisites are gone.
    const Combat c;
    WorldState world;
    world.set(c.unarmed, true);
    world.set(c.armed, false);
    world.set(c.enemyDead, false);

    WorldState goal;
    goal.set(c.enemyDead, true);

    Agent agent;
    agent.setGoal(goal);
    const uint32_t draw = agent.advance(c.planner, world);
    ASSERT_EQ(c.planner.action(draw).name, "draw");

    world = world.after(c.planner.action(draw).effects);
    ASSERT_EQ(c.planner.action(agent.advance(c.planner, world)).name, "attack");

    // Disarmed by somebody else.
    world.set(c.armed, false);
    world.set(c.unarmed, true);
    const uint32_t again = agent.advance(c.planner, world);
    ASSERT_NE(again, Agent::kNoAction);
    EXPECT_EQ(c.planner.action(again).name, "draw");
    EXPECT_TRUE(agent.replanned());
}

TEST(Agent, AStepTheWorldSatisfiedOnItsOwnIsSkipped) {
    // Somebody else drew the sword. The plan is still the plan and the cursor moves past
    // the step that is already done rather than the character repeating it.
    const Combat c;
    WorldState world;
    world.set(c.unarmed, true);
    world.set(c.armed, false);
    world.set(c.enemyDead, false);

    WorldState goal;
    goal.set(c.enemyDead, true);

    Agent agent;
    agent.setGoal(goal);
    ASSERT_EQ(c.planner.action(agent.advance(c.planner, world)).name, "draw");

    world.set(c.armed, true);
    world.set(c.unarmed, false);
    const uint32_t next = agent.advance(c.planner, world);
    ASSERT_NE(next, Agent::kNoAction);
    EXPECT_EQ(c.planner.action(next).name, "attack");
}

TEST(Agent, AnUnreachableGoalReportsNoActionRatherThanSearchingForever) {
    Combat c;
    const uint32_t flying = c.planner.declare("flying");
    WorldState world;
    world.set(c.unarmed, true);
    world.set(flying, false);

    WorldState goal;
    goal.set(flying, true);

    Agent agent;
    agent.setGoal(goal);
    EXPECT_EQ(agent.advance(c.planner, world), Agent::kNoAction);
    EXPECT_TRUE(agent.plan().empty());
}

TEST(Agent, AGoalAlreadyMetNeedsNoPlan) {
    const Combat c;
    WorldState world;
    world.set(c.enemyDead, true);
    WorldState goal;
    goal.set(c.enemyDead, true);

    Agent agent;
    agent.setGoal(goal);
    EXPECT_EQ(agent.advance(c.planner, world), Agent::kNoAction);
    EXPECT_FALSE(agent.replanned()) << "a goal already met should not cost a search";
}
