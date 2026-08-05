#include "particles/ParticleSystem.h"

#include "core/Logger.h"
#include "core/Profiler.h"

#include <algorithm>
#include <cmath>

namespace particles {

using scene::ParticleEmitter;

void ParticleSystem::setEmitters(std::vector<ParticleEmitter> emitters, uint32_t budget) {
    emitterList = std::move(emitters);
    // Generations are carried forward and bumped, never reset: a handle into the
    // replaced list must stop validating, and resetting to 1 would do the opposite --
    // old slot 0 at generation 1 would validate against new slot 0 at generation 1.
    const size_t previous = slots.size();
    slots.resize(std::max(previous, emitterList.size()));
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i < previous) ++slots[i].generation;
        slots[i].live = i < emitterList.size();
    }
    slots.resize(emitterList.size());
    for (ParticleEmitter& e : emitterList) {
        e.accumulator = 0.0f;
        e.emitted = 0;
        // Authored values, so clamped rather than trusted: a non-positive lifetime is a
        // particle born dead into a slot nothing ever frees, and a negative rate runs the
        // accumulator backwards.
        e.lifetime = std::max(e.lifetime, 1.0f / 1024.0f);
        e.rate = std::max(e.rate, 0.0f);
        e.lifetimeJitter = std::clamp(e.lifetimeJitter, 0.0f, 1.0f);
    }

    // The budget is a floor, not a ceiling. Clamping the derived figure down to it would
    // only defer the same allocation to the first step, since the pool grows anyway.
    const uint32_t required = requiredCapacity(emitterList);
    poolCapacity = std::min(std::max(required, budget), kMaxCapacity);

    // The sort's domain has to be a power of two, and a budget is an arbitrary number.
    // Rounded up: rounding down would put the pool back under what the emitters need.
    if (poolCapacity != 0) {
        uint32_t p = 1;
        while (p < poolCapacity && p < kMaxCapacity) p *= 2u;
        poolCapacity = std::min(p, kMaxCapacity);
    }

    deathTime.assign(poolCapacity, 0.0f);
    freeSlots.clear();
    freeSlots.reserve(poolCapacity);
    spawnList.clear();
    spawnList.reserve(poolCapacity);
    alive = 0;
    dropped = 0;
    now = 0.0f;
    lastStep = 0.0f;
}

uint32_t ParticleSystem::wantedCapacity() const {
    // A retired slot keeps its `ParticleEmitter` until something overwrites it, so
    // summing `emitterList` whole would hold the pool at the high-water mark of
    // everything that ever ran.
    std::vector<ParticleEmitter> live;
    live.reserve(emitterList.size());
    for (size_t i = 0; i < emitterList.size(); ++i) {
        if (i < slots.size() && slots[i].live) live.push_back(emitterList[i]);
    }
    return requiredCapacity(live);
}

bool ParticleSystem::grow(uint32_t atLeast) {
    if (atLeast <= poolCapacity || poolCapacity >= kMaxCapacity) return false;

    // The sort's domain has to be a power of two, so this rounds up.
    uint32_t next = std::max(poolCapacity, 1u);
    while (next < atLeast && next < kMaxCapacity) next *= 2u;
    next = std::min(next, kMaxCapacity);
    if (next <= poolCapacity) return false;

    // Resized, not reset: `deathTime` is the whole of the pool's CPU-side state, so
    // growing at the tail leaves every particle in flight where it was and the new slots
    // read as dead. Reassigning instead kills them all mid-flight.
    deathTime.resize(next, 0.0f);
    freeSlots.reserve(next);
    spawnList.reserve(next);
    poolCapacity = next;
    return true;
}

uint32_t ParticleSystem::requiredCapacity(const std::vector<ParticleEmitter>& emitters) {
    double total = 0.0;
    for (const ParticleEmitter& e : emitters) {
        // Plus one, because an emitter slower than one particle per frame still emits
        // one eventually, and a pool of zero would refuse it forever.
        total += std::ceil(static_cast<double>(std::max(e.rate, 0.0f)) * static_cast<double>(e.maxLifetime())) + 1.0;
    }
    if (total <= 0.0) return 0;

    const auto needed = static_cast<uint64_t>(total);
    uint64_t p = 1;
    while (p < needed && p < ParticleSystem::kMaxCapacity) p *= 2u;
    return static_cast<uint32_t>(std::min<uint64_t>(p, ParticleSystem::kMaxCapacity));
}

EmitterId ParticleSystem::create(ParticleEmitter emitter) {
    emitter.accumulator = 0.0f;
    emitter.emitted = 0;
    // The clamps setEmitters applies, for the same reasons.
    emitter.lifetime = std::max(emitter.lifetime, 1.0f / 1024.0f);
    emitter.rate = std::max(emitter.rate, 0.0f);
    emitter.lifetimeJitter = std::clamp(emitter.lifetimeJitter, 0.0f, 1.0f);

    // Computed unclamped, so the test below can actually fail: `requiredCapacity` returns
    // `min(need, kMaxCapacity)`, and comparing that against `kMaxCapacity` refuses nothing.
    // Over-subscription against the emitters already running is not checked here; that is
    // `droppedSpawns()`, counted per birth in update() where the free list is known.
    const double needed = emitter.burst > 0
                              ? static_cast<double>(emitter.burst)
                              : std::ceil(static_cast<double>(std::max(emitter.rate, 0.0f)) *
                                          static_cast<double>(emitter.maxLifetime())) +
                                    1.0;
    if (needed > static_cast<double>(kMaxCapacity)) {
        ++dropped;
        core::Logger::warn(core::LogCategory::Scene,
                           "Particle emitter '%s': refused, it needs %.0f particles and the sort key addresses %u",
                           emitter.name.c_str(), needed, kMaxCapacity);
        return {};
    }

    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].live) continue;
        slots[i].live = true;
        slots[i].expiresAt = -1.0f;
        slots[i].burstDone = false;
        emitterList[i] = std::move(emitter);
        return EmitterId{static_cast<uint32_t>(i), slots[i].generation};
    }

    const auto slot = static_cast<uint32_t>(emitterList.size());
    emitterList.push_back(std::move(emitter));
    slots.emplace_back();
    return EmitterId{slot, slots[slot].generation};
}

EmitterId ParticleSystem::spawnEffect(ParticleEmitter effect, const glm::vec3& position, const glm::vec3& normal) {
    // A caller that left this at zero asked for an effect and would get an emitter that
    // never stops -- the one outcome a fire-and-forget call must not have.
    if (effect.burst == 0) effect.burst = 16;

    // Any perpendicular will do for the other two axes: the emitter's cone is symmetric
    // about +Y, so their roll is not observable.
    glm::mat4 basis(1.0f);
    const float length = glm::length(normal);
    if (length > 1e-6f) {
        const glm::vec3 up = normal / length;
        // The world axis least parallel to `up`, so the cross product is well conditioned.
        const glm::vec3 seed = std::abs(up.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 right = glm::normalize(glm::cross(seed, up));
        const glm::vec3 forward = glm::cross(up, right);
        basis[0] = glm::vec4(right, 0.0f);
        basis[1] = glm::vec4(up, 0.0f);
        basis[2] = glm::vec4(forward, 0.0f);
    }
    basis[3] = glm::vec4(position, 1.0f);
    effect.transform = basis;

    return create(std::move(effect));
}

void ParticleSystem::destroy(EmitterId id) {
    if (!valid(id)) return;

    slots[id.index].live = false;
    ++slots[id.index].generation;
    // Particles already in flight are left to expire; killing them is a visible pop, and
    // the pool reclaims their slots when they die anyway.
    emitterList[id.index].rate = 0.0f;
    emitterList[id.index].accumulator = 0.0f;
}

void ParticleSystem::update(float dt) {
    // `zone` rather than the `s` every other scope in the tree uses: the spawn loop below
    // has a `GpuSpawn s`, and shadowing it is a warning this build treats as an error.
    auto zone = core::Profiler::scope("ParticleSystem::update");
    lastStep = dt;
    spawnList.clear();
    if (poolCapacity == 0) {
        alive = 0;
        return;
    }

    now += dt;

    // A linear pass rather than an expiry queue, because it yields the free list in
    // ascending slot order -- which is what makes the whole system reproducible.
    freeSlots.clear();
    alive = 0;
    for (uint32_t i = 0; i < poolCapacity; ++i) {
        if (deathTime[i] > now) {
            ++alive;
        } else {
            freeSlots.push_back(i);
        }
    }

    // Before this frame's births, so a slot freed here is available to the next create().
    for (uint32_t e = 0; e < slots.size(); ++e) {
        if (!slots[e].live || slots[e].expiresAt < 0.0f || slots[e].expiresAt > now) continue;
        destroy(EmitterId{e, slots[e].generation});
    }

    size_t nextFree = 0;
    for (uint32_t e = 0; e < emitterList.size(); ++e) {
        if (!slots[e].live) continue;
        ParticleEmitter& em = emitterList[e];

        uint32_t births;
        if (em.burst > 0) {
            if (slots[e].burstDone) continue;
            slots[e].burstDone = true;
            births = em.burst;
            slots[e].expiresAt = now + em.lifetime * (1.0f + em.lifetimeJitter);
        } else {
            em.accumulator += em.rate * dt;
            if (em.accumulator < 1.0f) continue;
            births = static_cast<uint32_t>(em.accumulator);
            em.accumulator -= static_cast<float>(births);
        }

        for (uint32_t k = 0; k < births; ++k) {
            if (nextFree >= freeSlots.size()) {
                // Counted rather than logged from here: an emitter over budget would
                // print sixty lines a second.
                dropped += births - k;
                break;
            }

            const uint32_t slot = freeSlots[nextFree++];
            const uint32_t seed = em.emitted++;

            // Symmetric about the authored lifetime: a one-sided jitter shifts the mean,
            // and the mean is what the pool was sized from.
            const float r = gfx::particleRandom(seed, gfx::kRandomLifetime) * 2.0f - 1.0f;
            const float lifetime = std::max(em.lifetime * (1.0f + em.lifetimeJitter * r), 1.0f / 1024.0f);

            // Births spread across the step. Stacked on its first instant instead, a
            // 600/s emitter lays ten particles at one point per frame and the jet reads
            // as a string of beads.
            const float age = dt * (static_cast<float>(births - 1 - k) / static_cast<float>(births));

            const float birth = now - age;
            deathTime[slot] = birth + lifetime;
            ++alive;

            gfx::GpuSpawn s;
            s.meta = glm::uvec4(slot, e, seed, 0u);
            s.params = glm::vec4(lifetime, birth, 0.0f, 0.0f);
            spawnList.push_back(s);
        }
    }
}

void ParticleSystem::writeGpuEmitters(gfx::GpuEmitter* out) const {
    for (size_t i = 0; i < emitterList.size(); ++i) {
        const ParticleEmitter& e = emitterList[i];
        gfx::GpuEmitter& g = out[i];
        g.transform = e.transform;
        g.velocity = glm::vec4(e.velocity, e.speedJitter);
        g.boxExtent = glm::vec4(e.boxExtent, e.coneAngle);
        g.gravity = glm::vec4(e.gravity, e.drag);
        g.colorStart = e.colorStart;
        g.colorEnd = e.colorEnd;
        g.params = glm::vec4(e.sizeStart, e.sizeEnd, e.restitution, e.emissiveIntensity);
        g.sprite = glm::vec4(e.spin, e.erosion, e.flipbookLoops, 0.0f);

        uint32_t bits = 0;
        if (e.emissive) bits |= scene::kEmitterEmissive;
        if (e.collides) bits |= scene::kEmitterCollides;
        // Clamped to at least one cell each way. A grid of zero would make the frame
        // count zero, and the shader divides by it.
        const uint32_t cols = std::max(e.flipbookCols, 1u);
        const uint32_t rows = std::max(e.flipbookRows, 1u);
        g.flags = glm::uvec4(e.texture, bits, cols | (rows << 16), 0u);
    }
}

} // namespace particles
