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
            // The controller only. Re-pairing is what a game does when a rig gets a new
            // body, and dropping the vocabulary on the way through would silently move a
            // second exporter's rig back onto the first one's names.
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

        // Looked up per step rather than cached at `pair` time, and the reason is not
        // laziness: `setStateMachine` may run at any point after a pairing, and a cached
        // index would then name a parameter of the machine that has gone. Three string
        // compares against a machine with a handful of parameters is not a cost worth a
        // staleness rule.
        const AnimationStateMachine& machine = animator.stateMachine(p.rig);
        const uint32_t speed = machine.findParameter(p.names.speed);
        const uint32_t airborne = machine.findParameter(p.names.airborne);
        const uint32_t jump = machine.findParameter(p.names.jump);

        if (speed != kAnyState) {
            // **`speed / moveSpeed`, and the divisor is the collider's rather than a
            // constant.** A game writing `speed / 4.0` is asserting that the machine's
            // `run` threshold sits at 4 m/s -- a fact about the *rig's* thresholds and the
            // *collider's* top speed, two things the engine holds and the game had to guess
            // consistently with. Normalised here, a rig authored against a different top
            // speed works without editing a game.
            const float top = physics.characterMoveSpeed(p.controller);
            const float carried = physics.characterSpeed(p.controller);
            animator.setParameter(p.rig, speed, top > 0.0f ? std::min(carried / top, 1.0f) : 0.0f);
        }
        if (airborne != kAnyState) {
            animator.setParameter(p.rig, airborne, physics.characterOnGround(p.controller) ? 0.0f : 1.0f);
        }
        // A jump the controller could not make must not animate one, and a jump it made a
        // step late must animate a step late.
        if (jump != kAnyState && physics.characterJumped(p.controller)) animator.fire(p.rig, jump);
    }
    pairs.resize(live);
}

} // namespace scene
