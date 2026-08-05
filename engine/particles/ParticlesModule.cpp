#include "particles/ParticlesModule.h"

#include "Engine.h"
#include "Modules.h"

#include <utility>

namespace particles {

namespace {

/// The engine's one particle system. File-scope rather than an `Engine` member, which
/// would name `particles::ParticleSystem` in `Engine.h` and link particles into every
/// game.
ParticleSystem g_particles;

struct Module final : modules::Particles {
    void setEmitters(std::vector<scene::ParticleEmitter> emitters, uint32_t budget) override {
        g_particles.setEmitters(std::move(emitters), budget);
    }

    void addEmitter(const scene::ParticleEmitter& emitter) override { (void)g_particles.create(emitter); }

    void update(float dt) override { g_particles.update(dt); }

    bool growToWanted() override {
        const uint32_t want = g_particles.wantedCapacity();
        if (want <= g_particles.capacity()) return false;
        return g_particles.grow(want);
    }

    [[nodiscard]] Pool pool() const override { return {g_particles.capacity(), g_particles.emitterCount()}; }

    void placeEmitters(const core::Slot<bool(uint32_t, glm::mat4*)>& poseOf) override {
        for (scene::ParticleEmitter& e : g_particles.emitters()) {
            glm::mat4 world(1.0f);
            if (poseOf(e.node, &world)) e.transform = world;
        }
    }

    [[nodiscard]] core::Slot<void(uint32_t, const glm::mat4&)> emitterTransforms() override {
        return core::Slot<void(uint32_t, const glm::mat4&)>::bind<&ParticleSystem::setEmitterTransform>(&g_particles);
    }

    [[nodiscard]] gfx::ParticleFrame frame() const override {
        gfx::ParticleFrame f;
        f.spawns = g_particles.spawns();
        f.emitterCount = g_particles.emitterCount();
        f.writeEmitters = core::Slot<void(gfx::GpuEmitter*)>::bind<&ParticleSystem::writeGpuEmitters>(&g_particles);
        f.alive = g_particles.aliveCount();
        f.dropped = g_particles.droppedSpawns();
        f.now = g_particles.time();
        f.dt = g_particles.step();
        return f;
    }
};

Module g_module;

/// Assign `modules::particles` from a header instead and any transitive include links
/// particles into a game that never asked for them.
struct Registrar {
    Registrar() { modules::particles = &g_module; }
};

const Registrar g_registrar;

} // namespace

} // namespace particles

// Defining this in Engine.cpp instead links particles into every binary -- Engine.cpp is in
// all of them and this file is not.
::particles::ParticleSystem& Engine::particles() {
    return ::particles::g_particles;
}
