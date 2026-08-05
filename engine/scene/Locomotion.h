#pragma once

#include "scene/Animation.h"
#include "scene/Physics.h"

#include <string>
#include <vector>

namespace scene {

/**
 * @file scene/Locomotion.h
 * @brief The wiring between a character controller and the state machine that animates it.
 *
 * Layering -- what decides a character's intent, and why that is not here -- is argued in
 * `architecture/systems.md`.
 */

/**
 * @brief Writes a rig's locomotion parameters from what its controller actually did.
 *
 * Every value written here comes back out of the solver, never out of an input map.
 * `characterJumped` in particular cannot be reconstructed from the key and the ground state:
 * a coyote window and a jump buffer make the two disagree by design.
 */
class LocomotionDriver {
  public:
    /**
     * @brief The names looked up in one rig's own machine.
     *
     * A name the machine does not carry is skipped rather than reported, so a misspelling
     * here costs an animation that silently never blends.
     */
    struct Parameters {
        std::string speed = "speed";       ///< 0..1, the controller's speed over its top speed
        std::string airborne = "airborne"; ///< 1 while not standing on anything
        std::string jump = "jump";         ///< fired on the step the controller launched
    };

    /// Pair a controller with the rig it animates, on the driver's current vocabulary.
    /// Re-pairing a rig keeps the names that pair already had.
    void pair(PhysicsCharacterId controller, AnimatorId rig);
    /// The same, for a rig that spells its parameters its own way.
    void pair(PhysicsCharacterId controller, AnimatorId rig, Parameters names);
    /// Forget a pairing. Also happens by itself when either handle goes stale.
    void unpair(AnimatorId rig);
    void clear() { pairs.clear(); }

    /// Live pairs. Dead ones are dropped by `update`, so this is only exact after one.
    [[nodiscard]] uint32_t pairCount() const { return static_cast<uint32_t>(pairs.size()); }
    /// The vocabulary a pair gets when it names none of its own.
    [[nodiscard]] const Parameters& parameters() const { return defaultNames; }
    /// The vocabulary `rig` is actually driven by, or the default if it is not paired.
    [[nodiscard]] const Parameters& parameters(AnimatorId rig) const;
    /**
     * @brief Set the vocabulary for every pair, and for every pair made afterwards.
     *
     * Rewriting existing pairs too is what makes naming-then-pairing and pairing-then-naming
     * give the same answer; touching only the template reintroduces that order dependence.
     */
    void setParameters(Parameters p);
    /// One rig's, leaving every other pair alone.
    void setParameters(AnimatorId rig, Parameters p);

    /**
     * @brief Write this step's parameters for every live pair.
     *
     * Call after `PhysicsWorld::step`, never before. A `CharacterVirtual` that has not yet
     * been stepped still reports its constructed ground state, which reads as *in the air*,
     * and its authored transform rather than the solved one -- so running first makes every
     * character fall and land in the opening steps of a run with no input at all.
     */
    void update(const PhysicsWorld& physics, SceneAnimator& animator);

  private:
    struct Pair {
        PhysicsCharacterId controller;
        AnimatorId rig;
        Parameters names;
    };

    std::vector<Pair> pairs;
    /// The template a pair that names nothing starts with; `update` reads the pair's copy.
    Parameters defaultNames;
};

} // namespace scene
