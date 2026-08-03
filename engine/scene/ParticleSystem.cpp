#include "scene/ParticleSystem.h"

#include "core/Json.h"
#include "core/Logger.h"
#include "core/Profiler.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scene {

namespace {

using rapidjson::Value;
// Shared with Config.cpp and Collider.cpp since S4.2. They were duplicated here while
// there were two of them, which was the Rule of Threes applied literally; the third
// occurrence is what moved them to core/Json.h.
using core::json::gltfJsonSpan;
using core::json::member;
using core::json::readAngleDegrees;
using core::json::readBool;
using core::json::readFloat;
using core::json::readString;
using core::json::readUint;
using core::json::readVec;

// `gltfJsonSpan` was written here first (S3.1) and copied to Collider.cpp (S4.2)
// deliberately, two occurrences being a coincidence. AudioSource.cpp (S5.2) is the third
// and moved it to core/Json.h, alongside the readers above that got there the same way.

} // namespace

void ParticleSystem::setEmitters(std::vector<ParticleEmitter> emitters, uint32_t budget) {
    emitterList = std::move(emitters);
    // Generations are *carried forward and bumped*, not reset. This is a whole-list
    // replacement, so every handle into the previous list has to stop validating -- and
    // resetting to 1 would do the opposite, because a handle to old slot 0 at generation
    // 1 would validate against new slot 0 at generation 1. Slots the new list does not
    // reach keep their counter for the same reason.
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
        // A non-positive lifetime is a particle that is born dead and a slot that is
        // never freed, so it is clamped rather than trusted. Same for the rate: a
        // negative rate would run the accumulator backwards.
        e.lifetime = std::max(e.lifetime, 1.0f / 1024.0f);
        e.rate = std::max(e.rate, 0.0f);
        e.lifetimeJitter = std::clamp(e.lifetimeJitter, 0.0f, 1.0f);
    }

    // **The budget is a floor, not a ceiling** (C40). It used to clamp the derived figure
    // down and warn that emission would be refused once the pool filled; the pool grows now,
    // so clamping down would only mean rebuilding on the first step and the warning described
    // a refusal that cannot happen. What a stated budget buys is allocating up front instead.
    const uint32_t required = requiredCapacity(emitterList);
    poolCapacity = std::min(std::max(required, budget), kMaxCapacity);

    // The sort's domain has to be a power of two, and a budget is an arbitrary number.
    // Rounded **up**: this is a floor, and rounding down would put the pool back under what
    // the emitters need.
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
    // The live emitters only. A retired slot keeps its `ParticleEmitter` until something
    // overwrites it, so summing `emitterList` whole would hold the pool at the high-water
    // mark of everything that ever ran.
    std::vector<ParticleEmitter> live;
    live.reserve(emitterList.size());
    for (size_t i = 0; i < emitterList.size(); ++i) {
        if (i < slots.size() && slots[i].live) live.push_back(emitterList[i]);
    }
    return requiredCapacity(live);
}

bool ParticleSystem::grow(uint32_t atLeast) {
    if (atLeast <= poolCapacity || poolCapacity >= kMaxCapacity) return false;

    // The sort's domain has to be a power of two, so this rounds *up* -- the opposite of
    // `setEmitters`, which rounds down because it is clamping to a ceiling a caller stated.
    // Nothing is being clamped here; the pool is being made big enough.
    uint32_t next = std::max(poolCapacity, 1u);
    while (next < atLeast && next < kMaxCapacity) next *= 2u;
    next = std::min(next, kMaxCapacity);
    if (next <= poolCapacity) return false;

    // **Resized, not reset.** `deathTime` is the whole of the pool's CPU-side state and a
    // live slot is one whose entry is still in the future, so growing at the tail leaves
    // every particle in flight exactly where it was; the new slots read as dead, which is
    // what a zero death time means. The GPU half is the renderer's to carry across, and
    // `Engine` pairs the two so a caller cannot do one without the other.
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
    // The same clamps setEmitters applies, and for the same reasons: a non-positive
    // lifetime is a particle born dead into a slot that is never freed, and a negative
    // rate runs the accumulator backwards.
    emitter.lifetime = std::max(emitter.lifetime, 1.0f / 1024.0f);
    emitter.rate = std::max(emitter.rate, 0.0f);
    emitter.lifetimeJitter = std::clamp(emitter.lifetimeJitter, 0.0f, 1.0f);

    // **No longer refused for being bigger than the pool** (C40). `wantedCapacity()` reports
    // what the live emitters need and the engine grows the pool to it after the step, so an
    // emitter that does not fit yet is one the pool is about to be resized for. The only
    // remaining ceiling is `kMaxCapacity`, which is the sort key's index field and is not a
    // budget anybody stated.
    //
    // Over-subscription against the emitters already running is still deliberately not
    // checked here: the policy for that is `droppedSpawns()`, counted per birth in update()
    // where the free list is actually known.
    // **Unclamped, because `requiredCapacity` clamps.** It returns `min(need, kMaxCapacity)`,
    // so comparing its result against `kMaxCapacity` can never be greater and the test would
    // be dead code that refused nothing.
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
    // A one-shot by construction. A caller that left this at zero asked for an effect and
    // would have got an emitter that never stops, which is the one outcome a
    // fire-and-forget call must not have.
    if (effect.burst == 0) effect.burst = 16;

    // The emitter's local +Y aimed along the normal, so an authored upward spray becomes a
    // spray off whatever surface was hit. Any perpendicular will do for the other two
    // axes -- the emitter's cone is symmetric about +Y, so their roll is not observable.
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
    // Stops spawning now. Particles already in flight are left to expire: killing them
    // would be a visible pop, and the pool reclaims their slots when they die anyway.
    emitterList[id.index].rate = 0.0f;
    emitterList[id.index].accumulator = 0.0f;
}

void ParticleSystem::update(float dt) {
    // `zone` rather than the `s` every other scope in the tree uses: the spawn loop below
    // already has a `GpuSpawn s`, and shadowing it here is a warning this build treats as
    // an error.
    auto zone = core::Profiler::scope("ParticleSystem::update");
    lastStep = dt;
    spawnList.clear();
    if (poolCapacity == 0) {
        alive = 0;
        return;
    }

    now += dt;

    // One scan, and it produces both numbers. Walking the whole pool rather than an
    // expiry queue is deliberate: it is a linear pass over four bytes a slot, it
    // produces the free list in ascending order for free, and that order is what makes
    // the whole system reproducible.
    freeSlots.clear();
    alive = 0;
    for (uint32_t i = 0; i < poolCapacity; ++i) {
        if (deathTime[i] > now) {
            ++alive;
        } else {
            freeSlots.push_back(i);
        }
    }

    // A one-shot whose last particle has died releases its own slot (C3). Done before
    // this frame's births so a slot freed here is available to a create() next frame,
    // and done here rather than in destroy() because "the last particle died" is a fact
    // only this function knows.
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
            // Emitted once, in full, on the first update after it was created. A burst
            // spread over frames would be a rate, and the caller would have asked for one.
            if (slots[e].burstDone) continue;
            slots[e].burstDone = true;
            births = em.burst;
            // The longest any of these can live, which is when the slot may be released.
            slots[e].expiresAt = now + em.lifetime * (1.0f + em.lifetimeJitter);
        } else {
            em.accumulator += em.rate * dt;
            if (em.accumulator < 1.0f) continue;
            births = static_cast<uint32_t>(em.accumulator);
            em.accumulator -= static_cast<float>(births);
        }

        for (uint32_t k = 0; k < births; ++k) {
            if (nextFree >= freeSlots.size()) {
                // Stated rather than silent, and counted rather than logged from here:
                // an emitter over budget would otherwise print sixty lines a second.
                dropped += births - k;
                break;
            }

            const uint32_t slot = freeSlots[nextFree++];
            const uint32_t seed = em.emitted++;

            // Symmetric about the authored lifetime, so jitter spreads the deaths
            // without shifting the mean -- which matters, because the mean is what the
            // pool was sized from.
            const float r = particleRandom(seed, kRandomLifetime) * 2.0f - 1.0f;
            const float lifetime = std::max(em.lifetime * (1.0f + em.lifetimeJitter * r), 1.0f / 1024.0f);

            // Births are spread across the step rather than stacked on its first
            // instant. Without this a 600/s emitter lays down ten particles at exactly
            // one point every frame, and a jet reads as a string of beads.
            const float age = dt * (static_cast<float>(births - 1 - k) / static_cast<float>(births));

            // The birth time, not the age: it is what the particle carries, and it is
            // what makes the GPU's death test the same arithmetic as this one.
            const float birth = now - age;
            deathTime[slot] = birth + lifetime;
            ++alive;

            GpuSpawn s;
            s.meta = glm::uvec4(slot, e, seed, 0u);
            s.params = glm::vec4(lifetime, birth, 0.0f, 0.0f);
            spawnList.push_back(s);
        }
    }
}

void ParticleSystem::writeGpuEmitters(GpuEmitter* out) const {
    for (size_t i = 0; i < emitterList.size(); ++i) {
        const ParticleEmitter& e = emitterList[i];
        GpuEmitter& g = out[i];
        g.transform = e.transform;
        g.velocity = glm::vec4(e.velocity, e.speedJitter);
        g.boxExtent = glm::vec4(e.boxExtent, e.coneAngle);
        g.gravity = glm::vec4(e.gravity, e.drag);
        g.colorStart = e.colorStart;
        g.colorEnd = e.colorEnd;
        g.params = glm::vec4(e.sizeStart, e.sizeEnd, e.restitution, e.emissiveIntensity);
        g.sprite = glm::vec4(e.spin, e.erosion, e.flipbookLoops, 0.0f);

        uint32_t bits = 0;
        if (e.emissive) bits |= kEmitterEmissive;
        if (e.collides) bits |= kEmitterCollides;
        // Clamped to at least one cell each way. A grid of zero would make the frame
        // count zero, and the shader divides by it.
        const uint32_t cols = std::max(e.flipbookCols, 1u);
        const uint32_t rows = std::max(e.flipbookRows, 1u);
        g.flags = glm::uvec4(e.texture, bits, cols | (rows << 16), 0u);
    }
}

bool parseSceneEmitters(const rapidjson::Value& nodesArray, std::vector<ParticleEmitter>& out) {
    // The document is parsed once, by the caller, and handed to all three readers
    // (C14). It used to be parsed here, and in the other two, and that was about
    // three quarters of a large scene's load -- see core/Json.h.
    const Value* nodes = &nodesArray;

    for (rapidjson::SizeType n = 0; n < nodes->Size(); ++n) {
        const Value* extras = core::json::member((*nodes)[n], "extras");
        if (extras == nullptr) continue;
        const Value* def = core::json::member(*extras, "substrate_emitter");
        if (def == nullptr || !def->IsObject()) continue;

        ParticleEmitter e;
        e.node = n;
        core::json::readString(*def, "name", e.name);
        if (e.name.empty()) core::json::readString((*nodes)[n], "name", e.name);
        // The line this copy was missing (C14). A collider and an audio source on an
        // unnamed node both report "node 7"; an emitter reported an empty string, because
        // three hand-maintained copies of an eighteen-line prologue is exactly what drift
        // looks like. Found by the audit that wrote the row, not by anything failing.
        if (e.name.empty()) e.name = "node " + std::to_string(n);

        core::json::readFloat(*def, "rate", e.rate);
        core::json::readFloat(*def, "lifetime", e.lifetime);
        readFloat(*def, "lifetimeJitter", e.lifetimeJitter);

        core::json::readVec<3>(*def, "velocity", &e.velocity.x);
        readFloat(*def, "speedJitter", e.speedJitter);
        core::json::readAngleDegrees(*def, "coneAngle", e.coneAngle);
        core::json::readVec<3>(*def, "boxExtent", &e.boxExtent.x);

        readVec<3>(*def, "gravity", &e.gravity.x);
        readFloat(*def, "drag", e.drag);

        readVec<4>(*def, "colorStart", &e.colorStart.x);
        readVec<4>(*def, "colorEnd", &e.colorEnd.x);
        readFloat(*def, "sizeStart", e.sizeStart);
        readFloat(*def, "sizeEnd", e.sizeEnd);

        core::json::readUint(*def, "texture", e.texture);
        core::json::readUint(*def, "flipbookCols", e.flipbookCols);
        core::json::readUint(*def, "flipbookRows", e.flipbookRows);
        readFloat(*def, "flipbookLoops", e.flipbookLoops);
        readFloat(*def, "spin", e.spin);
        readFloat(*def, "erosion", e.erosion);
        core::json::readBool(*def, "emissive", e.emissive);
        readFloat(*def, "emissiveIntensity", e.emissiveIntensity);
        core::json::readBool(*def, "collides", e.collides);
        readFloat(*def, "restitution", e.restitution);

        out.push_back(std::move(e));
    }
    return true;
}

} // namespace scene
