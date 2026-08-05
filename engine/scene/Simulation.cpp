#include "scene/Simulation.h"

#include "Modules.h"
#include "core/Profiler.h"

namespace scene {

void Simulation::step(float stepSeconds) {
    auto s = core::Profiler::scope("simulate");

    // The four movers are called unconditionally; each already returns early on an empty
    // world. Re-adding an `!empty()` guard here deletes the profiler row instead of zeroing
    // it, and a missing row is indistinguishable from a system that cost nothing.
    modules::anim->update(stepSeconds);

    // `update` clears the fired-event list, so skipping it hands a game that reads
    // `firedEvents()` after a quiet step the previous step's list.
    sprites.update(stepSeconds);

    modules::particles->update(stepSeconds);

    // Last of the three, so a body nothing pushed this step is still where the animation
    // left it.
    modules::physics->step(stepSeconds);

    // After the step, never before: a `CharacterVirtual` that has not been stepped reports
    // its constructed ground state, which reads as in the air. The animator picks these up on
    // the *next* step, so every parameter carries one step of latency uniformly.
    {
        auto ls = core::Profiler::scope("LocomotionDriver::update");
        modules::anim->updateLocomotion(modules::physics->characterMotion());
    }

    // Audio runs on the fixed step like the other movers; a mixer advanced by wall-clock
    // time makes ducking and occlusion functions of the frame rate.
    //
    // Each of the three calls below gates itself on a running mixer with sources in it.
    // Hoisting that into one condition around the scopes, or unbracing either scope,
    // collapses three profiler rows into one that is also wrong.
    {
        auto sa = core::Profiler::scope("audioSources");

        if (!modules::anim->stats().empty) modules::audio->placeSources(modules::anim->poses());
    }

    // A source on a body is moved by the scene tree in `endFrame`, at the frame's alpha --
    // reading the body here at alpha 1 instead would give audio a different transform than
    // the image, which is the divergence the tree exists to remove. The sweep below
    // therefore reads a position set at the end of the previous frame.
    {
        auto so = core::Profiler::scope("audioOcclusion");
        if (!modules::physics->stats().empty) {
            modules::audio->updateOcclusion(modules::physics->segmentBlocked());
        }
    }

    // Unconditional, and not under a source count: a running mixer that holds no sources
    // still has bus fades to advance.
    modules::audio->update(stepSeconds);
}

} // namespace scene
