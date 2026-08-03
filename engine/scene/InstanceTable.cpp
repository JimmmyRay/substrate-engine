#include "scene/InstanceTable.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <cassert>

namespace scene {

void InstanceTable::reserve(uint32_t n) {
    shading.reserve(n);
    bounds.reserve(n);
    prevTransforms.reserve(n);
    ranges.reserve(n);
    localMin.reserve(n);
    localMax.reserve(n);
    generations.reserve(n);
}

void InstanceTable::refreshBounds(uint32_t slot, const glm::mat4& m) {
    const glm::vec3 lo = localMin[slot];
    const glm::vec3 hi = localMax[slot];

    glm::vec3 wmin(1e30f);
    glm::vec3 wmax(-1e30f);
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 local((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z);
        const glm::vec3 world = glm::vec3(m * glm::vec4(local, 1.0f));
        wmin = glm::min(wmin, world);
        wmax = glm::max(wmax, world);
    }

    // Radius of the sphere around the *box*, not around the geometry: a tighter
    // sphere would need the vertices, and this one is only ever used to reject.
    const float radius = glm::length((wmax - wmin) * 0.5f);
    bounds[slot].worldMin = glm::vec4(wmin, radius);
    bounds[slot].worldMax = glm::vec4(wmax, 0.0f);
}

InstanceId InstanceTable::create(const InstanceDesc& desc) {
    uint32_t slot;
    if (!freeSlots.empty()) {
        slot = freeSlots.back();
        freeSlots.pop_back();
    } else {
        slot = static_cast<uint32_t>(shading.size());
        shading.emplace_back();
        bounds.emplace_back();
        prevTransforms.emplace_back(1.0f);
        ranges.emplace_back();
        localMin.emplace_back(0.0f);
        localMax.emplace_back(0.0f);
        // One, not zero. `Handle::valid()` reserves generation zero for "never issued",
        // which is what makes a zeroed handle invalid rather than a live reference to
        // slot 0 -- so the first handle a slot ever produces has to be past it.
        generations.push_back(1);
    }

    localMin[slot] = desc.localMin;
    localMax[slot] = desc.localMax;
    ranges[slot] = {desc.firstIndex,  desc.indexCount,  desc.baseVertex,    desc.vertexCount,
                    desc.skinOffset, desc.morphOffset, desc.morphTargets, desc.morphWeightOffset};

    uint32_t f = kInstanceLive;
    if (desc.blended) f |= kInstanceBlended;
    if (desc.masked) f |= kInstanceMasked;
    if (desc.dynamic) f |= kInstanceDynamic;
    // A deformed instance is dynamic by definition: its vertices are rebuilt every
    // frame, so the tiers 3.4 and 3.9 select on that flag apply to it whether or not
    // its node transform ever changes.
    if (desc.skin != 0xFFFFFFFFu) f |= kInstanceSkinned | kInstanceDynamic;
    if (desc.morphTargets > 0) f |= kInstanceMorphed | kInstanceDynamic;
    // Same reasoning again: a cloth's vertices are rewritten every frame, so it is dynamic
    // whether or not anything ever moves its node -- and nothing can, because its transform
    // is identity by construction.
    if (desc.cloth) f |= kInstanceCloth | kInstanceDynamic;

    GpuInstance& g = shading[slot];
    g.meta = glm::uvec4(desc.primitive, desc.material, f, desc.character);
    g.model = desc.transform;

    const glm::mat3 n = glm::inverseTranspose(glm::mat3(desc.transform));
    g.normal0 = glm::vec4(n[0], 0.0f);
    g.normal1 = glm::vec4(n[1], 0.0f);
    g.normal2 = glm::vec4(n[2], 0.0f);

    refreshBounds(slot, desc.transform);

    // A new instance has no history. Seeding it with its own transform is what makes
    // "it did not move" the default: a velocity computed from these two is exactly
    // zero, so the object does not smear on the frame it appears.
    prevTransforms[slot] = desc.transform;

    ++live;
    if (desc.blended) ++blended;
    if ((f & kInstanceDynamic) != 0u && !desc.blended) ++dynamic;
    ++rev;

    return InstanceId{slot, generations[slot]};
}

void InstanceTable::destroy(InstanceId id) {
    if (!valid(id)) return;

    const uint32_t dead = shading[id.index].meta.z;
    if ((dead & kInstanceBlended) != 0u) --blended;
    if ((dead & kInstanceDynamic) != 0u && (dead & kInstanceBlended) == 0u) --dynamic;
    --live;

    shading[id.index] = GpuInstance{};

    // An empty box rather than a zero one. A zero box is a point at the origin, which
    // a frustum test would happily accept; inverted min/max fails every overlap test
    // there is, so a dead slot that reaches the cull dispatch is rejected rather than
    // drawn as whatever geometry the slot last held.
    bounds[id.index].worldMin = glm::vec4(1e30f, 1e30f, 1e30f, 0.0f);
    bounds[id.index].worldMax = glm::vec4(-1e30f, -1e30f, -1e30f, 0.0f);
    ranges[id.index] = {};

    // Wraps at 2^32 creations in one slot, which is a handle collision after roughly
    // four billion create/destroy cycles on the same slot. Recorded rather than
    // guarded: the guard costs a branch on every validity test to defend against a
    // number no frame loop reaches.
    ++generations[id.index];
    freeSlots.push_back(id.index);
    ++rev;
}

void InstanceTable::setTransform(InstanceId id, const glm::mat4& m) {
    if (!valid(id)) return;

    GpuInstance& g = shading[id.index];
    g.model = m;

    const glm::mat3 n = glm::inverseTranspose(glm::mat3(m));
    g.normal0 = glm::vec4(n[0], 0.0f);
    g.normal1 = glm::vec4(n[1], 0.0f);
    g.normal2 = glm::vec4(n[2], 0.0f);

    refreshBounds(id.index, m);
    ++rev;
}

void InstanceTable::setCharacter(InstanceId id, uint32_t character) {
    if (!valid(id)) return;
    if (shading[id.index].meta.w == character) return;
    shading[id.index].meta.w = character;
    ++rev;
}

void InstanceTable::setFlags(InstanceId id, uint32_t set, uint32_t clear) {
    if (!valid(id)) return;

    // kInstanceLive is not a caller's to set: clearing it would leave a slot that
    // every consumer skips but the free list has never heard of, which is a leak that
    // looks like a deletion.
    set &= ~kInstanceLive;
    clear &= ~kInstanceLive;

    const uint32_t before = shading[id.index].meta.z;
    const uint32_t after = (before & ~clear) | set;
    if (after == before) return;

    if ((before & kInstanceBlended) != (after & kInstanceBlended)) {
        blended += (after & kInstanceBlended) ? 1u : ~0u;
    }
    // `dynamic` is a conjunction of two bits, so it cannot be maintained by watching
    // either one alone -- clearing BLENDED on a dynamic instance adds it to the count
    // without DYNAMIC having changed at all.
    const auto counts = [](uint32_t f) {
        return (f & kInstanceDynamic) != 0u && (f & kInstanceBlended) == 0u;
    };
    if (counts(before) != counts(after)) dynamic += counts(after) ? 1u : ~0u;
    shading[id.index].meta.z = after;
    ++rev;
}

void InstanceTable::endFrame() {
    const size_t n = shading.size();
    for (size_t i = 0; i < n; ++i) prevTransforms[i] = shading[i].model;
}

void InstanceTable::clear() {
    shading.clear();
    bounds.clear();
    prevTransforms.clear();
    ranges.clear();
    localMin.clear();
    localMax.clear();
    generations.clear();
    freeSlots.clear();
    live = 0;
    blended = 0;
    dynamic = 0;
    ++rev;
}

} // namespace scene
