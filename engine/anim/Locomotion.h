#pragma once

#include "anim/SceneAnimator.h"
#include "core/Handle.h"
#include "core/Slot.h"
#include "scene/AnimationRig.h"
#include "scene/CharacterMotion.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/**
 * @file engine/anim/Locomotion.h
 * @brief The wiring between a character controller and the state machine that animates it.
 *
 * Layering -- what decides a character's intent, and why that is not here -- is argued in
 * `architecture/systems.md`.
 */
namespace anim {

/**
 * @brief Writes a rig's locomotion parameters from what its controller actually did.
 *
 * Every value written here comes back out of the solver, never out of an input map.
 * `CharacterMotion::jumped` in particular cannot be reconstructed from the key and the
 * ground state: a coyote window and a jump buffer make the two disagree by design.
 */
class LocomotionDriver {
  public:
    /// Where a step's `scene::CharacterMotion` comes from, keyed by a packed controller
    /// handle. False retires the pairing, which is what drops a controller that is gone.
    using MotionSlot = core::Slot<bool(uint64_t, scene::CharacterMotion*)>;

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

    /**
     * @brief Pair a controller with the rig it animates, on the driver's current vocabulary.
     *
     * Re-pairing a rig keeps the names that pair already had.
     *
     * **A template over the handle rather than a `PhysicsCharacterId` parameter**: naming
     * that type here is one module reaching into another, and the driver never dereferences
     * the handle -- it hands it straight back to the motion slot. The tag still makes the
     * round trip type-safe at each end.
     */
    template <typename Tag>
    void pair(core::Handle<Tag> controller, scene::AnimatorId rig) {
        pair(controller, rig, defaultNames);
    }
    /// The same, for a rig that spells its parameters its own way.
    template <typename Tag>
    void pair(core::Handle<Tag> controller, scene::AnimatorId rig, Parameters names) {
        if (!controller.valid()) return;
        pairKeyed(core::packHandle(controller), rig, std::move(names));
    }
    /// The same, for a controller already flattened by `core::packHandle` -- which is the
    /// only shape `engine/Modules.h` can carry, since it may not name a controller's tag
    /// either.
    void pairKeyed(uint64_t controller, scene::AnimatorId rig) { pairKeyed(controller, rig, defaultNames); }
    void pairKeyed(uint64_t controller, scene::AnimatorId rig, Parameters names);
    /// Forget a pairing. Also happens by itself when either handle goes stale.
    void unpair(scene::AnimatorId rig);
    void clear() { pairs.clear(); }

    /// Live pairs. Dead ones are dropped by `update`, so this is only exact after one.
    [[nodiscard]] uint32_t pairCount() const { return static_cast<uint32_t>(pairs.size()); }
    /// The vocabulary a pair gets when it names none of its own.
    [[nodiscard]] const Parameters& parameters() const { return defaultNames; }
    /// The vocabulary `rig` is actually driven by, or the default if it is not paired.
    [[nodiscard]] const Parameters& parameters(scene::AnimatorId rig) const;
    /**
     * @brief Set the vocabulary for every pair, and for every pair made afterwards.
     *
     * Rewriting existing pairs too is what makes naming-then-pairing and pairing-then-naming
     * give the same answer; touching only the template reintroduces that order dependence.
     */
    void setParameters(Parameters p);
    /// One rig's, leaving every other pair alone.
    void setParameters(scene::AnimatorId rig, Parameters p);

    /**
     * @brief Write this step's parameters for every live pair.
     *
     * Call after `PhysicsWorld::step`, never before. A `CharacterVirtual` that has not yet
     * been stepped still reports its constructed ground state, which reads as *in the air*,
     * and its authored transform rather than the solved one -- so running first makes every
     * character fall and land in the opening steps of a run with no input at all.
     */
    void update(const MotionSlot& motion, SceneAnimator& animator);

  private:
    struct Pair {
        uint64_t controller = 0;
        scene::AnimatorId rig;
        Parameters names;
    };

    std::vector<Pair> pairs;
    /// The template a pair that names nothing starts with; `update` reads the pair's copy.
    Parameters defaultNames;
};

} // namespace anim
