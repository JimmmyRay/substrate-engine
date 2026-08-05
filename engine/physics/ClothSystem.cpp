#include "physics/ClothSystem.h"

#include "core/Logger.h"
#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace physics {

bool ClothSystem::add(PhysicsWorld& world, uint32_t instance, uint32_t primitive, const scene::ClothDesc& desc) {
    const scene::ClothTopology topo = scene::weldCloth(desc);
    if (topo.empty()) {
        core::Logger::warn(core::LogCategory::Scene,
                           "cloth: primitive %u welded to %zu particles and %zu faces; not simulated", primitive,
                           topo.positions.size(), topo.faces.size() / 3);
        return false;
    }

    // Refused rather than simulated: a cloth pinned nowhere falls out of the world on frame
    // one and reads as an engine bug.
    const bool anyPinned = std::any_of(topo.invMasses.begin(), topo.invMasses.end(), [](float m) { return m == 0.0f; });
    if (!anyPinned) {
        core::Logger::warn(core::LogCategory::Scene,
                           "cloth: primitive %u has no vertex at %s >= 0.999, so nothing holds it up; not simulated. "
                           "Run scripts/check_pins.py over the source",
                           primitive, std::string(scene::kPinAttribute).c_str());
        return false;
    }

    const uint32_t body = world.createCloth(topo);
    if (body == PhysicsWorld::kNoCloth) return false;

    Cloth c;
    c.instance = instance;
    c.primitive = primitive;
    c.body = body;
    c.remap = topo.remap;
    c.indices.assign(desc.indices.begin(), desc.indices.end());

    // The load transform is baked into the rest pose and the instance transform is identity
    // from here on, so animating a `FABRIC_` mesh's parent moves nothing -- see
    // limitations.md.
    //
    // Normals go through the inverse transpose, which matters for the one frame before the
    // first `update`: a non-uniformly scaled curtain shaded by unadjusted normals is visibly
    // wrong, and a one-frame artefact gets attributed to the solver.
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(desc.transform)));
    c.vertices.resize(desc.vertices.size());
    for (size_t i = 0; i < desc.vertices.size(); ++i) {
        c.vertices[i] = desc.vertices[i];
        c.vertices[i].position = glm::vec3(desc.transform * glm::vec4(desc.vertices[i].position, 1.0f));
        c.vertices[i].normal = normalMatrix * desc.vertices[i].normal;
        c.vertices[i].tangent =
            glm::vec4(glm::mat3(desc.transform) * glm::vec3(desc.vertices[i].tangent), desc.vertices[i].tangent.w);
    }
    // Captured before the first recompute: taken after, these would be once-orthogonalised
    // tangents rather than the authored ones, and their value would depend on when.
    c.restTangents.reserve(c.vertices.size());
    for (const scene::Vertex& v : c.vertices) c.restTangents.push_back(v.tangent);
    scene::recomputeClothNormals(c.vertices, c.indices, c.restTangents);

    c.boundsMin = glm::vec3(std::numeric_limits<float>::max());
    c.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const scene::Vertex& v : c.vertices) {
        c.boundsMin = glm::min(c.boundsMin, v.position);
        c.boundsMax = glm::max(c.boundsMax, v.position);
    }

    clothes.push_back(std::move(c));
    return true;
}

void ClothSystem::update(const PhysicsWorld& world) {
    for (Cloth& c : clothes) {
        const uint32_t particles = world.clothParticleCount(c.body);
        if (particles == 0) continue;

        // Grown and reused, never made a local: an allocation per body per step measured
        // 179 ns and 2.4% of the step.
        if (scratch.size() < particles) scratch.resize(particles);
        world.clothPositions(c.body, std::span<glm::vec3>(scratch.data(), particles));

        float maxMoved = 0.0f;
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        for (size_t i = 0; i < c.vertices.size(); ++i) {
            const uint32_t p = c.remap[i];
            if (p >= particles) continue;
            const glm::vec3 next = scratch[p];
            maxMoved = std::max(maxMoved, glm::length(next - c.vertices[i].position));
            c.vertices[i].position = next;
            lo = glm::min(lo, next);
            hi = glm::max(hi, next);
        }

        scene::recomputeClothNormals(c.vertices, c.indices, c.restTangents);
        c.lastMaxDisplacement = maxMoved;
        c.boundsMin = lo;
        c.boundsMax = hi;
    }
}

uint32_t ClothSystem::vertexCount() const {
    uint32_t total = 0;
    for (const Cloth& c : clothes) total += static_cast<uint32_t>(c.vertices.size());
    return total;
}

} // namespace physics
