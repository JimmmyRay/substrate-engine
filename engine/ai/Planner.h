#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ai {

/**
 * @file ai/Planner.h
 * @brief A goal-oriented planner: what a character is trying to do, and how to get there.
 *
 * **This is not a second state machine and must not become one.** `AnimationStateMachine`
 * decides *what pose*, its parameters are continuous, and `speed` at 0.42 -- most of the way
 * from walk to run -- is a value a planner has no way to express. This decides *what to do*,
 * its state is boolean, and its output is a sequence. The two meet at an intent: the planner
 * decides "walk to the pot", the character controller pursues it, and the machine blends
 * whatever gait that produces. See `architecture/systems.md`.
 *
 * A planner rather than another state machine, and the distinction is the reason the layer
 * exists. A machine needs every route spelled out as a transition: to reach `attack` from
 * `unarmed` somebody authors `unarmed -> draw -> attack`, and authors it again for every
 * state `draw` might be entered from. A planner is given `attack` as a goal and derives the
 * route from what each action requires and produces, so an action added later is reachable
 * from everything that satisfies it without a transition being edited.
 */

/// Every property a world state can hold an opinion about. Sixty-four is a `uint64_t`, and
/// the whole design rests on a state fitting in a register: comparison is two instructions,
/// applying an effect is two, and the search never allocates a state.
inline constexpr uint32_t kMaxProperties = 64;

/// A property that was never declared, and the value `find` returns for a name it has not
/// seen. Distinct from property 0, which is a perfectly ordinary property.
inline constexpr uint32_t kNoProperty = 0xFFFFFFFFu;

/**
 * @brief What is true, and what this state says nothing about.
 *
 * Two masks rather than one, because "the door is shut" and "I have no opinion about the
 * door" are different claims and a single bitmask cannot tell them apart. An action's
 * prerequisites are the second kind almost everywhere -- most actions care about two
 * properties out of forty -- and a goal is as well.
 */
struct WorldState {
    uint64_t known = 0; ///< bit set: this state has an opinion about that property
    uint64_t value = 0; ///< bit set: and the opinion is `true`. Meaningless where `known` is 0.

    void set(uint32_t property, bool on) {
        if (property >= kMaxProperties) return;
        const uint64_t bit = uint64_t{1} << property;
        known |= bit;
        value = on ? (value | bit) : (value & ~bit);
    }

    /// Forget an opinion. What an effect that *invalidates* a property does.
    void clear(uint32_t property) {
        if (property >= kMaxProperties) return;
        const uint64_t bit = uint64_t{1} << property;
        known &= ~bit;
        value &= ~bit;
    }

    [[nodiscard]] bool knows(uint32_t property) const {
        return property < kMaxProperties && (known & (uint64_t{1} << property)) != 0;
    }
    [[nodiscard]] bool get(uint32_t property) const {
        return property < kMaxProperties && (value & (uint64_t{1} << property)) != 0;
    }

    /// Does this state satisfy every opinion `wanted` holds? A property `wanted` says
    /// nothing about is not a constraint, and a property *this* says nothing about cannot
    /// satisfy one -- an unknown is not a false.
    [[nodiscard]] bool satisfies(const WorldState& wanted) const {
        return (known & wanted.known) == wanted.known && ((value ^ wanted.value) & wanted.known) == 0;
    }

    /// This state with `effects` applied over it.
    [[nodiscard]] WorldState after(const WorldState& effects) const {
        return {known | effects.known, (value & ~effects.known) | (effects.value & effects.known)};
    }

    bool operator==(const WorldState& o) const { return known == o.known && value == o.value; }
};

/// One thing a character can do. A flat record: no `std::function` body and no `shared_ptr`,
/// because what the planner needs is the *contract* -- what must be true, what becomes true,
/// what it costs -- and running it is the caller's job at the far end of the plan.
struct Action {
    std::string name;
    WorldState prerequisites;
    WorldState effects;
    /// Anything positive. Compared, summed and never interpreted, so a caller may use
    /// seconds, metres or a made-up number as long as it uses one of them throughout.
    float cost = 1.0f;
};

/**
 * @brief The action table and the search over it.
 *
 * Hosted: no Vulkan, no window, no clock. The whole of it is a name table, an action vector
 * and an A* over `WorldState`, so it tests under every sanitizer.
 */
class Planner {
  public:
    /// Declare a property, or return the one already declared under that name.
    /// `kNoProperty` once sixty-four exist, logged once.
    uint32_t declare(const std::string& name);
    /// The property under `name`, or `kNoProperty`. Never declares.
    [[nodiscard]] uint32_t find(const std::string& name) const;
    [[nodiscard]] const std::string& propertyName(uint32_t property) const;
    [[nodiscard]] uint32_t propertyCount() const { return static_cast<uint32_t>(properties.size()); }

    /// Add an action. Returns its index, which is what a plan is a sequence of.
    uint32_t add(Action action);
    [[nodiscard]] const Action& action(uint32_t index) const { return actions[index]; }
    [[nodiscard]] uint32_t actionCount() const { return static_cast<uint32_t>(actions.size()); }
    void clear();

    /**
     * @brief The cheapest sequence from `from` that satisfies `goal`.
     *
     * @param out Filled with action indices in the order they must run. Cleared first, and
     *            **left empty on failure** rather than holding a partial route: a plan that
     *            got halfway is not a plan, and a caller that executed one would leave the
     *            world in a state nobody asked for.
     * @return false when no sequence reaches the goal within `kMaxPlanNodes`.
     *
     * A goal already satisfied returns true with an empty plan, which is the honest answer
     * and is not the same as a failure. Callers have to tell the two apart, so the return
     * value carries it rather than the size.
     */
    [[nodiscard]] bool plan(const WorldState& from, const WorldState& goal, std::vector<uint32_t>& out) const;

    /// The bound on the search, and it is a bound on *expansions* rather than on plan
    /// length. A search that hits it returns failure, which is the same answer a caller
    /// gets for a genuinely unreachable goal -- deliberately, because a plan nobody can
    /// find and a plan too expensive to find are the same thing to the character.
    static constexpr uint32_t kMaxPlanNodes = 4096;

  private:
    std::vector<std::string> properties;
    std::vector<Action> actions;
};

/**
 * @brief A goal a character carries, the plan it is following, and where it has got to.
 *
 * The layer the card calls an *intent*. A planner on its own answers a question; this is
 * what makes an answer something a character can act on over several steps, and what
 * notices that the world moved out from under it.
 */
class Agent {
  public:
    void setGoal(WorldState goal);
    [[nodiscard]] const WorldState& goal() const { return wanted; }

    /**
     * @brief Re-plan if the current plan is no longer valid, and report the current step.
     *
     * @return the action to be running now, or `kNoAction` when the goal is met or
     *         unreachable.
     *
     * **Re-planned on event rather than per frame**, which is the whole reason the plan is
     * held rather than recomputed: a search run sixty times a second over a question with no
     * prerequisites and no sequence would cost the cross-fades to do it. `advance` is cheap
     * on the frames where nothing changed -- it compares the current step's prerequisites
     * against the world and returns.
     */
    uint32_t advance(const Planner& planner, const WorldState& world);

    /// The plan as it stands, for a log line or an inspector. Empty when there is none.
    [[nodiscard]] const std::vector<uint32_t>& plan() const { return steps; }
    [[nodiscard]] uint32_t cursor() const { return at; }
    /// True when the last `advance` had to search. What a caller logs the plan on.
    [[nodiscard]] bool replanned() const { return replannedLast; }

    static constexpr uint32_t kNoAction = 0xFFFFFFFFu;

  private:
    WorldState wanted;
    std::vector<uint32_t> steps;
    uint32_t at = 0;
    bool replannedLast = false;
};

} // namespace ai
