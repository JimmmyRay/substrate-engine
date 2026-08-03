#include "scene/NavMesh.h"

#include "core/Profiler.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>

namespace scene {

namespace {

/// Twice the signed area of a triangle projected onto XZ. Positive when `c` is left of the
/// line `a`->`b`, which is the whole of the funnel's geometry: every decision it makes is a
/// question about which side of the current sight line a portal endpoint fell on.
float triArea2(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    return (c.x - a.x) * (b.z - a.z) - (b.x - a.x) * (c.z - a.z);
}

/// The part of `v` perpendicular to `up`, which is what "horizontal" means once a follower
/// is walking a plane that is not XZ (D18). `up` must be unit length. For +Y this is
/// exactly `(v.x, 0, v.z)` -- the subtraction cancels rather than approximately cancels --
/// so the 3D numbers are unchanged.
glm::vec3 flatten(const glm::vec3& v, const glm::vec3& up) { return v - up * glm::dot(v, up); }

float distanceXZ(const glm::vec3& a, const glm::vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

/// Squared distance from a point to an axis-aligned box, zero inside it. The BVH's only
/// pruning test.
float distance2ToBox(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi) {
    const glm::vec3 d = glm::max(glm::max(lo - p, p - hi), glm::vec3(0.0f));
    return glm::dot(d, d);
}

/// Cell key for the weld grid. Three 21-bit fields would overflow on a world measured in
/// hundreds of kilometres at a centimetre epsilon, so this hashes rather than packs and the
/// caller compares real distances afterwards -- a collision costs a distance test, not a
/// wrong answer.
size_t cellHash(int64_t x, int64_t y, int64_t z) {
    size_t h = static_cast<size_t>(x) * 73856093u;
    h ^= static_cast<size_t>(y) * 19349663u;
    h ^= static_cast<size_t>(z) * 83492791u;
    return h;
}

} // namespace

// ==================================================================== bake

void NavMesh::bake(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices,
                   const NavBuildParams& params) {
    // Voxelize, region-label, BVH.
    auto zone = core::Profiler::scope("NavMesh::bake");
    verts.clear();
    tris.clear();
    nodes.clear();
    bvhOrder.clear();
    regions = 0;
    build = params;

    // **The solver's frame, decided once here** (D18). Everything below this line -- the
    // slope filter, the barycentric tests, the funnel's left and right -- is written for
    // Y up, and none of it changes: what changes is the frame the vertices arrive in. A
    // rotation rather than an axis swap, because a permutation flips handedness and the
    // funnel reads a portal's sides off a winding that only holds in a right-handed basis.
    //
    // The +Y case takes no arithmetic at all rather than multiplying by an identity
    // quaternion, which is what makes a 3D scene bit-for-bit the scene it was.
    const float reach = glm::length(build.up);
    const glm::vec3 axis = reach > 1e-6f ? build.up / reach : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 y(0.0f, 1.0f, 0.0f);
    const float along = glm::dot(axis, y);
    rotated = along < 1.0f - 1e-6f;
    if (!rotated) {
        navRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    } else if (along <= -1.0f + 1e-6f) {
        // Antiparallel: the shortest arc is undefined, so pick one. Any half turn about an
        // axis in the XZ plane takes -Y onto +Y, and X is as good as any.
        navRotation = glm::angleAxis(glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
    } else {
        navRotation = glm::rotation(axis, y);
    }

    if (indices.size() < 3) return;

    // ---------------------------------------------------------------- weld
    //
    // A triangle soup has no adjacency: two floor tiles that visibly share an edge have
    // four distinct corners at two positions, and every one of them came from a different
    // vertex in the source buffer because they carried different normals or UVs. Welding
    // on position alone is what turns that back into a surface.
    const float weld = std::max(params.weldEpsilon, 1e-6f);
    const float weld2 = weld * weld;
    std::unordered_map<size_t, std::vector<uint32_t>> grid;
    std::vector<uint32_t> remap(positions.size(), 0);

    for (uint32_t i = 0; i < positions.size(); ++i) {
        // The one place the world enters. Everything welded, filtered, indexed and stored
        // below is in the solver's frame from here on.
        const glm::vec3 p = toNav(positions[i]);
        const auto cx = static_cast<int64_t>(std::floor(p.x / weld));
        const auto cy = static_cast<int64_t>(std::floor(p.y / weld));
        const auto cz = static_cast<int64_t>(std::floor(p.z / weld));

        uint32_t found = 0xFFFFFFFFu;
        // The 27 neighbouring cells, not just this one: two points either side of a cell
        // boundary are a nanometre apart and would otherwise never be compared.
        for (int64_t dz = -1; dz <= 1 && found == 0xFFFFFFFFu; ++dz) {
            for (int64_t dy = -1; dy <= 1 && found == 0xFFFFFFFFu; ++dy) {
                for (int64_t dx = -1; dx <= 1 && found == 0xFFFFFFFFu; ++dx) {
                    const auto it = grid.find(cellHash(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (const uint32_t candidate : it->second) {
                        const glm::vec3 d = verts[candidate] - p;
                        if (glm::dot(d, d) <= weld2) {
                            found = candidate;
                            break;
                        }
                    }
                }
            }
        }

        if (found == 0xFFFFFFFFu) {
            found = static_cast<uint32_t>(verts.size());
            verts.push_back(p);
            grid[cellHash(cx, cy, cz)].push_back(found);
        }
        remap[i] = found;
    }

    // ---------------------------------------------------------------- slope filter
    const float cosLimit = std::cos(glm::radians(std::clamp(params.walkableSlopeDegrees, 0.0f, 89.0f)));
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        uint32_t a = remap[indices[i]];
        uint32_t b = remap[indices[i + 1]];
        uint32_t c = remap[indices[i + 2]];
        // Welding collapses degenerate slivers onto themselves, which is a feature: they
        // carry no area and would only add adjacency noise.
        if (a == b || b == c || a == c) continue;

        const glm::vec3 n = glm::cross(verts[b] - verts[a], verts[c] - verts[a]);
        const float len = glm::length(n);
        if (len < 1e-12f) continue;
        // Signed, not absolute. A ceiling is a surface whose normal points *down*, and a
        // mesh that accepted it would bake the underside of every floor as walkable and
        // then route agents across it. The cost is that a floor authored with reversed
        // winding is silently not walkable -- which is the same trade Recast makes, and
        // the same one every renderer here already makes by backface-culling it.
        //
        // It also means every surviving triangle already winds with its normal up, so the
        // funnel can read a shared edge's two vertices as left and right without asking
        // which triangle it came from. That property is load-bearing and it is free.
        if (n.y / len < cosLimit) continue;

        NavTriangle t;
        t.v[0] = a;
        t.v[1] = b;
        t.v[2] = c;
        t.neighbour[0] = t.neighbour[1] = t.neighbour[2] = kNoTriangle;
        tris.push_back(t);
    }

    if (tris.empty()) {
        verts.clear();
        return;
    }

    // ---------------------------------------------------------------- adjacency
    //
    // Keyed on the unordered vertex pair, so the two triangles either side of an edge
    // arrive at the same key from opposite windings.
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> edges;
    edges.reserve(tris.size() * 3);
    for (uint32_t t = 0; t < tris.size(); ++t) {
        for (uint32_t e = 0; e < 3; ++e) {
            const uint32_t v0 = tris[t].v[e];
            const uint32_t v1 = tris[t].v[(e + 1) % 3];
            const uint64_t key = (static_cast<uint64_t>(std::min(v0, v1)) << 32) | std::max(v0, v1);
            const auto it = edges.find(key);
            if (it == edges.end()) {
                edges.emplace(key, std::make_pair(t, e));
                continue;
            }
            // A third triangle on one edge is non-manifold geometry -- a wall meeting a
            // floor along a line that also carries a ramp. The first pairing wins and the
            // rest go unlinked, because a corridor through a non-manifold edge has no
            // well-defined left and right and the funnel would produce nonsense.
            if (tris[it->second.first].neighbour[it->second.second] != kNoTriangle) continue;
            tris[t].neighbour[e] = it->second.first;
            tris[it->second.first].neighbour[it->second.second] = t;
        }
    }

    labelRegions(params.minRegionArea);
    if (tris.empty()) {
        verts.clear();
        return;
    }
    buildBvh();
}

// ==================================================================== regions

void NavMesh::labelRegions(float minRegionArea) {
    constexpr uint32_t kUnlabelled = 0xFFFFFFFFu;
    std::vector<uint32_t> label(tris.size(), kUnlabelled);
    std::vector<float> area;
    std::vector<uint32_t> stack;

    uint32_t next = 0;
    for (uint32_t seed = 0; seed < tris.size(); ++seed) {
        if (label[seed] != kUnlabelled) continue;
        float total = 0.0f;
        stack.push_back(seed);
        label[seed] = next;
        while (!stack.empty()) {
            const uint32_t t = stack.back();
            stack.pop_back();
            total += 0.5f * glm::length(glm::cross(verts[tris[t].v[1]] - verts[tris[t].v[0]],
                                                   verts[tris[t].v[2]] - verts[tris[t].v[0]]));
            for (const uint32_t n : tris[t].neighbour) {
                if (n == kNoTriangle || label[n] != kUnlabelled) continue;
                label[n] = next;
                stack.push_back(n);
            }
        }
        area.push_back(total);
        ++next;
    }

    // Compact. A scene bakes dozens of scraps an agent can never reach -- a windowsill,
    // the top of a crate -- and each one is a region `nearest` can snap to and `findPath`
    // can then only fail from.
    std::vector<uint32_t> oldToNew(tris.size(), kNoTriangle);
    std::vector<NavTriangle> kept;
    std::vector<uint32_t> regionRemap(area.size(), kUnlabelled);
    uint32_t liveRegions = 0;
    for (uint32_t t = 0; t < tris.size(); ++t) {
        if (area[label[t]] < minRegionArea) continue;
        if (regionRemap[label[t]] == kUnlabelled) regionRemap[label[t]] = liveRegions++;
        oldToNew[t] = static_cast<uint32_t>(kept.size());
        kept.push_back(tris[t]);
        kept.back().region = regionRemap[label[t]];
    }

    for (NavTriangle& t : kept) {
        for (uint32_t& n : t.neighbour) {
            n = (n == kNoTriangle) ? kNoTriangle : oldToNew[n];
        }
    }
    tris = std::move(kept);
    regions = liveRegions;
}

// ==================================================================== bvh

glm::vec3 NavMesh::centroid(uint32_t tri) const {
    const NavTriangle& t = tris[tri];
    return (verts[t.v[0]] + verts[t.v[1]] + verts[t.v[2]]) / 3.0f;
}

void NavMesh::buildBvh() {
    bvhOrder.resize(tris.size());
    for (uint32_t i = 0; i < tris.size(); ++i) bvhOrder[i] = i;
    nodes.clear();
    nodes.reserve(tris.size() * 2);
    (void)buildBvhRange(0, static_cast<uint32_t>(bvhOrder.size()), 0);
}

uint32_t NavMesh::buildBvhRange(uint32_t first, uint32_t count, uint32_t depth) {
    const uint32_t self = static_cast<uint32_t>(nodes.size());
    nodes.emplace_back();

    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (uint32_t i = first; i < first + count; ++i) {
        const NavTriangle& t = tris[bvhOrder[i]];
        for (const uint32_t v : t.v) {
            lo = glm::min(lo, verts[v]);
            hi = glm::max(hi, verts[v]);
        }
    }
    nodes[self].boundsMin = lo;
    nodes[self].boundsMax = hi;

    // Leaf. The depth cap is a backstop for geometry that defeats the median split --
    // thousands of coincident triangles, which a merged scene really does contain.
    constexpr uint32_t kLeafSize = 4;
    if (count <= kLeafSize || depth > 48) {
        nodes[self].firstTri = first;
        nodes[self].triCount = count;
        return self;
    }

    const glm::vec3 extent = hi - lo;
    const int axis = (extent.x > extent.y && extent.x > extent.z) ? 0 : (extent.y > extent.z ? 1 : 2);
    const uint32_t mid = count / 2;
    std::nth_element(bvhOrder.begin() + first, bvhOrder.begin() + first + mid, bvhOrder.begin() + first + count,
                     [&](uint32_t a, uint32_t b) { return centroid(a)[axis] < centroid(b)[axis]; });

    nodes[self].triCount = 0;
    (void)buildBvhRange(first, mid, depth + 1);
    // The right child's index is not stored: a node's left child always follows it, and
    // the right one is found by walking past the left subtree. That is one uint32 saved
    // per node and one indirection saved per descent, and it is why `firstTri` is only
    // meaningful on a leaf.
    nodes[self].firstTri = buildBvhRange(first + mid, count - mid, depth + 1);
    return self;
}

// ==================================================================== queries

glm::vec3 NavMesh::closestOnTriangle(uint32_t tri, const glm::vec3& p) const {
    // Ericson's region test, unrolled. The barycentric shortcut would be shorter and is
    // wrong on obtuse triangles, which a merged floor is full of.
    const NavTriangle& t = tris[tri];
    const glm::vec3& a = verts[t.v[0]];
    const glm::vec3& b = verts[t.v[1]];
    const glm::vec3& c = verts[t.v[2]];

    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = p - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));

    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }

    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

NavPoint NavMesh::nearestNav(const glm::vec3& p, float maxDistance) const {
    NavPoint out;
    if (nodes.empty()) return out;

    float best2 = maxDistance * maxDistance;
    // An explicit stack rather than recursion: a 48-deep tree is fine on the stack, but
    // this runs per agent per frame in the worst case and the call overhead is the
    // measurable part.
    uint32_t stack[64];
    uint32_t depth = 0;
    stack[depth++] = 0;

    while (depth > 0) {
        const BvhNode& n = nodes[stack[--depth]];
        if (distance2ToBox(p, n.boundsMin, n.boundsMax) > best2) continue;

        if (n.triCount > 0) {
            for (uint32_t i = n.firstTri; i < n.firstTri + n.triCount; ++i) {
                const uint32_t tri = bvhOrder[i];
                const glm::vec3 q = closestOnTriangle(tri, p);
                const float d2 = glm::dot(q - p, q - p);
                if (d2 < best2) {
                    best2 = d2;
                    out.triangle = tri;
                    out.position = q;
                }
            }
            continue;
        }
        if (depth + 2 <= 64) {
            stack[depth++] = &n - nodes.data() + 1; // left child
            stack[depth++] = n.firstTri;            // right child
        }
    }
    return out;
}

NavPoint NavMesh::dropToFloorNav(const glm::vec3& p, float maxDrop) const {
    NavPoint out;
    if (nodes.empty()) return out;

    // The highest surface at or below `p`, which is not the same as the nearest one: an
    // agent standing on a balcony is nearer to the balcony than to the ground below, and
    // an agent one millimetre above the ground is nearer to the ground than to nothing.
    float bestY = -std::numeric_limits<float>::max();
    const float floor = p.y - maxDrop;

    uint32_t stack[64];
    uint32_t depth = 0;
    stack[depth++] = 0;

    while (depth > 0) {
        const BvhNode& n = nodes[stack[--depth]];
        // A small epsilon on the XZ test, because a query exactly on a shared edge would
        // otherwise miss both triangles.
        constexpr float kEps = 1e-4f;
        if (p.x < n.boundsMin.x - kEps || p.x > n.boundsMax.x + kEps) continue;
        if (p.z < n.boundsMin.z - kEps || p.z > n.boundsMax.z + kEps) continue;
        if (n.boundsMax.y < floor || n.boundsMin.y > p.y + kEps) continue;

        if (n.triCount > 0) {
            for (uint32_t i = n.firstTri; i < n.firstTri + n.triCount; ++i) {
                const uint32_t tri = bvhOrder[i];
                const NavTriangle& t = tris[tri];
                const glm::vec3& a = verts[t.v[0]];
                const glm::vec3& b = verts[t.v[1]];
                const glm::vec3& c = verts[t.v[2]];

                // Barycentric containment in XZ.
                const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
                if (std::abs(d) < 1e-12f) continue;
                const float u = ((b.z - c.z) * (p.x - c.x) + (c.x - b.x) * (p.z - c.z)) / d;
                const float v = ((c.z - a.z) * (p.x - c.x) + (a.x - c.x) * (p.z - c.z)) / d;
                const float w = 1.0f - u - v;
                if (u < -1e-4f || v < -1e-4f || w < -1e-4f) continue;

                const float y = u * a.y + v * b.y + w * c.y;
                if (y > p.y + kEps || y < floor) continue;
                if (y > bestY) {
                    bestY = y;
                    out.triangle = tri;
                    out.position = glm::vec3(p.x, y, p.z);
                }
            }
            continue;
        }
        if (depth + 2 <= 64) {
            stack[depth++] = &n - nodes.data() + 1;
            stack[depth++] = n.firstTri;
        }
    }
    return out;
}

bool NavMesh::raycastNav(const NavPoint& from, const glm::vec3& to) const {
    if (!from || tris.empty()) return false;

    constexpr float kEps = 1e-5f;
    uint32_t t = from.triangle;
    const glm::vec3& p = from.position;

    // Which side of the ray a vertex sitting exactly *on* it counts as. Picking an exit edge
    // needs a strict left and a strict right, and a vertex on the line is neither -- so the
    // walk below is the walk of the ray nudged infinitesimally off its own line. **Decided
    // once and held for every step**: a nudge that changed direction part way is a ray that
    // crosses an edge and then decides it never did.
    //
    // Nudged *into* the starting triangle, which is the whole of the fix. A triangle the ray
    // merely grazes -- one lying wholly to one side of it, which both triangles either side
    // of a tile boundary are, and a grid path runs along one the whole way -- has no pair of
    // vertices the ray separates, so the search below finds no exit edge at all and calls a
    // line lying entirely on the mesh an obstruction. Nudging its way instead puts the ray
    // through its interior, and every step after that is an ordinary crossing.
    bool anyLeft = false;
    bool anyRight = false;
    for (const uint32_t v : tris[t].v) {
        const float area = triArea2(p, to, verts[v]);
        anyLeft = anyLeft || area > 0.0f;
        anyRight = anyRight || area < 0.0f;
    }
    // Only a triangle lying wholly to the left needs the other nudge; one with a vertex on
    // each side is entered either way, and keeps the one the walk has always used.
    const bool tieLeft = anyRight || !anyLeft;

    const auto leftOfRay = [&](const glm::vec3& v) {
        const float area = triArea2(p, to, v);
        return area != 0.0f ? area > 0.0f : tieLeft;
    };

    // Bounded by the triangle count: a walk that has crossed every triangle is a walk that
    // is looping on a degenerate edge, and returning "blocked" is the safe answer.
    for (uint32_t step = 0; step <= tris.size(); ++step) {
        const NavTriangle& tri = tris[t];
        const glm::vec3& v0 = verts[tri.v[0]];
        const glm::vec3& v1 = verts[tri.v[1]];
        const glm::vec3& v2 = verts[tri.v[2]];
        if (triArea2(v0, v1, to) >= -kEps && triArea2(v1, v2, to) >= -kEps && triArea2(v2, v0, to) >= -kEps) {
            return true;
        }

        uint32_t exit = 3;
        for (uint32_t e = 0; e < 3; ++e) {
            const glm::vec3& a = verts[tri.v[e]];
            const glm::vec3& b = verts[tri.v[(e + 1) % 3]];
            // `to` must be beyond this edge, and the ray must separate its endpoints. Both
            // halves are needed: the first alone picks an edge the ray misses, the second
            // alone picks the edge behind the start point.
            //
            // The first stays an exact comparison against zero. A tie there means `to` lies
            // on this edge's line, which is either an edge collinear with the ray -- whose
            // endpoints are then both on it, so the second rejects it anyway -- or `to` on
            // the edge itself, which the containment test above already answered.
            if (triArea2(a, b, to) >= 0.0f) continue;
            if (leftOfRay(a) == leftOfRay(b)) continue;
            exit = e;
            break;
        }
        if (exit == 3) return false;

        const uint32_t next = tri.neighbour[exit];
        if (next == kNoTriangle) return false; // Walked off the edge of the world.
        t = next;
    }
    return false;
}

bool NavMesh::corridorClearNav(const glm::vec3& from, const glm::vec3& to, float radius) const {
    const NavPoint start = nearestNav(from, 1.0f);
    if (!start) return false;
    if (!raycastNav(start, to)) return false;
    if (radius <= 1e-4f) return true;

    const glm::vec3 d = to - from;
    const float len = std::sqrt(d.x * d.x + d.z * d.z);
    if (len < 1e-5f) return true;

    // Both edges of the band an agent of this width sweeps. Testing only the centre line
    // is what lets a smoothing pass shave a corner the funnel had already inset away from,
    // which would undo the one thing `agentRadius` does.
    const glm::vec3 side(-d.z / len * radius, 0.0f, d.x / len * radius);
    for (const float s : {1.0f, -1.0f}) {
        const glm::vec3 offset = from + side * s;
        const NavPoint at = nearestNav(offset, radius * 2.0f);
        if (!at) return false;
        // Snapped back onto the mesh means the offset start was already off it, so the
        // band does not fit here regardless of what the walk would say.
        if (distanceXZ(at.position, offset) > 1e-3f) return false;
        if (!raycastNav(at, to + side * s)) return false;
    }
    return true;
}

// ============================================== the world's frame and the solver's (D18)
//
// Every public query is one of these, and the private `*Nav` half below is what every
// internal caller reaches for. Splitting them is what stops a rotation being applied
// twice: `corridorClear` asks `nearest` and `raycast`, `findPath` asks `findCorridor` and
// `corridorClear`, and a wrapper calling a wrapper would rotate the same point again.
//
// A falsy `NavPoint` carries a zero position, which any rotation leaves zero, so a miss
// needs no special case on the way back out.

NavPoint NavMesh::nearest(const glm::vec3& p, float maxDistance) const {
    NavPoint out = nearestNav(toNav(p), maxDistance);
    out.position = toWorld(out.position);
    return out;
}

NavPoint NavMesh::dropToFloor(const glm::vec3& p, float maxDrop) const {
    NavPoint out = dropToFloorNav(toNav(p), maxDrop);
    out.position = toWorld(out.position);
    return out;
}

bool NavMesh::raycast(const NavPoint& from, const glm::vec3& to) const {
    return raycastNav({from.triangle, toNav(from.position)}, toNav(to));
}

bool NavMesh::corridorClear(const glm::vec3& from, const glm::vec3& to, float radius) const {
    return corridorClearNav(toNav(from), toNav(to), radius);
}

bool NavMesh::findPath(const NavPoint& from, const NavPoint& to, std::vector<glm::vec3>& out) const {
    if (!findPathNav({from.triangle, toNav(from.position)}, {to.triangle, toNav(to.position)}, out)) return false;
    for (glm::vec3& p : out) p = toWorld(p);
    return true;
}

bool NavMesh::findCorridor(const NavPoint& from, const NavPoint& to, std::vector<uint32_t>& out) const {
    return findCorridorNav({from.triangle, toNav(from.position)}, {to.triangle, toNav(to.position)}, out);
}

bool NavMesh::reachable(const NavPoint& from, const NavPoint& to) const {
    if (!from || !to) return false;
    return tris[from.triangle].region == tris[to.triangle].region;
}

// ==================================================================== search

bool NavMesh::findCorridorNav(const NavPoint& from, const NavPoint& to, std::vector<uint32_t>& out) const {
    out.clear();
    if (!reachable(from, to)) return false;

    if (from.triangle == to.triangle) {
        out.push_back(from.triangle);
        return true;
    }

    constexpr float kUnvisited = std::numeric_limits<float>::max();
    std::vector<float> g(tris.size(), kUnvisited);
    std::vector<uint32_t> cameFrom(tris.size(), kNoTriangle);
    std::vector<uint8_t> closed(tris.size(), 0);

    using Entry = std::pair<float, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;

    g[from.triangle] = 0.0f;
    open.emplace(distanceXZ(from.position, to.position), from.triangle);

    while (!open.empty()) {
        const uint32_t current = open.top().second;
        open.pop();
        if (closed[current]) continue;
        closed[current] = 1;

        if (current == to.triangle) {
            for (uint32_t t = current; t != kNoTriangle; t = cameFrom[t]) out.push_back(t);
            std::reverse(out.begin(), out.end());
            return true;
        }

        const glm::vec3 here = (current == from.triangle) ? from.position : centroid(current);
        for (const uint32_t next : tris[current].neighbour) {
            if (next == kNoTriangle || closed[next]) continue;
            const glm::vec3 there = (next == to.triangle) ? to.position : centroid(next);
            const float tentative = g[current] + distanceXZ(here, there);
            if (tentative >= g[next]) continue;
            g[next] = tentative;
            cameFrom[next] = current;
            open.emplace(tentative + distanceXZ(there, to.position), next);
        }
    }
    return false;
}

bool NavMesh::findPathNav(const NavPoint& from, const NavPoint& to, std::vector<glm::vec3>& out) const {
    out.clear();

    std::vector<uint32_t> corridor;
    if (!findCorridorNav(from, to, corridor)) return false;

    if (corridor.size() == 1) {
        out.push_back(from.position);
        out.push_back(to.position);
        return true;
    }

    // ---------------------------------------------------------------- portals
    //
    // One per shared edge, plus a degenerate one at the goal so the funnel has something
    // to close against. Left and right are taken from the *outgoing* triangle's winding,
    // which is well defined because the bake made every normal point up.
    std::vector<glm::vec3> left;
    std::vector<glm::vec3> right;
    left.reserve(corridor.size() + 1);
    right.reserve(corridor.size() + 1);

    for (size_t i = 0; i + 1 < corridor.size(); ++i) {
        const NavTriangle& t = tris[corridor[i]];
        uint32_t edge = 3;
        for (uint32_t e = 0; e < 3; ++e) {
            if (t.neighbour[e] == corridor[i + 1]) {
                edge = e;
                break;
            }
        }
        if (edge == 3) return false; // A* returned a corridor that is not connected.

        // `v[edge]` is the left endpoint and `v[edge+1]` the right one, travelling out of
        // this triangle into the next. The opposite assignment is the intuitive one -- with
        // normals up and a consistent winding, `v[edge+1]` is geometrically on the left --
        // and it is wrong here, because `triArea2`'s positive half-plane is the funnel's
        // *right*. Getting it backwards does not fail loudly: the funnel simply restarts at
        // every portal and returns the corridor's own vertices, which still walks.
        glm::vec3 l = verts[t.v[edge]];
        glm::vec3 r = verts[t.v[(edge + 1) % 3]];

        // The agent radius, applied here rather than in the bake. Both endpoints move
        // toward the middle; a portal narrower than twice the radius collapses to its
        // midpoint rather than inverting, because a tight squeeze is still a way through
        // and a crossed portal is a path that leaves the mesh.
        const glm::vec3 along = r - l;
        const float width = glm::length(glm::vec3(along.x, 0.0f, along.z));
        if (width > 1e-6f) {
            const float inset = std::min(build.agentRadius, width * 0.5f);
            const glm::vec3 dir = along / width;
            l += dir * inset;
            r -= dir * inset;
        }
        left.push_back(l);
        right.push_back(r);
    }
    left.push_back(to.position);
    right.push_back(to.position);

    // ---------------------------------------------------------------- funnel
    //
    // Mononen's simple stupid funnel. The apex is where the path currently is; the two
    // sight lines narrow until one crosses the other, and the crossing is a corner the
    // path must actually turn.
    out.push_back(from.position);
    glm::vec3 apex = from.position;
    glm::vec3 portalLeft = from.position;
    glm::vec3 portalRight = from.position;
    size_t apexIndex = 0;
    size_t leftIndex = 0;
    size_t rightIndex = 0;

    for (size_t i = 0; i < left.size(); ++i) {
        const glm::vec3& l = left[i];
        const glm::vec3& r = right[i];

        // Right side tightens, unless it would cross the left -- in which case the left is
        // a corner and the funnel restarts from it.
        if (triArea2(apex, portalRight, r) <= 0.0f) {
            if (apex == portalRight || triArea2(apex, portalLeft, r) > 0.0f) {
                portalRight = r;
                rightIndex = i;
            } else {
                if (out.empty() || out.back() != portalLeft) out.push_back(portalLeft);
                apex = portalLeft;
                apexIndex = leftIndex;
                portalLeft = apex;
                portalRight = apex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                i = apexIndex;
                continue;
            }
        }

        if (triArea2(apex, portalLeft, l) >= 0.0f) {
            if (apex == portalLeft || triArea2(apex, portalRight, l) < 0.0f) {
                portalLeft = l;
                leftIndex = i;
            } else {
                if (out.empty() || out.back() != portalRight) out.push_back(portalRight);
                apex = portalRight;
                apexIndex = rightIndex;
                portalLeft = apex;
                portalRight = apex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                i = apexIndex;
                continue;
            }
        }
    }

    if (out.empty() || out.back() != to.position) out.push_back(to.position);

    // ---------------------------------------------------------------- smoothing
    //
    // The funnel returns the shortest path *through the corridor A\* chose*, and that is
    // not always the shortest path through the mesh: on an open floor a dozen corridors
    // tie on cost, and the one that wins need not be the one containing the straight line.
    // Detour has the same property and answers it the same way -- walk the mesh directly
    // and drop every waypoint the agent can see past.
    //
    // Quadratic in waypoints, over a list that is single digits after the funnel has done
    // its work. The alternative is a smoothing pass that only looks one waypoint ahead,
    // which cannot remove a run of three.
    if (out.size() > 2) {
        std::vector<glm::vec3> straight;
        straight.reserve(out.size());
        straight.push_back(out.front());
        size_t i = 0;
        while (i + 1 < out.size()) {
            size_t j = out.size() - 1;
            for (; j > i + 1; --j) {
                if (corridorClearNav(out[i], out[j], build.agentRadius)) break;
            }
            straight.push_back(out[j]);
            i = j;
        }
        out = std::move(straight);
    }

    // ---------------------------------------------------------------- corners only
    //
    // A waypoint on the straight line between its neighbours is not a turn. The funnel
    // emits one wherever the path runs exactly *through* a portal endpoint rather than
    // between the two -- a diagonal across a grid of square cells does it at every corner
    // it crosses -- because a zero signed area reads as the sight lines having crossed.
    // The walk above cannot take it back out: the same segment runs along the shared edges
    // it is asking about, and `raycastNav` finds no exit edge for a ray that leaves through
    // a vertex, so it answers "blocked" for a line that is entirely on the mesh.
    //
    // Which of the two survives is decided by whether the area came out as exactly zero,
    // so leaving it makes the shape of a path across an open floor a property of the
    // rounding rather than of the floor. Deviation is measured in 3D, so a ramp's crest --
    // collinear seen from above, a corner seen from the side -- is a corner and stays.
    // Dropping moves the polyline by at most `kStraight`, well under any clearance
    // `agentRadius` bought.
    constexpr float kStraight = 1e-4f;
    if (out.size() > 2) {
        std::vector<glm::vec3> corners;
        corners.reserve(out.size());
        corners.push_back(out.front());
        for (size_t i = 1; i + 1 < out.size(); ++i) {
            // Against the last waypoint *kept*, not the last one seen, so a run of three
            // collinear points collapses rather than leaving its middle behind.
            const glm::vec3 span = out[i + 1] - corners.back();
            const glm::vec3 off = out[i] - corners.back();
            const float len2 = glm::dot(span, span);
            const glm::vec3 away = len2 > 1e-12f ? off - span * (glm::dot(off, span) / len2) : off;
            if (glm::length(away) > kStraight) corners.push_back(out[i]);
        }
        corners.push_back(out.back());
        out = std::move(corners);
    }
    return true;
}

// ==================================================================== steering

glm::vec3 steer(PathFollower& follower, const glm::vec3& position, float maxSpeed) {
    // Advance past everything already reached, not just one waypoint. Two tests, and the
    // second is the one that matters: an agent is done with a waypoint either because it
    // is standing on it *or* because it is already past it along the outgoing segment. A
    // radius test alone leaves an agent that overshot in one long frame walking backwards
    // to a corner it cleared -- which does not need a teleport to happen, only 5 m/s and a
    // frame that took 400 ms.
    // Whatever the mesh called up, not +Y (D18). A follower walking a flat world's XY plane
    // has to drop Z, and a follower that dropped the wrong axis measures its progress along
    // the one axis it is not travelling on -- so it never reaches a waypoint and never
    // leaves the first one.
    const glm::vec3 up = glm::length(follower.up) > 1e-6f ? glm::normalize(follower.up) : glm::vec3(0.0f, 1.0f, 0.0f);

    while (follower.waypoint + 1 < follower.path.size()) {
        const glm::vec3& here = follower.path[follower.waypoint];
        const glm::vec3& next = follower.path[follower.waypoint + 1];
        const bool reached = glm::length(flatten(position - here, up)) <= follower.waypointRadius;
        const bool passed = glm::dot(flatten(position - here, up), flatten(next - here, up)) > 0.0f;
        if (!reached && !passed) break;
        ++follower.waypoint;
    }
    if (follower.done()) return glm::vec3(0.0f);

    const bool last = follower.waypoint + 1 == follower.path.size();
    const glm::vec3 target = follower.path[follower.waypoint];
    const glm::vec3 toTarget = flatten(target - position, up);
    const float distance = glm::length(toTarget);

    if (last && distance <= follower.arriveRadius * 0.1f) {
        follower.waypoint = follower.path.size();
        return glm::vec3(0.0f);
    }
    if (distance < 1e-5f) return glm::vec3(0.0f);

    // Horizontal only. A ramp's rise is the floor's business, not the follower's, and
    // including it would slow an agent down for climbing.
    const glm::vec3 dir = toTarget / distance;
    // Arrival, on the final waypoint alone. Easing into an intermediate corner would make
    // an agent crawl through every turn.
    const float speed = last ? maxSpeed * std::min(1.0f, distance / std::max(follower.arriveRadius, 1e-4f)) : maxSpeed;
    return dir * speed;
}

} // namespace scene
