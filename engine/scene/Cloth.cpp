#include "scene/Cloth.h"

#include "core/Logger.h"
#include "scene/Physics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace scene {
namespace {

/**
 * The weld grid, in metres. A tenth of a millimetre: far below anything an author places
 * deliberately and far above the error an exporter's float round trip introduces, which is
 * the window a position weld has to sit in.
 *
 * A *grid* rather than a radius, so the answer does not depend on which vertex was seen
 * first -- two points either land in the same cell or they do not. A radius search would
 * make the weld order-dependent, and an order-dependent simulation mesh is an
 * order-dependent solve, which is the determinism `Physics.h` spends three paragraphs
 * defending. The cost is the ordinary grid artefact: two points a nanometre apart across a
 * cell boundary stay separate. For fabric that is invisible, and the alternative is not.
 */
constexpr float kWeldGrid = 1.0e-4f;

/// The quantised cell of a world-space position, as three integers. `std::llround` rather
/// than a cast, because a cast truncates toward zero and would put -0.5 and +0.5 in cells
/// that are two apart rather than one.
struct Cell {
    int64_t x = 0, y = 0, z = 0;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct CellHash {
    size_t operator()(const Cell& c) const {
        // FNV-1a over the three, the same fold `layoutDigest` uses and for the same
        // reason: a sum or an xor lets two coordinates cancel.
        uint64_t h = 1469598103934665603ull;
        for (int64_t v : {c.x, c.y, c.z}) {
            const auto u = static_cast<uint64_t>(v);
            for (unsigned b = 0; b < 8; ++b) {
                h ^= (u >> (b * 8)) & 0xFFull;
                h *= 1099511628211ull;
            }
        }
        return static_cast<size_t>(h);
    }
};

Cell cellOf(const glm::vec3& p) {
    return {static_cast<int64_t>(std::llround(static_cast<double>(p.x) / kWeldGrid)),
            static_cast<int64_t>(std::llround(static_cast<double>(p.y) / kWeldGrid)),
            static_cast<int64_t>(std::llround(static_cast<double>(p.z) / kWeldGrid))};
}

} // namespace

ClothTopology weldCloth(const ClothDesc& desc) {
    ClothTopology topo;
    if (desc.vertices.empty()) return topo;

    topo.remap.resize(desc.vertices.size());
    topo.positions.reserve(desc.vertices.size());
    topo.invMasses.reserve(desc.vertices.size());

    // World space before the weld, not after -- the grid is a world-space grid, so a
    // scaled mesh welded in object space would weld against a differently sized cell.
    std::unordered_map<Cell, uint32_t, CellHash> seen;
    seen.reserve(desc.vertices.size() * 2);

    for (size_t i = 0; i < desc.vertices.size(); ++i) {
        const glm::vec3 world = glm::vec3(desc.transform * glm::vec4(desc.vertices[i].position, 1.0f));
        // A span shorter than the vertices leaves the remainder free. The loader sizes
        // them together and `check_pins.py` refuses a file where they disagree, so this is
        // a defence rather than a case.
        const float invMass = i < desc.masses.size() ? desc.masses[i].invMass : 1.0f;

        // Insert-or-find in one lookup. The welded index is `positions.size()` at the
        // moment of first sight, so the numbering is the order the render vertices are in
        // -- a function of the file and not of the hash table, which is what keeps the
        // simulation mesh identical between runs. Nothing here ever iterates `seen`.
        const auto [it, inserted] = seen.emplace(cellOf(world), static_cast<uint32_t>(topo.positions.size()));
        if (inserted) {
            topo.positions.push_back(world);
            topo.invMasses.push_back(invMass);
        } else {
            // The minimum, so a seam with a pinned side stays pinned. See the field
            // comment: averaging would invent a weight the author did not write.
            topo.invMasses[it->second] = std::min(topo.invMasses[it->second], invMass);
        }
        topo.remap[i] = it->second;
    }

    topo.faces.reserve(desc.indices.size());
    for (size_t i = 0; i + 2 < desc.indices.size(); i += 3) {
        const uint32_t a = topo.remap[desc.indices[i]];
        const uint32_t b = topo.remap[desc.indices[i + 1]];
        const uint32_t c = topo.remap[desc.indices[i + 2]];
        // A weld turns a sliver into a degenerate, and Jolt asserts on one rather than
        // ignoring it -- `SoftBodySharedSettings::AddFace` has the assert in so many
        // words. Dropping it here is the only place that check can live, because by the
        // time the face reaches Jolt the process is already going down in a debug build.
        if (a == b || b == c || a == c) continue;
        topo.faces.push_back(a);
        topo.faces.push_back(b);
        topo.faces.push_back(c);
    }

    return topo;
}

void recomputeClothNormals(std::span<Vertex> vertices, std::span<const uint32_t> indices,
                           std::span<const glm::vec4> restTangents) {
    if (vertices.empty()) return;

    for (Vertex& v : vertices) v.normal = glm::vec3(0.0f);

    // Area-weighted, which is what an unnormalised cross product already is: its length is
    // twice the triangle's area. So the weighting is free and *not* doing it would cost an
    // extra normalize per face.
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) continue;
        const glm::vec3 face = glm::cross(vertices[b].position - vertices[a].position,
                                          vertices[c].position - vertices[a].position);
        vertices[a].normal += face;
        vertices[b].normal += face;
        vertices[c].normal += face;
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
        Vertex& v = vertices[i];
        const glm::vec4 rest = i < restTangents.size() ? restTangents[i] : v.tangent;
        const float len = glm::length(v.normal);
        // A vertex no non-degenerate face touched. Up rather than zero, because
        // `normalize(vec3(0))` is a NaN and the G-buffer shades from whatever it is
        // handed -- the same trap `SceneParse.cpp` names for a missing TANGENT.
        v.normal = len > 1e-12f ? v.normal / len : glm::vec3(0.0f, 1.0f, 0.0f);

        // Gram-Schmidt from the *rest* tangent, keeping the handedness in w. The UVs did
        // not move, so the direction the file authored is still the right one; all that
        // changed is the plane it has to lie in. Starting from `v.tangent` instead would
        // make this a fold over its own output and the answer a function of how many times
        // it had been called -- see the header.
        glm::vec3 t = glm::vec3(rest);
        t -= v.normal * glm::dot(v.normal, t);
        const float tlen = glm::length(t);
        if (tlen > 1e-6f) {
            v.tangent = glm::vec4(t / tlen, rest.w);
        } else {
            // The tangent went parallel to the normal, which a fold can do. Any
            // perpendicular will do for a surface whose tangent frame has collapsed;
            // picking the axis the normal is least aligned to keeps it non-degenerate.
            const glm::vec3 axis =
                std::abs(v.normal.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            v.tangent = glm::vec4(glm::normalize(glm::cross(axis, v.normal)), rest.w);
        }
    }
}

bool ClothSystem::add(PhysicsWorld& world, uint32_t instance, uint32_t primitive, const ClothDesc& desc) {
    const ClothTopology topo = weldCloth(desc);
    if (topo.empty()) {
        core::Logger::warn(core::LogCategory::Scene,
                           "cloth: primitive %u welded to %zu particles and %zu faces; not simulated", primitive,
                           topo.positions.size(), topo.faces.size() / 3);
        return false;
    }

    // A cloth pinned nowhere is refused rather than simulated. It falls out of the world
    // on frame one and reads as an engine bug; `check_pins.py` refuses the same case
    // before the export leaves Blender, and this is that refusal for a file that never
    // went through it.
    const bool anyPinned = std::any_of(topo.invMasses.begin(), topo.invMasses.end(), [](float m) { return m == 0.0f; });
    if (!anyPinned) {
        core::Logger::warn(core::LogCategory::Scene,
                           "cloth: primitive %u has no vertex at %s >= 0.999, so nothing holds it up; not simulated. "
                           "Run scripts/check_pins.py over the source",
                           primitive, std::string(kPinAttribute).c_str());
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

    // The rest pose, in world space, because the instance transform is identity from here
    // on: a soft body has no rigid transform to push down a node hierarchy, so the load
    // transform is baked in and the node's is ignored thereafter. That is a real
    // restriction -- a `FABRIC_` mesh cannot be moved by animating its parent -- and it is
    // recorded in limitations.md rather than left to be discovered.
    //
    // Normals go through the inverse transpose rather than the transform, which matters
    // for the one frame before the first `update` replaces them: a non-uniformly scaled
    // curtain shaded by its unadjusted normals is visibly wrong, and "visible for one
    // frame" is exactly the kind of thing that gets attributed to the solver.
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(desc.transform)));
    c.vertices.resize(desc.vertices.size());
    for (size_t i = 0; i < desc.vertices.size(); ++i) {
        c.vertices[i] = desc.vertices[i];
        c.vertices[i].position = glm::vec3(desc.transform * glm::vec4(desc.vertices[i].position, 1.0f));
        c.vertices[i].normal = normalMatrix * desc.vertices[i].normal;
        c.vertices[i].tangent =
            glm::vec4(glm::mat3(desc.transform) * glm::vec3(desc.vertices[i].tangent), desc.vertices[i].tangent.w);
    }
    // Captured *before* the first recompute, so it is the transformed authoring tangent
    // rather than a once-orthogonalised version of it -- one fewer thing whose value
    // depends on when it was taken.
    c.restTangents.reserve(c.vertices.size());
    for (const Vertex& v : c.vertices) c.restTangents.push_back(v.tangent);
    recomputeClothNormals(c.vertices, c.indices, c.restTangents);

    c.boundsMin = glm::vec3(std::numeric_limits<float>::max());
    c.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const Vertex& v : c.vertices) {
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

        // Grown once and reused, never allocated per cloth per frame. The pose-resolve row
        // measured an allocation per body per step at 179 ns and 2.4% of the step, which
        // is the whole argument for this being a member and not a local.
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

        recomputeClothNormals(c.vertices, c.indices, c.restTangents);
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

} // namespace scene
