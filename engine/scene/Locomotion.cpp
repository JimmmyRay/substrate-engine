#include "scene/Locomotion.h"

#include <algorithm>
#include <utility>

namespace scene {

void LocomotionDriver::pair(PhysicsCharacterId controller, AnimatorId rig) {
    pair(controller, rig, defaultNames);
}

void LocomotionDriver::pair(PhysicsCharacterId controller, AnimatorId rig, Parameters names) {
    if (!controller.valid() || !rig.valid()) return;
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

const LocomotionDriver::Parameters& LocomotionDriver::parameters(AnimatorId rig) const {
    for (const Pair& p : pairs) {
        if (p.rig == rig) return p.names;
    }
    return defaultNames;
}

void LocomotionDriver::setParameters(Parameters p) {
    defaultNames = std::move(p);
    for (Pair& pair : pairs) pair.names = defaultNames;
}

void LocomotionDriver::setParameters(AnimatorId rig, Parameters p) {
    for (Pair& pair : pairs) {
        if (pair.rig == rig) {
            pair.names = std::move(p);
            return;
        }
    }
}

void LocomotionDriver::unpair(AnimatorId rig) {
    pairs.erase(std::remove_if(pairs.begin(), pairs.end(), [rig](const Pair& p) { return p.rig == rig; }),
                pairs.end());
}

void LocomotionDriver::update(const PhysicsWorld& physics, SceneAnimator& animator) {
    size_t live = 0;
    for (Pair& p : pairs) {
        if (!physics.valid(p.controller) || !animator.valid(p.rig)) continue;
        pairs[live++] = p;

        // Looked up per step, not cached at `pair` time: `setStateMachine` may run after a
        // pairing, and a cached index would then name a parameter of a machine that is gone.
        const AnimationStateMachine& machine = animator.stateMachine(p.rig);
        const uint32_t speed = machine.findParameter(p.names.speed);
        const uint32_t airborne = machine.findParameter(p.names.airborne);
        const uint32_t jump = machine.findParameter(p.names.jump);

        if (speed != kAnyState) {
            // The divisor is the collider's top speed, not a constant: a literal here would
            // pin every rig's blend thresholds to one collider's tuning.
            const float top = physics.characterMoveSpeed(p.controller);
            const float carried = physics.characterSpeed(p.controller);
            animator.setParameter(p.rig, speed, top > 0.0f ? std::min(carried / top, 1.0f) : 0.0f);
        }
        if (airborne != kAnyState) {
            animator.setParameter(p.rig, airborne, physics.characterOnGround(p.controller) ? 0.0f : 1.0f);
        }
        if (jump != kAnyState && physics.characterJumped(p.controller)) animator.fire(p.rig, jump);
    }
    pairs.resize(live);
}

} // namespace scene
