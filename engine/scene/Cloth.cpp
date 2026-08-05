#include "scene/Cloth.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace scene {
namespace {

/**
 * The weld grid, in metres. A tenth of a millimetre sits in the only window that works:
 * below anything an author places deliberately, above an exporter's float round-trip error.
 *
 * A grid, not a radius. A radius search welds differently depending on which vertex was seen
 * first, and an order-dependent simulation mesh is an order-dependent solve. The price is the
 * usual grid artefact -- two points a nanometre apart across a cell boundary stay separate.
 */
constexpr float kWeldGrid = 1.0e-4f;

/// The quantised cell of a world-space position. `std::llround`, not a cast: a cast truncates
/// toward zero and puts -0.5 and +0.5 two cells apart rather than one.
struct Cell {
    int64_t x = 0, y = 0, z = 0;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct CellHash {
    size_t operator()(const Cell& c) const {
        // FNV-1a over the three. A sum or an xor lets two coordinates cancel.
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

    // World space before the weld: the grid is world-space, so welding in object space would
    // give a scaled mesh a differently sized cell.
    std::unordered_map<Cell, uint32_t, CellHash> seen;
    seen.reserve(desc.vertices.size() * 2);

    for (size_t i = 0; i < desc.vertices.size(); ++i) {
        const glm::vec3 world = glm::vec3(desc.transform * glm::vec4(desc.vertices[i].position, 1.0f));
        const float invMass = i < desc.masses.size() ? desc.masses[i].invMass : 1.0f;

        // Particles are numbered by first sight, so the simulation mesh is a function of the
        // file and not of the hash table -- do not renumber by iterating `seen`, whose order
        // varies between runs.
        const auto [it, inserted] = seen.emplace(cellOf(world), static_cast<uint32_t>(topo.positions.size()));
        if (inserted) {
            topo.positions.push_back(world);
            topo.invMasses.push_back(invMass);
        } else {
            // The minimum, so a seam with a pinned side stays pinned; averaging invents a
            // weight the author did not write.
            topo.invMasses[it->second] = std::min(topo.invMasses[it->second], invMass);
        }
        topo.remap[i] = it->second;
    }

    topo.faces.reserve(desc.indices.size());
    for (size_t i = 0; i + 2 < desc.indices.size(); i += 3) {
        const uint32_t a = topo.remap[desc.indices[i]];
        const uint32_t b = topo.remap[desc.indices[i + 1]];
        const uint32_t c = topo.remap[desc.indices[i + 2]];
        // A weld turns a source sliver into a degenerate, and
        // `SoftBodySharedSettings::AddFace` asserts on one rather than ignoring it -- letting
        // it through takes a debug build down.
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

    // The cross product is left unnormalised on purpose: its length is twice the triangle
    // area, so the area weighting is free and normalising per face would remove it.
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
        // A vertex no non-degenerate face touched falls back to up, not zero:
        // `normalize(vec3(0))` is a NaN and the G-buffer shades whatever it is handed.
        v.normal = len > 1e-12f ? v.normal / len : glm::vec3(0.0f, 1.0f, 0.0f);

        // Gram-Schmidt from the *rest* tangent, handedness in w untouched. Starting from
        // `v.tangent` makes this a fold over its own output, and the result a function of how
        // many times it has been called.
        glm::vec3 t = glm::vec3(rest);
        t -= v.normal * glm::dot(v.normal, t);
        const float tlen = glm::length(t);
        if (tlen > 1e-6f) {
            v.tangent = glm::vec4(t / tlen, rest.w);
        } else {
            // The tangent went parallel to the normal, which a fold can do. Crossing against
            // the axis the normal is least aligned to is what keeps the result non-degenerate;
            // a fixed axis produces a zero vector at some orientation.
            const glm::vec3 axis =
                std::abs(v.normal.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            v.tangent = glm::vec4(glm::normalize(glm::cross(axis, v.normal)), rest.w);
        }
    }
}

} // namespace scene
