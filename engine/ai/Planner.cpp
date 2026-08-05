#include "ai/Planner.h"

#include "core/Logger.h"

#include <algorithm>
#include <bit>
#include <unordered_map>

namespace ai {

namespace {

const std::string kNoName;

/// Unsatisfied goal bits, unscaled. Scale it by anything and the heuristic can overestimate,
/// at which point A* returns a plan rather than the cheapest one.
uint32_t unmet(const WorldState& state, const WorldState& goal) {
    const uint64_t missing = (~state.known & goal.known) | ((state.value ^ goal.value) & goal.known & state.known);
    return static_cast<uint32_t>(std::popcount(missing));
}

struct Node {
    WorldState state;
    float cost = 0.0f;        ///< g: what it took to get here
    uint32_t parent = 0xFFFFFFFFu;
    uint32_t viaAction = 0xFFFFFFFFu;
};

struct StateHash {
    size_t operator()(const WorldState& s) const {
        // Multiply-xor over two 64-bit words: a collision costs a comparison, not a wrong
        // answer.
        uint64_t h = s.known * 0x9E3779B97F4A7C15ull;
        h ^= s.value + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

} // namespace

uint32_t Planner::declare(const std::string& name) {
    for (uint32_t i = 0; i < properties.size(); ++i) {
        if (properties[i] == name) return i;
    }
    if (properties.size() >= kMaxProperties) {
        // Refused and named, never folded onto an existing bit: a property that quietly
        // aliases another is a plan that is wrong in a way nobody can read.
        core::Logger::warn(core::LogCategory::Scene,
                           "Planner: '%s' is the %u'th property and the state is %u bits wide -- refused",
                           name.c_str(), static_cast<uint32_t>(properties.size()) + 1, kMaxProperties);
        return kNoProperty;
    }
    properties.push_back(name);
    return static_cast<uint32_t>(properties.size()) - 1;
}

uint32_t Planner::find(const std::string& name) const {
    for (uint32_t i = 0; i < properties.size(); ++i) {
        if (properties[i] == name) return i;
    }
    return kNoProperty;
}

const std::string& Planner::propertyName(uint32_t property) const {
    return property < properties.size() ? properties[property] : kNoName;
}

uint32_t Planner::add(Action action) {
    // A zero-cost action makes every plan through it free and the frontier's ordering
    // meaningless; a negative one makes A* wrong outright.
    action.cost = std::max(action.cost, 1e-4f);
    actions.push_back(std::move(action));
    return static_cast<uint32_t>(actions.size()) - 1;
}

void Planner::clear() {
    properties.clear();
    actions.clear();
}

bool Planner::plan(const WorldState& from, const WorldState& goal, std::vector<uint32_t>& out) const {
    out.clear();
    if (from.satisfies(goal)) return true;

    std::vector<Node> nodes;
    nodes.push_back({from, 0.0f, 0xFFFFFFFFu, 0xFFFFFFFFu});

    // The cheapest way to each state seen so far. Keyed on the state, not the route: the
    // number of states is bounded by the properties and the number of routes is not.
    std::unordered_map<WorldState, float, StateHash> best;
    best.emplace(from, 0.0f);

    // A vector with a linear minimum, not a heap: the frontier is branching factor times plan
    // length, which is tens. A heap starts paying only well past that.
    std::vector<uint32_t> open{0};

    for (uint32_t expansions = 0; !open.empty() && expansions < kMaxPlanNodes; ++expansions) {
        uint32_t pick = 0;
        float bestF = 0.0f;
        for (uint32_t i = 0; i < open.size(); ++i) {
            const Node& n = nodes[open[i]];
            const float f = n.cost + static_cast<float>(unmet(n.state, goal));
            if (i == 0 || f < bestF) {
                bestF = f;
                pick = i;
            }
        }
        const uint32_t index = open[pick];
        open[pick] = open.back();
        open.pop_back();

        if (nodes[index].state.satisfies(goal)) {
            for (uint32_t walk = index; nodes[walk].viaAction != 0xFFFFFFFFu; walk = nodes[walk].parent) {
                out.push_back(nodes[walk].viaAction);
            }
            std::reverse(out.begin(), out.end());
            return true;
        }

        for (uint32_t a = 0; a < actions.size(); ++a) {
            const Action& candidate = actions[a];
            if (!nodes[index].state.satisfies(candidate.prerequisites)) continue;

            const WorldState next = nodes[index].state.after(candidate.effects);
            // An action changing nothing about this state is a loop of length one; admit it
            // and the frontier grows while the plan does not.
            if (next == nodes[index].state) continue;

            const float cost = nodes[index].cost + candidate.cost;
            if (const auto seen = best.find(next); seen != best.end() && seen->second <= cost) continue;

            best[next] = cost;
            nodes.push_back({next, cost, index, a});
            open.push_back(static_cast<uint32_t>(nodes.size()) - 1);
        }
    }

    // Nothing partial. `out` was cleared at the top and only a completed walk writes to it.
    return false;
}

void Agent::setGoal(WorldState goal) {
    wanted = goal;
    steps.clear();
    at = 0;
}

uint32_t Agent::advance(const Planner& planner, const WorldState& world) {
    replannedLast = false;

    if (world.satisfies(wanted)) {
        steps.clear();
        at = 0;
        return kNoAction;
    }

    // The cursor must move past what is done *before* the plan is judged. A finished step's
    // prerequisites are routinely false afterwards -- that is what an effect is -- so judging
    // validity first re-plans on every step of every successful plan: `draw` requires
    // `unarmed`, drawing makes the character armed, and the cursor still points at `draw`.
    const auto skipFinished = [&] {
        while (at < steps.size() && world.satisfies(planner.action(steps[at]).effects)) ++at;
    };
    skipFinished();

    if (at >= steps.size() || !world.satisfies(planner.action(steps[at]).prerequisites)) {
        replannedLast = true;
        at = 0;
        if (!planner.plan(world, wanted, steps)) {
            steps.clear();
            return kNoAction;
        }
        // A fresh plan can still open on a step the world already satisfies, so the same walk
        // runs again rather than being assumed unnecessary.
        skipFinished();
        if (at >= steps.size()) return kNoAction;
    }

    return steps[at];
}

} // namespace ai
