#include "anim/Locomotion.h"

#include <algorithm>
#include <utility>

namespace anim {

void LocomotionDriver::pairKeyed(uint64_t controller, scene::AnimatorId rig, Parameters names) {
    if (!rig.valid()) return;
    for (Pair& p : pairs) {
        if (p.rig == rig) {
            // Controller only: also assigning `names` here would silently move a re-paired
            // rig off its own vocabulary and onto the caller's default.
            p.controller = controller;
            return;
        }
    }
    pairs.push_back({controller, rig, std::move(names)});
}

const LocomotionDriver::Parameters& LocomotionDriver::parameters(scene::AnimatorId rig) const {
    for (const Pair& p : pairs) {
        if (p.rig == rig) return p.names;
    }
    return defaultNames;
}

void LocomotionDriver::setParameters(Parameters p) {
    defaultNames = std::move(p);
    for (Pair& pair : pairs) pair.names = defaultNames;
}

void LocomotionDriver::setParameters(scene::AnimatorId rig, Parameters p) {
    for (Pair& pair : pairs) {
        if (pair.rig == rig) {
            pair.names = std::move(p);
            return;
        }
    }
}

void LocomotionDriver::unpair(scene::AnimatorId rig) {
    pairs.erase(std::remove_if(pairs.begin(), pairs.end(), [rig](const Pair& p) { return p.rig == rig; }),
                pairs.end());
}

void LocomotionDriver::update(const MotionSlot& motion, SceneAnimator& animator) {
    size_t live = 0;
    for (Pair& p : pairs) {
        scene::CharacterMotion moved;
        if (!motion(p.controller, &moved) || !animator.valid(p.rig)) continue;
        pairs[live++] = p;

        // Looked up per step, not cached at `pair` time: `setStateMachine` may run after a
        // pairing, and a cached index would then name a parameter of a machine that is gone.
        const scene::AnimationStateMachine& machine = animator.stateMachine(p.rig);
        const uint32_t speed = machine.findParameter(p.names.speed);
        const uint32_t airborne = machine.findParameter(p.names.airborne);
        const uint32_t jump = machine.findParameter(p.names.jump);

        if (speed != scene::kAnyState) {
            // The divisor is the collider's top speed, not a constant: a literal here would
            // pin every rig's blend thresholds to one collider's tuning.
            animator.setParameter(p.rig, speed,
                                  moved.topSpeed > 0.0f ? std::min(moved.speed / moved.topSpeed, 1.0f) : 0.0f);
        }
        if (airborne != scene::kAnyState) {
            animator.setParameter(p.rig, airborne, moved.onGround ? 0.0f : 1.0f);
        }
        if (jump != scene::kAnyState && moved.jumped) animator.fire(p.rig, jump);
    }
    pairs.resize(live);
}

} // namespace anim
