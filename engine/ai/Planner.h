#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ai {

/**
 * @file ai/Planner.h
 * @brief A goal-oriented planner: what a character is trying to do, and how to get there.
 *
 * Not a state machine, and authoring transitions here turns it into one: an action is reachable
 * from anything satisfying its prerequisites, so adding one edits nothing that already exists.
 * `AnimationStateMachine` decides what pose and takes continuous parameters; this decides what
 * to do, in booleans. See `architecture/systems.md`.
 */

/// Every property a world state can hold an opinion about. Raising it past 64 costs the
/// `uint64_t` a state is: comparison stops being two instructions and the search starts
/// allocating.
inline constexpr uint32_t kMaxProperties = 64;

/// A property that was never declared, and the value `find` returns for a name it has not
/// seen. Distinct from property 0, which is a perfectly ordinary property.
inline constexpr uint32_t kNoProperty = 0xFFFFFFFFu;

/**
 * @brief What is true, and what this state says nothing about.
 *
 * Two masks and not one: "the door is shut" and "I have no opinion about the door" are
 * different claims, and a single bitmask cannot tell them apart.
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

/// One thing a character can do. The planner reads only the contract; running the action is
/// the caller's job at the far end of the plan.
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
 * No Vulkan, no window, no clock. Reach for any of the three and this leaves the hosted set
 * and stops running under the sanitizers.
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
     * @param out Filled with action indices in the order they must run. Cleared first and left
     *            empty on failure, never holding a partial route -- executing half a plan leaves
     *            the world in a state nobody asked for.
     * @return false when no sequence reaches the goal within `kMaxPlanNodes`.
     *
     * A goal already satisfied returns true with an empty plan, so an empty `out` does not
     * distinguish success from failure and the return value must be read.
     */
    [[nodiscard]] bool plan(const WorldState& from, const WorldState& goal, std::vector<uint32_t>& out) const;

    /// A bound on *expansions*, not on plan length. Hitting it returns the same failure a
    /// genuinely unreachable goal does, so the two cannot be told apart from the outside.
    static constexpr uint32_t kMaxPlanNodes = 4096;

  private:
    std::vector<std::string> properties;
    std::vector<Action> actions;
};

/**
 * @brief A goal a character carries, the plan it is following, and where it has got to.
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
     * Re-plans on event, never per frame. Call it expecting a search each time and a character
     * costs a full A* sixty times a second; on an unchanged world this compares the current
     * step's prerequisites and returns.
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
