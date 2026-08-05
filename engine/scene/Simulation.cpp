#include "scene/Simulation.h"

#include "core/Profiler.h"

namespace scene {

const std::vector<glm::mat4>& Simulation::poseFor(uint32_t node) const {
    const AnimatorId owner = animator.characterForNode(node);
    return animator.worldTransforms(owner.valid() ? owner : animator.characterAt(0));
}

void Simulation::step(float stepSeconds) {
    auto s = core::Profiler::scope("simulate");

    // The four movers are called unconditionally; each already returns early on an empty
    // world. Re-adding an `!empty()` guard here deletes the profiler row instead of zeroing
    // it, and a missing row is indistinguishable from a system that cost nothing.
    animator.update(stepSeconds);

    // `update` clears the fired-event list, so skipping it hands a game that reads
    // `firedEvents()` after a quiet step the previous step's list.
    sprites.update(stepSeconds);

    particles.update(stepSeconds);

    // Last of the three, so a body nothing pushed this step is still where the animation
    // left it.
    physics.step(stepSeconds);

    // After the step, never before: a `CharacterVirtual` that has not been stepped reports
    // its constructed ground state, which reads as in the air. The animator picks these up on
    // the *next* step, so every parameter carries one step of latency uniformly.
    {
        auto ls = core::Profiler::scope("LocomotionDriver::update");
        locomotion.update(physics, animator);
    }

    // Audio runs on the fixed step like the other movers; a mixer advanced by wall-clock
    // time makes ducking and occlusion functions of the frame rate.
    // `audioLive` gates the bodies below, not the scopes: turning it into an early return, or
    // unbracing either scope, collapses three profiler rows into one that is also wrong.
    const bool audioLive = audio.active() && !audio.empty();

    {
        auto sa = core::Profiler::scope("audioSources");

        if (audioLive && !animator.empty()) {
            for (uint32_t slot = 0; slot < audio.sourceCount(); ++slot) {
                const SoundId id = audio.soundAt(slot);
                if (!id.valid()) continue;
                const uint32_t node = audio.source(id).node;
                const std::vector<glm::mat4>& world = poseFor(node);
                if (node < world.size()) audio.setSourceTransform(id, world[node]);
            }
        }
    }

    // A source on a body is moved by the scene tree in `endFrame`, at the frame's alpha --
    // reading the body here at alpha 1 instead would give audio a different transform than
    // the image, which is the divergence the tree exists to remove. The sweep below
    // therefore reads a position set at the end of the previous frame.

    // One ray per occludable source, stopping short of both ends: the listener stands inside
    // its own character's capsule and a source usually sits at the centre of the body it
    // rides, so an untrimmed ray reports every source as occluded by what it is attached to.
    {
        auto so = core::Profiler::scope("audioOcclusion");
        if (audioLive && occlusion && !physics.empty()) {
            for (uint32_t slot = 0; slot < audio.sourceCount(); ++slot) {
                const SoundId i = audio.soundAt(slot);
                if (!audio.occludable(i)) continue;
                const glm::vec3 to = audio.sourcePosition(i);
                const BodyId ignore = slot < sourceBody.size() ? sourceBody[slot] : BodyId{};

                // Occluded only where *every* listener is behind something. The filter is one
                // biquad on one voice, so sweeping from listener 0 alone would muffle a
                // source the second player of a split screen can see plainly.
                bool blocked = true;
                for (uint32_t ears = 0; ears < audio.listenerCount() && blocked; ++ears) {
                    const glm::vec3 ear = audio.listenerPosition(ears);
                    const glm::vec3 delta = to - ear;
                    const float distance = glm::length(delta);
                    // Two margins is the whole segment: trimming both ends of anything
                    // shorter inverts it, and the sweep would test a backwards ray.
                    if (distance <= occlusionMargin * 2.0f) {
                        blocked = false;
                        break;
                    }
                    const glm::vec3 direction = delta / distance;
                    blocked = physics.segmentBlocked(ear + direction * occlusionMargin,
                                                     to - direction * occlusionMargin, ignore);
                }
                audio.setOccluded(i, blocked);
            }
        }
    }

    // Unconditional, and not under `audioLive`: a running engine that holds no sources still
    // has bus fades to advance.
    audio.update(stepSeconds);
}

} // namespace scene
