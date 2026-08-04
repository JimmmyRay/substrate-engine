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

// ------------------------------------------------------------------ standing geometry

/// XZ. Every triangle the slope filter keeps has a normal within 89 degrees of up, so its
/// footprint has area and nothing below projects onto a line.
glm::vec2 footprint(const glm::vec3& p) { return {p.x, p.z}; }

/// Twice the signed area of a footprint triangle, on `triArea2`'s convention: **positive is
/// the winding a walkable triangle already has**, because `n.y` and this are the same
/// expression. Every side test below inherits that, so "inside" is "positive" throughout.
float area2(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    return (c.x - a.x) * (b.y - a.y) - (b.x - a.x) * (c.y - a.y);
}

/// How far `p` is inside the edge through `a` along unit `dir`, signed the same way.
float sideOf(const glm::vec2& p, const glm::vec2& a, const glm::vec2& dir) {
    return dir.y * (p.x - a.x) - dir.x * (p.y - a.y);
}

/// One blocking triangle's trace across a walkable triangle's plane, in XZ.
struct StandingCut {
    glm::vec2 a{0.0f};
    glm::vec2 b{0.0f};
};

/**
 * @brief Where a triangle meets the plane `dot(n, x) == d`, in XZ, if it *stands on* it.
 *
 * Standing on is the whole rule, and it is what lets this need no agent height. Geometry
 * that reaches above the surface and touches or crosses it is standing on it; geometry
 * entirely above -- a ceiling, a balcony, a bridge -- is not, and neither is geometry
 * entirely below, which is what the underside of the floor itself always is. **A ceiling is
 * therefore not an obstacle**, which is this file's clearance limitation restated rather
 * than a new one.
 *
 * A column's side triangles have two vertices on the floor and one ten metres up, so each
 * contributes the base edge it stands on and the ring comes out once, whole. The ones with
 * a single vertex down meet the plane in a point and are refused here.
 */
bool standingCut(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& n, float d,
                 float eps, StandingCut& out) {
    const glm::vec3 p[3] = {p0, p1, p2};
    float h[3];
    float highest = -std::numeric_limits<float>::max();
    float lowest = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        h[i] = glm::dot(n, p[i]) - d;
        highest = std::max(highest, h[i]);
        lowest = std::min(lowest, h[i]);
    }
    if (highest <= eps || lowest > eps) return false;

    glm::vec2 touch[4];
    int count = 0;
    for (int i = 0; i < 3 && count < 4; ++i) {
        const int j = (i + 1) % 3;
        if (std::abs(h[i]) <= eps) touch[count++] = footprint(p[i]);
        if (count < 4 && ((h[i] > eps && h[j] < -eps) || (h[i] < -eps && h[j] > eps))) {
            touch[count++] = footprint(glm::mix(p[i], p[j], h[i] / (h[i] - h[j])));
        }
    }
    if (count < 2) return false;

    float best = -1.0f;
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            const glm::vec2 span = touch[i] - touch[j];
            const float len2 = glm::dot(span, span);
            if (len2 <= best) continue;
            best = len2;
            out.a = touch[i];
            out.b = touch[j];
        }
    }
    return best > eps * eps;
}

/// Split convex `poly` by the line through `a` along unit `dir`. Either side comes back
/// empty when the polygon lies wholly on the other one.
void splitConvex(const std::vector<glm::vec2>& poly, const glm::vec2& a, const glm::vec2& dir, float eps,
                 std::vector<glm::vec2>& inside, std::vector<glm::vec2>& outside) {
    inside.clear();
    outside.clear();
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const glm::vec2& p = poly[i];
        const glm::vec2& q = poly[(i + 1) % n];
        const float dp = sideOf(p, a, dir);
        const float dq = sideOf(q, a, dir);
        if (dp >= -eps) inside.push_back(p);
        if (dp <= eps) outside.push_back(p);
        if ((dp > eps && dq < -eps) || (dp < -eps && dq > eps)) {
            const glm::vec2 x = p + (q - p) * (dp / (dp - dq));
            inside.push_back(x);
            outside.push_back(x);
        }
    }
    if (inside.size() < 3) inside.clear();
    if (outside.size() < 3) outside.clear();
}

/**
 * @brief Does the segment `a`..`b` pass through the interior of convex `poly`?
 *
 * **The segment and not the line it lies on**, and that is what keeps the output bounded: a
 * cut may only split a piece it actually reaches. Splitting every piece by every cut's line
 * would build the arrangement of all of them against each other, which for a floor with
 * twenty-eight rings on it is hundreds of thousands of cells rather than hundreds.
 */
bool segmentEnters(const std::vector<glm::vec2>& poly, const glm::vec2& a, const glm::vec2& b, float eps) {
    const glm::vec2 travel = b - a;
    float first = 0.0f;
    float last = 1.0f;
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const glm::vec2& p = poly[i];
        glm::vec2 edge = poly[(i + 1) % n] - p;
        const float len = glm::length(edge);
        if (len <= 1e-9f) continue;
        edge /= len;

        const float depth = sideOf(a, p, edge);
        const float rate = sideOf(a + travel, p, edge) - depth;
        if (std::abs(rate) <= 1e-9f) {
            if (depth < eps) return false;
            continue;
        }
        const float t = (eps - depth) / rate;
        if (rate > 0.0f) {
            first = std::max(first, t);
        } else {
            last = std::min(last, t);
        }
        if (first > last) return false;
    }
    return last - first > 1e-6f;
}

/**
 * @brief A bucket grid over triangle footprints, in XZ.
 *
 * Not the BVH further down this file. That one indexes the *baked* triangles and is built
 * from what this decides; this one indexes the soup that arrived, and both of the carve's
 * questions -- what stands near this floor, what is above this point -- are footprint
 * queries. Two occurrences are a coincidence.
 */
struct Footprints {
    glm::vec2 origin{0.0f};
    float cell = 1.0f;
    int32_t wide = 1;
    int32_t deep = 1;
    std::vector<std::vector<uint32_t>> buckets;

    [[nodiscard]] int32_t column(float x) const {
        return std::clamp(static_cast<int32_t>(std::floor((x - origin.x) / cell)), 0, wide - 1);
    }
    [[nodiscard]] int32_t row(float z) const {
        return std::clamp(static_cast<int32_t>(std::floor((z - origin.y) / cell)), 0, deep - 1);
    }

    void build(const std::vector<glm::vec3>& verts, const std::vector<uint32_t>& soup) {
        glm::vec2 lo(std::numeric_limits<float>::max());
        glm::vec2 hi(-std::numeric_limits<float>::max());
        for (const glm::vec3& v : verts) {
            lo = glm::min(lo, footprint(v));
            hi = glm::max(hi, footprint(v));
        }
        const uint32_t count = static_cast<uint32_t>(soup.size() / 3);
        // About one triangle a cell, capped so a sparse world does not allocate a grid it
        // will never fill.
        const auto side = static_cast<int32_t>(std::clamp(std::sqrt(static_cast<float>(count)), 1.0f, 256.0f));
        origin = lo;
        const glm::vec2 span = glm::max(hi - lo, glm::vec2(1e-3f));
        cell = std::max(span.x, span.y) / static_cast<float>(side);
        wide = std::max(1, static_cast<int32_t>(span.x / cell) + 1);
        deep = std::max(1, static_cast<int32_t>(span.y / cell) + 1);
        buckets.assign(static_cast<size_t>(wide) * static_cast<size_t>(deep), {});

        for (uint32_t t = 0; t < count; ++t) {
            glm::vec2 tlo(std::numeric_limits<float>::max());
            glm::vec2 thi(-std::numeric_limits<float>::max());
            for (uint32_t k = 0; k < 3; ++k) {
                const glm::vec2 p = footprint(verts[soup[3 * t + k]]);
                tlo = glm::min(tlo, p);
                thi = glm::max(thi, p);
            }
            for (int32_t j = row(tlo.y); j <= row(thi.y); ++j) {
                for (int32_t i = column(tlo.x); i <= column(thi.x); ++i) {
                    buckets[static_cast<size_t>(j) * static_cast<size_t>(wide) + static_cast<size_t>(i)].push_back(t);
                }
            }
        }
    }

    /// Every triangle whose footprint may overlap the box, each once.
    void gather(const glm::vec2& lo, const glm::vec2& hi, std::vector<uint32_t>& seen,
                std::vector<uint32_t>& out) const {
        out.clear();
        for (int32_t j = row(lo.y); j <= row(hi.y); ++j) {
            for (int32_t i = column(lo.x); i <= column(hi.x); ++i) {
                for (const uint32_t t : buckets[static_cast<size_t>(j) * static_cast<size_t>(wide) +
                                                static_cast<size_t>(i)]) {
                    if (seen[t] != 0u) continue;
                    seen[t] = 1u;
                    out.push_back(t);
                }
            }
        }
        for (const uint32_t t : out) seen[t] = 0u;
    }
};

/**
 * @brief Is `p` inside solid geometry -- the nearest surface above it faces up?
 *
 * **The nearest one and not the parity of all of them**, because a point that lands exactly
 * on the edge two triangles share is counted twice or not at all by a parity test, and the
 * centroid of a piece cut along a footprint's own edges lands there often. The nearest hit
 * is the same surface from either triangle, and their normals agree.
 *
 * Going up out of a solid you leave through a face pointing up -- a column's cap. Going up
 * from open floor you either meet nothing or you enter something through a face pointing
 * down, which is what the underside of a bridge is.
 */
bool insideSolid(const glm::vec3& p, const std::vector<glm::vec3>& verts, const std::vector<uint32_t>& soup,
                 const std::vector<uint32_t>& candidates) {
    float nearest = std::numeric_limits<float>::max();
    bool facingUp = false;
    for (const uint32_t t : candidates) {
        const glm::vec3& a = verts[soup[3 * t]];
        const glm::vec3& b = verts[soup[3 * t + 1]];
        const glm::vec3& c = verts[soup[3 * t + 2]];
        // A vertical triangle has no footprint for the ray to pass through, so it is neither
        // a floor nor a ceiling as far as this question goes.
        const float twice = triArea2(a, b, c);
        if (std::abs(twice) < 1e-9f) continue;

        const glm::vec2 q = footprint(p);
        const glm::vec2 fa = footprint(a);
        const glm::vec2 fb = footprint(b);
        const glm::vec2 fc = footprint(c);
        const float wa = area2(q, fb, fc) / twice;
        const float wb = area2(fa, q, fc) / twice;
        const float wc = area2(fa, fb, q) / twice;
        if (wa < 0.0f || wb < 0.0f || wc < 0.0f) continue;

        const float height = wa * a.y + wb * b.y + wc * c.y;
        if (height <= p.y || height >= nearest) continue;
        nearest = height;
        facingUp = twice > 0.0f;
    }
    return nearest < std::numeric_limits<float>::max() && facingUp;
}

/**
 * @brief Give every polygon the corners its neighbours put on its edges.
 *
 * Splitting each piece on its own leaves T-junctions: a cut that ends on an edge two pieces
 * share is a corner one of them has and the other does not, and the two then agree on a line
 * in space and disagree about which vertices are on it. The weld cannot close that -- the
 * positions are distinct and correct -- so the adjacency pass sees two edges rather than one
 * and the surface comes apart into a region per piece.
 *
 * One pass is enough because inserting a corner introduces no new corner: the point set is
 * whatever the splitting already produced.
 */
void closeTJunctions(std::vector<std::vector<glm::vec3>>& loops, float weld) {
    std::vector<glm::vec3> corners;
    for (const std::vector<glm::vec3>& loop : loops) corners.insert(corners.end(), loop.begin(), loop.end());
    if (corners.empty()) return;

    // The same bucket idea as the footprints, over points rather than triangles.
    glm::vec2 lo(std::numeric_limits<float>::max());
    glm::vec2 hi(-std::numeric_limits<float>::max());
    for (const glm::vec3& p : corners) {
        lo = glm::min(lo, footprint(p));
        hi = glm::max(hi, footprint(p));
    }
    const glm::vec2 span = glm::max(hi - lo, glm::vec2(1e-3f));
    const auto side =
        static_cast<int32_t>(std::clamp(std::sqrt(static_cast<float>(corners.size())), 1.0f, 256.0f));
    const float cell = std::max(std::max(span.x, span.y) / static_cast<float>(side), weld);
    const int32_t wide = std::max(1, static_cast<int32_t>(span.x / cell) + 1);
    const int32_t deep = std::max(1, static_cast<int32_t>(span.y / cell) + 1);
    const auto column = [&](float x) {
        return std::clamp(static_cast<int32_t>(std::floor((x - lo.x) / cell)), 0, wide - 1);
    };
    const auto row = [&](float z) {
        return std::clamp(static_cast<int32_t>(std::floor((z - lo.y) / cell)), 0, deep - 1);
    };

    std::vector<std::vector<uint32_t>> buckets(static_cast<size_t>(wide) * static_cast<size_t>(deep));
    for (uint32_t i = 0; i < corners.size(); ++i) {
        buckets[static_cast<size_t>(row(corners[i].z)) * static_cast<size_t>(wide) +
                static_cast<size_t>(column(corners[i].x))]
            .push_back(i);
    }

    const float weld2 = weld * weld;
    std::vector<std::pair<float, glm::vec3>> inserts;
    std::vector<glm::vec3> rebuilt;
    for (std::vector<glm::vec3>& loop : loops) {
        rebuilt.clear();
        for (size_t i = 0; i < loop.size(); ++i) {
            const glm::vec3& a = loop[i];
            const glm::vec3& b = loop[(i + 1) % loop.size()];
            rebuilt.push_back(a);

            const glm::vec3 edge = b - a;
            const float len2 = glm::dot(edge, edge);
            if (len2 <= weld2) continue;

            inserts.clear();
            const glm::vec2 elo = glm::min(footprint(a), footprint(b));
            const glm::vec2 ehi = glm::max(footprint(a), footprint(b));
            for (int32_t j = row(elo.y); j <= row(ehi.y); ++j) {
                for (int32_t k = column(elo.x); k <= column(ehi.x); ++k) {
                    for (const uint32_t c : buckets[static_cast<size_t>(j) * static_cast<size_t>(wide) +
                                                    static_cast<size_t>(k)]) {
                        const glm::vec3& p = corners[c];
                        const float along = glm::dot(p - a, edge) / len2;
                        // Strictly between, and by more than the weld: an endpoint is already
                        // this edge's, and a point within the weld of one is that one.
                        if (along <= 0.0f || along >= 1.0f) continue;
                        const glm::vec3 offset = p - (a + edge * along);
                        if (glm::dot(offset, offset) > weld2) continue;
                        if (glm::dot(p - a, p - a) <= weld2 || glm::dot(p - b, p - b) <= weld2) continue;
                        inserts.emplace_back(along, p);
                    }
                }
            }
            if (inserts.empty()) continue;

            std::sort(inserts.begin(), inserts.end(),
                      [](const auto& x, const auto& y) { return x.first < y.first; });
            for (const auto& insert : inserts) {
                if (!rebuilt.empty() && glm::dot(rebuilt.back() - insert.second, rebuilt.back() - insert.second) <= weld2) {
                    continue;
                }
                rebuilt.push_back(insert.second);
            }
        }
        loop = rebuilt;
    }
}

/**
 * @brief Cut the walkable surface where solid geometry stands on it, rewriting the soup.
 *
 * The bake filters by slope and keeps what an agent could stand on. That silently answers a
 * question nobody asked: a floor authored as one large quad with props resting on it stays
 * one large quad, so every route across it is a straight line through every prop. The
 * geometry that proves the prop is there -- its sides, too steep to walk and thrown away by
 * the very next test -- is in this soup, and this is what reads it.
 *
 * Each walkable triangle is split by the traces of whatever stands on its plane, and the
 * pieces that end up inside something are dropped. The pieces are convex throughout, which
 * is what makes the splitting four lines of arithmetic rather than a triangulator, and no
 * length is quantised anywhere: this adds no cell size to tune and no rasterisation error,
 * which is the property the whole triangle approach is here for.
 */
void cutStandingGeometry(std::vector<glm::vec3>& verts, std::vector<uint32_t>& soup, const NavBuildParams& params) {
    auto zone = core::Profiler::scope("NavMesh::cutStandingGeometry");
    const uint32_t count = static_cast<uint32_t>(soup.size() / 3);
    if (count == 0) return;

    const float cosLimit = std::cos(glm::radians(std::clamp(params.walkableSlopeDegrees, 0.0f, 89.0f)));
    const float weld = std::max(params.weldEpsilon, 1e-6f);
    // Well under the weld, so a piece this splitting leaves too thin to matter collapses in
    // the weld that follows rather than surviving as a sliver with an opinion about adjacency.
    const float flat = weld * 0.01f;

    std::vector<glm::vec3> normals(count, glm::vec3(0.0f));
    std::vector<uint8_t> walkable(count, 0u);
    uint32_t standing = 0;
    uint32_t floors = 0;
    for (uint32_t t = 0; t < count; ++t) {
        const glm::vec3& a = verts[soup[3 * t]];
        const glm::vec3 n = glm::cross(verts[soup[3 * t + 1]] - a, verts[soup[3 * t + 2]] - a);
        const float len = glm::length(n);
        if (len < 1e-12f) continue;
        normals[t] = n / len;
        if (normals[t].y >= cosLimit) {
            walkable[t] = 1u;
            ++floors;
        } else {
            ++standing;
        }
    }
    // Nothing stands on anything, or there is nothing for it to stand on.
    if (standing == 0 || floors == 0) return;

    Footprints grid;
    grid.build(verts, soup);

    std::vector<uint32_t> seen(count, 0u);
    std::vector<uint32_t> nearby;
    std::vector<StandingCut> cuts;
    std::vector<std::vector<glm::vec2>> pieces;
    std::vector<glm::vec2> inside;
    std::vector<glm::vec2> outside;

    std::vector<uint32_t> out;
    out.reserve(soup.size());

    // **Every walkable triangle becomes a polygon here, cut or not.** A triangle nothing
    // stands on still has a neighbour that was cut, and the point that cut landed on their
    // shared edge is a vertex one of them has and the other does not -- which is a T-junction,
    // and a T-junction is two triangles that visibly touch and are not adjacent. The repair
    // below needs every boundary in one place to close them.
    std::vector<std::vector<glm::vec3>> kept;

    for (uint32_t t = 0; t < count; ++t) {
        const uint32_t base = 3 * t;
        if (walkable[t] == 0u) {
            // Passed through rather than dropped: the slope filter is still the one that
            // decides what a surface is, and it runs over what this hands back.
            out.insert(out.end(), {soup[base], soup[base + 1], soup[base + 2]});
            continue;
        }

        const glm::vec3& a = verts[soup[base]];
        const glm::vec3& b = verts[soup[base + 1]];
        const glm::vec3& c = verts[soup[base + 2]];
        const glm::vec3 n = normals[t];
        const float d = glm::dot(n, a);

        const glm::vec2 fa = footprint(a);
        const glm::vec2 fb = footprint(b);
        const glm::vec2 fc = footprint(c);
        const glm::vec2 lo = glm::min(fa, glm::min(fb, fc));
        const glm::vec2 hi = glm::max(fa, glm::max(fb, fc));

        cuts.clear();
        grid.gather(lo, hi, seen, nearby);
        for (const uint32_t other : nearby) {
            if (walkable[other] != 0u || other == t) continue;
            StandingCut cut;
            if (!standingCut(verts[soup[3 * other]], verts[soup[3 * other + 1]], verts[soup[3 * other + 2]], n, d,
                             weld, cut)) {
                continue;
            }
            cuts.push_back(cut);
        }

        // The triangle's own winding is already the one every side test below assumes: `n.y`
        // and `area2` are the same expression, so a walkable triangle is a positively wound
        // footprint by construction.
        pieces.clear();
        pieces.push_back({fa, fb, fc});
        for (const StandingCut& cut : cuts) {
            glm::vec2 dir = cut.b - cut.a;
            const float len = glm::length(dir);
            if (len <= flat) continue;
            dir /= len;
            for (size_t i = 0; i < pieces.size();) {
                if (!segmentEnters(pieces[i], cut.a, cut.b, flat)) {
                    ++i;
                    continue;
                }
                splitConvex(pieces[i], cut.a, dir, flat, inside, outside);
                if (inside.empty() || outside.empty()) {
                    ++i;
                    continue;
                }
                pieces[i] = inside;
                pieces.push_back(outside);
                ++i;
            }
        }

        for (const std::vector<glm::vec2>& piece : pieces) {
            glm::vec2 centre(0.0f);
            for (const glm::vec2& p : piece) centre += p;
            centre /= static_cast<float>(piece.size());
            // On the surface and a hair above it, so the floor it sits on is below the ray
            // rather than the first thing the ray meets.
            const glm::vec3 stand((centre.x), (d - n.x * centre.x - n.z * centre.y) / n.y + weld, centre.y);

            glm::vec2 plo = piece[0];
            glm::vec2 phi = piece[0];
            for (const glm::vec2& p : piece) {
                plo = glm::min(plo, p);
                phi = glm::max(phi, p);
            }
            // **Asked of every piece and not only of the ones something cut.** A floor
            // tessellated finer than the prop standing on it has triangles entirely within the
            // footprint, and no trace crosses those -- they are simply inside, and a cut that
            // only looked at what it had split would leave them walkable.
            grid.gather(plo, phi, seen, nearby);
            if (insideSolid(stand, verts, soup, nearby)) continue;

            std::vector<glm::vec3> loop;
            loop.reserve(piece.size());
            for (const glm::vec2& p : piece) loop.push_back({p.x, (d - n.x * p.x - n.z * p.y) / n.y, p.y});
            kept.push_back(std::move(loop));
        }
    }

    closeTJunctions(kept, weld);

    for (const std::vector<glm::vec3>& loop : kept) {
        const auto first = static_cast<uint32_t>(verts.size());
        if (loop.size() == 3) {
            // A triangle nothing cut, emitted as itself. The weld puts these back on the
            // vertices they arrived as, so a surface with nothing standing on it is the
            // surface it was.
            verts.insert(verts.end(), loop.begin(), loop.end());
            out.insert(out.end(), {first, first + 1, first + 2});
            continue;
        }
        // **A fan from a corner, and not from the middle.** A fan rooted inside the piece is a
        // pinwheel: the two ways round it are the same length to a corridor search comparing
        // centroids, so it takes whichever, and the funnel then has to pull the path through
        // the portals of the wrong half. A fan from a vertex is a strip -- one way through --
        // and because the piece is convex the straight line crosses its portals in order.
        //
        // The root has to be a corner whose own two edges carry no split point, or the first
        // or last triangle of the fan is three collinear points: no area, so no edge for the
        // piece on the other side to be adjacent across.
        const uint32_t n = static_cast<uint32_t>(loop.size());
        uint32_t root = n;
        for (uint32_t i = 0; i < n && root == n; ++i) {
            const glm::vec3& here = loop[i];
            const auto clear = [&](const glm::vec3& p, const glm::vec3& q) {
                const float base = distanceXZ(p, q);
                return base > 1e-6f && std::abs(triArea2(here, p, q)) > weld * base;
            };
            if (clear(loop[(i + 1) % n], loop[(i + 2) % n]) && clear(loop[(i + n - 2) % n], loop[(i + n - 1) % n])) {
                root = i;
            }
        }
        if (root == n) {
            // Every corner has a split point beside it. A centre vertex always works and
            // never degenerates; it only costs the path quality argued for above.
            glm::vec3 centre(0.0f);
            for (const glm::vec3& p : loop) centre += p;
            verts.push_back(centre / static_cast<float>(n));
            verts.insert(verts.end(), loop.begin(), loop.end());
            for (uint32_t k = 0; k < n; ++k) out.insert(out.end(), {first, first + 1 + k, first + 1 + (k + 1) % n});
            continue;
        }
        verts.insert(verts.end(), loop.begin(), loop.end());
        for (uint32_t k = 1; k + 1 < n; ++k) {
            out.insert(out.end(), {first + root, first + (root + k) % n, first + (root + k + 1) % n});
        }
    }

    soup.swap(out);
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

    // ---------------------------------------------------------------- standing geometry
    //
    // **Before the weld and in the solver's frame**, because everything the carve decides is
    // a question about up: what is standing on what, and which way a surface faces. It hands
    // back a soup, so every stage below is unchanged and still the one deciding what a
    // walkable surface is -- this only makes sure the surface has the props cut out of it.
    std::vector<glm::vec3> nav(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) nav[i] = toNav(positions[i]);
    std::vector<uint32_t> soup = indices;
    cutStandingGeometry(nav, soup, params);

    // ---------------------------------------------------------------- weld
    //
    // A triangle soup has no adjacency: two floor tiles that visibly share an edge have
    // four distinct corners at two positions, and every one of them came from a different
    // vertex in the source buffer because they carried different normals or UVs. Welding
    // on position alone is what turns that back into a surface.
    const float weld = std::max(params.weldEpsilon, 1e-6f);
    const float weld2 = weld * weld;
    std::unordered_map<size_t, std::vector<uint32_t>> grid;
    std::vector<uint32_t> remap(nav.size(), 0);

    for (uint32_t i = 0; i < nav.size(); ++i) {
        // Already in the solver's frame: the carve above is where the world entered, and
        // everything welded, filtered, indexed and stored below stays in that frame.
        const glm::vec3 p = nav[i];
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
    for (size_t i = 0; i + 2 < soup.size(); i += 3) {
        uint32_t a = remap[soup[i]];
        uint32_t b = remap[soup[i + 1]];
        uint32_t c = remap[soup[i + 2]];
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
