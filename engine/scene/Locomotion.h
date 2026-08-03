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
 * What is deliberately *not* here: any decision about what the character is trying to do.
 * This layer answers "what pose, given what the body did"; the layer that answers "what
 * should the body do" is a separate one — see `architecture/systems.md`.
 */

/**
 * @brief Writes a rig's locomotion parameters from what its controller actually did.
 *
 * A game used to do this itself, and it worked for exactly one rig: the parameter names,
 * the normalising divisor and the set of triggers all belong to the *rig*, so a second rig
 * meant a second copy of the loop with different constants in it.
 *
 * **Every value written here comes back out of the solver and none of it out of an input
 * map.** That is the property worth protecting: a driver that played `run` because a key was
 * down would satisfy "the character animates" and prove nothing. `characterJumped` in
 * particular cannot be reconstructed from the key and the ground state, because a coyote
 * window and a jump buffer make the two disagree by design.
 */
class LocomotionDriver {
  public:
    /**
     * @brief The names looked up in one rig's own machine.
     *
     * Strings, and the rig's. An engine that hard-coded `"speed"` would be an engine that
     * decides what a state machine may contain, and the whole point of a machine per
     * character is that two characters may run different ones. A name a machine does not
     * have is silently skipped: a rig with no `airborne` is a rig that does not care
     * whether it is in the air, not a misconfiguration.
     *
     * **Held per pair** (D19). One set for the whole driver made the paragraph above a
     * statement of intent rather than of fact: a second rig spelling its parameter `Speed`
     * got `kAnyState` and an animation that silently never blended, and naming the second
     * rig's parameters un-named the first's.
     */
    struct Parameters {
        std::string speed = "speed";       ///< 0..1, the controller's speed over its top speed
        std::string airborne = "airborne"; ///< 1 while not standing on anything
        std::string jump = "jump";         ///< fired on the step the controller launched
    };

    /// Pair a controller with the rig it animates, on the driver's current vocabulary.
    /// Pairing the same rig twice replaces, and keeps the names the pair already had.
    void pair(PhysicsCharacterId controller, AnimatorId rig);
    /// The same, for a rig that spells its parameters its own way -- a bestiary from three
    /// exporters, or anything imported from a marketplace.
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
     * **Both halves, which is what `SceneAnimator::setStateMachine` already does and for the
     * same reason.** A one-rig scene stays a one-call scene, and a game that pairs before it
     * names its parameters gets the same answer as one that names them first -- which is the
     * order dependence a template-only version would have bought.
     */
    void setParameters(Parameters p);
    /// One rig's, leaving every other pair alone. The call a second exporter needs.
    void setParameters(AnimatorId rig, Parameters p);

    /**
     * @brief Write this step's parameters for every live pair.
     *
     * **Call it after `PhysicsWorld::step` and not before.** Before the first step a
     * `CharacterVirtual` has never been asked to look at the world: its ground state is the
     * one it was constructed with, which reads as *in the air*, and its transform is the one
     * the file authored rather than the one the solver resolved. A driver reading either
     * makes every character fall and land in the first three steps of every run with nobody
     * touching a key. Running after the step means there is no such moment to guard against.
     *
     * A pair whose controller or rig has gone stale is dropped here rather than skipped, so
     * a world that destroys characters does not accumulate dead rows.
     */
    void update(const PhysicsWorld& physics, SceneAnimator& animator);

  private:
    struct Pair {
        PhysicsCharacterId controller;
        AnimatorId rig;
        Parameters names;
    };

    std::vector<Pair> pairs;
    /// What a pair that names nothing starts with. The template, not a second copy of the
    /// truth -- `update` reads the pair's and never this.
    Parameters defaultNames;
};

} // namespace scene
