#include "scene/Simulation.h"

#include "core/Profiler.h"

namespace scene {

const std::vector<glm::mat4>& Simulation::poseFor(uint32_t node) const {
    const AnimatorId owner = animator.characterForNode(node);
    return animator.worldTransforms(owner.valid() ? owner : animator.characterAt(0));
}

void Simulation::step(float stepSeconds) {
    auto s = core::Profiler::scope("simulate");

    // **Called unconditionally, and the `!empty()` guards that used to stand here are
    // gone.** Each of the four costs a named zero on a scene that has none of what it
    // moves, which is what lets `simulate` be summed against its children and a shortfall
    // read as work no zone names -- a guard at the call site would make the row disappear
    // instead, and a missing row and a free system look identical in the table. Every one
    // of them already returned early on an empty world; the guard was duplicating that
    // test one frame further out.
    animator.update(stepSeconds);

    // P5, beside the scene animator and on the same clock, which is what makes a paused
    // game have paused sprites and a time scale slow them without either being written
    // twice (C4). `update` clears the fired-event list, and a game reading `firedEvents()`
    // after a step where nothing was playing has to be handed an empty list rather than
    // the last one that was not -- so this one was unguarded before the other three were.
    sprites.update(stepSeconds);

    particles.update(stepSeconds);

    // Last of the three, so a body pushed by nothing this step is still where the
    // animation left it. `step` rather than `dt` for the sharpest version of the reason
    // the other two give: a solver integrated against a variable delta is
    // non-deterministic by construction.
    physics.step(stepSeconds);

    // **After the step and not before it** (G15). Every parameter this writes comes back
    // out of the solver, so it has to be read from a solver that has looked at the world:
    // before the first step a `CharacterVirtual`'s ground state is the one it was
    // constructed with, which reads as in the air. The animator picks the values up on the
    // *next* step's `SceneAnimator::update` above -- one step of latency, uniformly, which
    // is what keeps a machine's view of the world self-consistent.
    {
        auto ls = core::Profiler::scope("LocomotionDriver::update");
        locomotion.update(physics, animator);
    }

    // ------------------------------------------------------------------ audio (S5)
    // Last, and on the same clock as the other three movers: a mixer advanced by
    // wall-clock time would make a duck and an occlusion sweep functions of the frame
    // rate. Nothing here can change the rendered frame -- audio reads the camera and the
    // physics world and writes to a mix buffer.
    // **`step` does not return early**, and the `audioSources` scope is braced. It used to
    // be a function-lifetime scope opened before the early-out's two successors, so it
    // enclosed `audioOcclusion` *and* the mixer update and reported all three under one
    // name -- while a run with audio off showed none of the three at all. A named zero says
    // "this system did nothing"; a missing row says nothing whatever.
    const bool audioLive = audio.active() && !audio.empty();

    {
        auto sa = core::Profiler::scope("audioSources");

        // A source placed by an animated node follows it, exactly as an emitter does
        // (S3.1) -- a bell on a walking character.
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

    // A source on a body follows the body, and since G3 that happens in `endFrame` with
    // everything else the scene tree pushes. **This used to read the body at alpha 1
    // rather than at the frame's alpha**, on the grounds that the mixer wants where a
    // source is now rather than where it is being drawn. The tree gives one transform per
    // node per frame, and having audio see a different one than the image would put back
    // exactly the divergence the tree exists to remove -- so a source now hears itself
    // where it is drawn, and the occlusion pass below reads a position set at the end of
    // the previous frame. Both differences are one step of motion, which is a sixtieth of
    // a second of a sound's position.

    // Occlusion (S5.5). One ray per occludable source, from the listener, stopping short
    // of both ends: the listener stands inside its own character's capsule and a source
    // usually sits at the centre of the body it rides, so a ray drawn all the way to both
    // would report every source as occluded by the thing it is attached to.
    {
        auto so = core::Profiler::scope("audioOcclusion");
        if (audioLive && occlusion && !physics.empty()) {
            for (uint32_t slot = 0; slot < audio.sourceCount(); ++slot) {
                const SoundId i = audio.soundAt(slot);
                if (!audio.occludable(i)) continue;
                const glm::vec3 to = audio.sourcePosition(i);
                const BodyId ignore = slot < sourceBody.size() ? sourceBody[slot] : BodyId{};

                // **Occluded only where every listener is behind something** (C28). The
                // filter is one biquad on one voice and there is no per-listener version of
                // it, so the question a ray can answer is which way to resolve a source two
                // players disagree about. Muffling one that the second player can see
                // plainly is the worse mistake -- and it is the one an unchanged sweep
                // reading listener 0 would make on every frame of a split screen.
                bool blocked = true;
                for (uint32_t ears = 0; ears < audio.listenerCount() && blocked; ++ears) {
                    const glm::vec3 ear = audio.listenerPosition(ears);
                    const glm::vec3 delta = to - ear;
                    const float distance = glm::length(delta);
                    // Closer than the two margins is a source the listener is effectively
                    // standing on, and there is no segment left to test once both ends are
                    // trimmed.
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

    // Unconditional now, where the early-out above used to skip it. `update` returns on
    // `!impl->running` of its own accord, so an engine with no device costs the named zero
    // its scope records; an engine that is running but holds no sources now advances its
    // bus fades, which it always should have.
    audio.update(stepSeconds);
}

} // namespace scene
