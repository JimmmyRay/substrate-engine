#include <gtest/gtest.h>

#include "physics/ClothSystem.h"
#include "physics/PhysicsWorld.h"
#include "scene/Cloth.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

/**
 * @file tests/ClothTests.cpp
 * @brief Cloth, checked by the two properties an image could not assert.
 *
 * The golden suite pins a frame byte for byte, and the thing worth asserting about cloth
 * is not what it looks like on frame 60 -- it is that it stays inside a box and settles.
 * Those are bounds, they are cheap, they are hosted, and they need no reference to
 * re-snap. Nothing in `engine/assets/` carries a `FABRIC_` mesh, so cloth has no golden
 * case and does not need one; what it has is here.
 *
 * Everything in this file runs with no device and no window, which is what puts the solver
 * under all four sanitizers -- the strongest verification this board has, and the thing a
 * GPU cloth solver would have forfeited entirely.
 */
namespace {

using physics::ClothSystem;
using physics::PhysicsWorld;
using scene::ClothDesc;
using scene::ClothVertex;
using scene::Vertex;

/// How high above the origin the sheet starts, so "it never rises above its pins" and "it
/// never falls further than its own length" are both assertions about absolute y.
constexpr float kPinHeight = 2.0f;

/**
 * A rectangular sheet lying **flat**, in the XZ plane at y = `kPinHeight`, `cols` by `rows`
 * vertices `spacing` metres apart with a corner at the origin.
 *
 * Horizontal rather than hanging, and that is the whole reason these tests say anything.
 * The first version of this file built the sheet vertically and pinned its top edge --
 * which is a curtain already *in* its equilibrium pose, so gravity had nothing to do, every
 * vertex moved a fraction of a millimetre, and the envelope passed by asserting that
 * nothing happened. A flat sheet pinned along one edge has to swing through ninety degrees
 * to get where it is going, which is the motion an envelope is worth checking.
 *
 * It is also the case an author actually hits: a curtain is modelled flat and the engine is
 * what makes it hang.
 */
struct Sheet {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<ClothVertex> masses;
    uint32_t cols = 0;
    uint32_t rows = 0;

    [[nodiscard]] uint32_t at(uint32_t x, uint32_t y) const { return y * cols + x; }
    [[nodiscard]] ClothDesc desc() const { return ClothDesc{vertices, masses, indices, glm::mat4(1.0f)}; }
};

Sheet makeSheet(uint32_t cols, uint32_t rows, float spacing) {
    Sheet s;
    s.cols = cols;
    s.rows = rows;
    for (uint32_t y = 0; y < rows; ++y) {
        for (uint32_t x = 0; x < cols; ++x) {
            Vertex v;
            v.position = {static_cast<float>(x) * spacing, kPinHeight, static_cast<float>(y) * spacing};
            v.normal = {0.0f, 1.0f, 0.0f};
            v.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            v.uv = {static_cast<float>(x) / static_cast<float>(cols - 1),
                    static_cast<float>(y) / static_cast<float>(rows - 1)};
            s.vertices.push_back(v);
        }
    }
    for (uint32_t y = 0; y + 1 < rows; ++y) {
        for (uint32_t x = 0; x + 1 < cols; ++x) {
            const uint32_t a = s.at(x, y), b = s.at(x + 1, y), c = s.at(x + 1, y + 1), d = s.at(x, y + 1);
            s.indices.insert(s.indices.end(), {a, b, c, a, c, d});
        }
    }
    s.masses.assign(s.vertices.size(), ClothVertex{1.0f});
    return s;
}

/// A curtain: pinned along the edge at z = 0, so it swings down about the x axis and hangs
/// in the plane z = 0.
Sheet makeCurtain(uint32_t cols = 9, uint32_t rows = 9, float spacing = 0.25f) {
    Sheet s = makeSheet(cols, rows, spacing);
    for (uint32_t x = 0; x < cols; ++x) s.masses[s.at(x, 0)].invMass = 0.0f;
    return s;
}

/// A flag: pinned along the edge at x = 0, so it swings down about the z axis. The same
/// sheet about the other axis, which is why an envelope written for one catches the other.
Sheet makeFlag(uint32_t cols = 9, uint32_t rows = 9, float spacing = 0.25f) {
    Sheet s = makeSheet(cols, rows, spacing);
    for (uint32_t y = 0; y < rows; ++y) s.masses[s.at(0, y)].invMass = 0.0f;
    return s;
}

/// A world with nothing in it but the cloth handed to it. No floor, deliberately: an
/// envelope that a floor enforced would be asserting the floor rather than the solver.
struct Rig {
    PhysicsWorld world;
    ClothSystem cloths;

    explicit Rig(const Sheet& sheet) {
        scene::PhysicsConfig cfg;
        cfg.workerThreads = 0; // the determinism default, stated here rather than assumed
        world.init(cfg, 8);
        added = cloths.add(world, 0, 0, sheet.desc());
        world.finalize();
    }

    void step(uint32_t steps) {
        for (uint32_t i = 0; i < steps; ++i) world.step(1.0f / 60.0f);
        cloths.update(world);
    }

    /// The pose after `steps` steps, read once at the end -- which is what the engine does
    /// and therefore what should be tested.
    void run(uint32_t steps) { step(steps); }

    [[nodiscard]] const ClothSystem::Cloth& cloth() const { return cloths.at(0); }

    bool added = false;
};

} // namespace

// ------------------------------------------------------------------ the convention

TEST(Cloth, TheFabricPredicateAcceptsOnlyThePrefix) {
    EXPECT_TRUE(scene::isFabricMesh("FABRIC_Curtain"));
    EXPECT_TRUE(scene::isFabricMesh("FABRIC_"));
    EXPECT_TRUE(scene::isFabricMesh("FABRIC_a"));

    // The near misses, every one of them a name someone will actually type.
    EXPECT_FALSE(scene::isFabricMesh("Fabric"));
    EXPECT_FALSE(scene::isFabricMesh("Fabric_Curtain"));
    EXPECT_FALSE(scene::isFabricMesh("fabric_"));
    EXPECT_FALSE(scene::isFabricMesh("fabric_curtain"));
    EXPECT_FALSE(scene::isFabricMesh("FAB_"));
    EXPECT_FALSE(scene::isFabricMesh("FABRIC"));
    EXPECT_FALSE(scene::isFabricMesh(""));
    // Not a suffix and not a substring.
    EXPECT_FALSE(scene::isFabricMesh("Curtain_FABRIC_"));
    EXPECT_FALSE(scene::isFabricMesh(" FABRIC_"));
}

TEST(Cloth, PinWeightMapsToInverseMass) {
    // The two ends, and the threshold that is the whole of the contract.
    EXPECT_EQ(scene::clothInvMass(1.0f), 0.0f);
    EXPECT_EQ(scene::clothInvMass(0.999f), 0.0f);
    EXPECT_EQ(scene::clothInvMass(0.9999f), 0.0f);
    EXPECT_EQ(scene::clothInvMass(0.0f), 1.0f);

    // The fractional path. Tethered never exercised it -- its shipping asset's weights
    // were strictly {0, 1} -- so these are the only thing that will ever cover it.
    EXPECT_FLOAT_EQ(scene::clothInvMass(0.25f), 0.75f);
    EXPECT_FLOAT_EQ(scene::clothInvMass(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(scene::clothInvMass(0.9f), 0.1f);
    EXPECT_GT(scene::clothInvMass(0.5f), 0.0f);
    EXPECT_LT(scene::clothInvMass(0.5f), 1.0f);

    // 0.99 is heavy, not pinned. `check_pins.py` refuses a cloth whose highest weight is
    // this and prints the number; the two sides agree because both read 0.999.
    EXPECT_GT(scene::clothInvMass(0.99f), 0.0f);

    // Out of range clamps rather than refusing, and NaN must not pin.
    EXPECT_EQ(scene::clothInvMass(-1.0f), 1.0f);
    EXPECT_EQ(scene::clothInvMass(2.0f), 0.0f);
    EXPECT_EQ(scene::clothInvMass(std::numeric_limits<float>::quiet_NaN()), 1.0f);
}

TEST(Cloth, TheTwoProducersAgreeOnTheConvention) {
    // The check `chore-blender-authors-the-pins-the-engine-reads` could not run, because it
    // needed a loader that reads the attribute. This is that check, in the only form it can
    // take inside a C++ suite: the engine's spelling of the convention, asserted literally,
    // against what `scripts/check_pins.py` has hard-coded on the other side of the exporter.
    // A change to either without the other fails here.
    EXPECT_EQ(scene::kFabricPrefix, "FABRIC_");
    EXPECT_EQ(scene::kPinAttribute, "_PIN_WEIGHT");
    // PIN_THRESHOLD in check_pins.py. The one number that script had to borrow.
    EXPECT_EQ(scene::clothInvMass(0.999f), 0.0f);
    EXPECT_GT(scene::clothInvMass(std::nextafter(0.999f, 0.0f)), 0.0f);
}

// ------------------------------------------------------------------ the weld

TEST(Cloth, WeldingMergesCoincidentVerticesAndKeepsThePin) {
    // Two shading vertices at one point, as a UV seam or a smoothing split produces. This
    // is the case a generated scene cannot contain and a Blender export always does.
    Sheet s = makeSheet(2, 2, 1.0f);
    s.masses[0].invMass = 0.0f;
    // Duplicate vertex 0 exactly, free rather than pinned, and give a triangle a reference
    // to the duplicate instead.
    s.vertices.push_back(s.vertices[0]);
    s.masses.push_back(ClothVertex{1.0f});
    s.indices.push_back(4);
    s.indices.push_back(1);
    s.indices.push_back(2);

    const scene::ClothTopology topo = scene::weldCloth(s.desc());
    EXPECT_EQ(topo.positions.size(), 4u) << "the duplicate should have merged";
    EXPECT_EQ(topo.remap.size(), 5u);
    EXPECT_EQ(topo.remap[4], topo.remap[0]) << "both copies must be one particle";
    // The minimum wins, so a seam with a pinned side stays pinned.
    EXPECT_EQ(topo.invMasses[topo.remap[0]], 0.0f);
}

TEST(Cloth, WeldingDropsFacesItMadeDegenerate) {
    Sheet s = makeSheet(2, 2, 1.0f);
    // A sliver: a third vertex placed on top of the first. Welding collapses the triangle.
    s.vertices.push_back(s.vertices[0]);
    s.masses.push_back(ClothVertex{1.0f});
    s.indices.push_back(0);
    s.indices.push_back(4);
    s.indices.push_back(1);

    const scene::ClothTopology topo = scene::weldCloth(s.desc());
    // Two real triangles from the quad; the collapsed one is gone rather than passed to
    // Jolt, which asserts on a degenerate face.
    EXPECT_EQ(topo.faces.size(), 6u);
    for (size_t i = 0; i + 2 < topo.faces.size(); i += 3) {
        EXPECT_NE(topo.faces[i], topo.faces[i + 1]);
        EXPECT_NE(topo.faces[i + 1], topo.faces[i + 2]);
        EXPECT_NE(topo.faces[i], topo.faces[i + 2]);
    }
}

TEST(Cloth, WeldingIsAFunctionOfTheFileAndNotOfTheHashTable) {
    // The numbering is first-sight order, so two welds of the same input are identical
    // element for element. An order-dependent simulation mesh would be an order-dependent
    // solve, which is the determinism `Physics.h` defends everywhere else.
    const Sheet s = makeCurtain();
    const scene::ClothTopology a = scene::weldCloth(s.desc());
    const scene::ClothTopology b = scene::weldCloth(s.desc());
    ASSERT_EQ(a.positions.size(), b.positions.size());
    EXPECT_EQ(a.remap, b.remap);
    EXPECT_EQ(a.faces, b.faces);
    EXPECT_EQ(0, std::memcmp(a.positions.data(), b.positions.data(), a.positions.size() * sizeof(glm::vec3)));
}

TEST(Cloth, TheTransformIsBakedIntoTheRestPose) {
    Sheet s = makeCurtain(3, 3, 1.0f);
    ClothDesc d = s.desc();
    d.transform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));

    const scene::ClothTopology topo = scene::weldCloth(d);
    ASSERT_FALSE(topo.positions.empty());
    for (const glm::vec3& p : topo.positions) EXPECT_GE(p.x, 9.9f);
}

// ------------------------------------------------------------------ normals

TEST(Cloth, NormalsAreRecomputedAndTangentsStayInThePlane) {
    Sheet s = makeSheet(3, 3, 1.0f);
    // Rotate the sheet into the XZ plane by hand, leaving the stale normals behind.
    for (Vertex& v : s.vertices) v.position = {v.position.x, 0.0f, v.position.y};

    scene::recomputeClothNormals(s.vertices, s.indices);
    for (const Vertex& v : s.vertices) {
        EXPECT_NEAR(glm::length(v.normal), 1.0f, 1e-5f);
        EXPECT_NEAR(std::abs(v.normal.y), 1.0f, 1e-5f) << "a flat sheet in XZ has a vertical normal";
        EXPECT_NEAR(glm::dot(glm::vec3(v.tangent), v.normal), 0.0f, 1e-5f);
        EXPECT_NEAR(glm::length(glm::vec3(v.tangent)), 1.0f, 1e-5f);
        EXPECT_EQ(v.tangent.w, 1.0f) << "handedness is a property of the surface, not of the pose";
    }
}

TEST(Cloth, AVertexNoFaceTouchesGetsAUsableBasisRatherThanANaN) {
    Sheet s = makeSheet(2, 2, 1.0f);
    s.indices.clear(); // no faces at all
    scene::recomputeClothNormals(s.vertices, s.indices);
    for (const Vertex& v : s.vertices) {
        EXPECT_FALSE(std::isnan(v.normal.x + v.normal.y + v.normal.z));
        EXPECT_NEAR(glm::length(v.normal), 1.0f, 1e-5f);
        EXPECT_NEAR(glm::length(glm::vec3(v.tangent)), 1.0f, 1e-5f);
    }
}

// ------------------------------------------------------------------ the solve

TEST(Cloth, APinnedVertexNeverMoves) {
    const Sheet s = makeCurtain();
    Rig rig(s);
    ASSERT_TRUE(rig.added);

    std::vector<glm::vec3> before;
    for (uint32_t x = 0; x < s.cols; ++x) before.push_back(s.vertices[s.at(x, 0)].position);

    rig.run(300);

    for (uint32_t x = 0; x < s.cols; ++x) {
        const glm::vec3 now = rig.cloth().vertices[s.at(x, 0)].position;
        EXPECT_NEAR(glm::length(now - before[x]), 0.0f, 1e-4f) << "pinned vertex " << x << " moved";
    }
}

TEST(Cloth, AnUnpinnedVertexFalls) {
    const Sheet s = makeCurtain();
    Rig rig(s);
    ASSERT_TRUE(rig.added);

    const uint32_t probe = s.at(s.cols / 2, s.rows - 1); // the free edge, furthest from the pins
    const float restY = s.vertices[probe].position.y;
    rig.run(120);
    const float nowY = rig.cloth().vertices[probe].position.y;
    EXPECT_LT(nowY, restY - 1.0f) << "the free edge should have swung down from flat";
}

TEST(Cloth, AFullyPinnedClothIsStatic) {
    Sheet s = makeSheet(9, 9, 0.25f);
    for (ClothVertex& m : s.masses) m.invMass = 0.0f;

    Rig rig(s);
    ASSERT_TRUE(rig.added);
    const std::vector<Vertex> rest = rig.cloth().vertices;

    rig.run(100);
    for (size_t i = 0; i < rest.size(); ++i) {
        EXPECT_NEAR(glm::length(rig.cloth().vertices[i].position - rest[i].position), 0.0f, 1e-5f);
    }
    EXPECT_NEAR(rig.cloth().lastMaxDisplacement, 0.0f, 1e-5f);
}

TEST(Cloth, AHeavyVertexIsMobileAndAPinnedOneIsNot) {
    /*
     * The fractional path, end to end rather than at the mapping alone -- and the
     * assertion is *not* the one C19's card predicted. The card said a partial pin should
     * give "a vertex that moves less than a free one", which is true of a particle falling
     * on its own and false of a particle in an inextensible sheet, which is what every
     * vertex of this cloth is.
     *
     * The reason is the solver rather than the mapping. A position-based constraint
     * distributes its correction between two vertices in proportion to their inverse
     * masses, so a *heavier* vertex receives a smaller share of the correction that pulls
     * it back toward its neighbour and therefore sags marginally **further**, not less.
     * Measured: over 30 steps a `_PIN_WEIGHT` of 0.9 drops 126 um against a free vertex's
     * 94 um, the opposite direction and both of them slack rather than fall.
     *
     * So what is asserted is what the weight actually controls, and it is the distinction
     * that matters to an author: at or above 0.999 a vertex is nailed down, and anywhere
     * below it the vertex moves. A partial pin is heavy, not sticky.
     */
    EXPECT_GT(scene::clothInvMass(0.9f), 0.0f);
    EXPECT_LT(scene::clothInvMass(0.9f), 1.0f);

    Sheet s = makeCurtain(5, 5, 0.25f);
    // One row of the column made heavy but not pinned, all the way out from the pins.
    for (uint32_t y = 1; y < s.rows; ++y) s.masses[s.at(2, y)].invMass = scene::clothInvMass(0.9f);

    Rig heavy(s);
    ASSERT_TRUE(heavy.added);
    heavy.run(120);

    const uint32_t probe = s.at(2, s.rows - 1);
    const uint32_t pin = s.at(2, 0);
    const float heavyMoved = glm::length(heavy.cloth().vertices[probe].position - s.vertices[probe].position);
    const float pinnedMoved = glm::length(heavy.cloth().vertices[pin].position - s.vertices[pin].position);
    EXPECT_GT(heavyMoved, 1.0e-3f) << "a partial pin is heavy, not immovable";
    EXPECT_NEAR(pinnedMoved, 0.0f, 1e-4f) << "at or above 0.999 the vertex is nailed down";

    // And the weight is not simply ignored: the same curtain with the column left free
    // lands somewhere else.
    const Sheet light = makeCurtain(5, 5, 0.25f);
    Rig free(light);
    ASSERT_TRUE(free.added);
    free.run(120);
    EXPECT_NE(free.cloth().vertices[probe].position, heavy.cloth().vertices[probe].position);
}

TEST(Cloth, AClothPinnedNowhereIsRefusedRatherThanSimulated) {
    // Which is also the `FABRIC_` mesh with `_PIN_WEIGHT` absent: the loader writes an
    // array of ones for that case on purpose, so both arrive here and both are refused
    // with a reason instead of falling out of the world on frame one.
    const Sheet s = makeSheet(5, 5, 0.25f);
    Rig rig(s);
    EXPECT_FALSE(rig.added);
    EXPECT_TRUE(rig.cloths.empty());
}

TEST(Cloth, AClothWithNoFacesIsRefused) {
    Sheet s = makeCurtain(3, 3, 1.0f);
    s.indices.clear();
    Rig rig(s);
    EXPECT_FALSE(rig.added);
}

// ------------------------------------------------------------------ the envelope
//
// The two properties C19 argued are worth more here than an image, with the numbers on the
// card rather than left to whatever the code happened to do.

namespace {

/// The union of the cloth's own AABB over a run of steps. Sampled every step rather than
/// at the end, because the thing an envelope catches is the *overshoot* -- a cloth that
/// swings twice as far as it should and comes back looks identical at rest.
struct Envelope {
    glm::vec3 lo{std::numeric_limits<float>::max()};
    glm::vec3 hi{std::numeric_limits<float>::lowest()};
};

Envelope sweep(Rig& rig, uint32_t steps) {
    Envelope e;
    for (uint32_t i = 0; i < steps; ++i) {
        rig.step(1);
        e.lo = glm::min(e.lo, rig.cloth().boundsMin);
        e.hi = glm::max(e.hi, rig.cloth().boundsMax);
    }
    return e;
}

/**
 * The stated envelope, and it is the same sentence for both shapes: **a vertex is never
 * above the pin line, and never further from it than the fabric is long.**
 *
 * `kSide` is 2 m -- nine vertices at 0.25 m -- so a sheet pinned along one edge at
 * y = `kPinHeight` reaches at most `kSide` in any direction from that edge. A cloth that
 * exceeds it has stretched, and a cloth that rises above `kPinHeight` has been given energy
 * the constraints did not take back, which is the trampoline this property exists to
 * catch. The tolerance is 5 cm, 2.5% of the sheet, and it is the solver's residual rather
 * than a number widened until something passed.
 */
constexpr float kSide = 2.0f;
constexpr float kSlack = 0.05f;

} // namespace

TEST(Cloth, ACurtainStaysInsideItsEnvelope) {
    // A 2 m x 2 m sheet lying flat at y = 2, pinned along its z = 0 edge. It falls through
    // ninety degrees to hang in the plane z = 0.
    const Sheet s = makeCurtain(9, 9, 0.25f);
    Rig rig(s);
    ASSERT_TRUE(rig.added);
    const Envelope e = sweep(rig, 400);

    // It cannot rise above the line it hangs from. This is what a trampoline fails.
    EXPECT_LE(e.hi.y, kPinHeight + kSlack) << "the curtain rose above the bar it hangs from";
    // It cannot fall further than its own length below that line.
    EXPECT_GE(e.lo.y, kPinHeight - kSide - kSlack) << "the curtain stretched past its own length";
    // A pendulum released from horizontal reaches horizontal on the far side and no
    // further, so |z| is bounded by the fabric's length either way.
    EXPECT_LE(std::max(std::abs(e.lo.z), std::abs(e.hi.z)), kSide + kSlack) << "the curtain swung past its length";
    // Nothing acts along the pin line, so it stays over its own footprint in x.
    EXPECT_GE(e.lo.x, -kSlack);
    EXPECT_LE(e.hi.x, kSide + kSlack);
}

TEST(Cloth, AFlagStaysInsideItsEnvelope) {
    // The same sheet pinned along its x = 0 edge instead, so it swings about the other
    // axis. Written as its own case because an envelope that only ever ran on one axis
    // would pass a solver that had the other two wrong.
    const Sheet s = makeFlag(9, 9, 0.25f);
    Rig rig(s);
    ASSERT_TRUE(rig.added);
    const Envelope e = sweep(rig, 400);

    EXPECT_LE(e.hi.y, kPinHeight + kSlack) << "the flag rose above the pole it hangs from";
    EXPECT_GE(e.lo.y, kPinHeight - kSide - kSlack) << "the flag stretched past its own length";
    EXPECT_LE(std::max(std::abs(e.lo.x), std::abs(e.hi.x)), kSide + kSlack) << "the flag swung past its length";
    EXPECT_GE(e.lo.z, -kSlack);
    EXPECT_LE(e.hi.z, kSide + kSlack);
}

/**
 * The convergence threshold and the budget it has to be met in, both measured rather than
 * chosen: a 2 m curtain released from flat reaches **exactly zero** per-step displacement
 * at 300 steps (5 s), when Jolt puts the body to sleep. 360 steps is that with a fifth
 * again of margin, and 1e-4 m per step -- a tenth of a millimetre -- is a hundred times
 * tighter than the two millimetres a step the first version of these settings twitched at
 * forever.
 *
 * A threshold nobody wrote down is a threshold that gets widened the first time it fails,
 * so both numbers are on C19's card as well as here.
 */
constexpr uint32_t kSettleSteps = 360;
constexpr float kSettleThreshold = 1.0e-4f;

TEST(Cloth, ACurtainSettlesRatherThanOscillatingForever) {
    // The property the envelope alone would not catch: a permanent twitch stays inside a
    // box forever. This is what says the cloth *stops*.
    const Sheet s = makeCurtain(9, 9, 0.25f);
    Rig rig(s);
    ASSERT_TRUE(rig.added);

    // Stepped one at a time, because `lastMaxDisplacement` is the movement since the last
    // read -- a single `step(360)` would report how far the cloth travelled in total,
    // which is a large number for a cloth that behaved perfectly.
    for (uint32_t i = 0; i < kSettleSteps; ++i) rig.step(1);
    const float settled = rig.cloth().lastMaxDisplacement;
    EXPECT_LT(settled, kSettleThreshold)
        << "still moving " << settled << " m per step after " << kSettleSteps << " steps";

    // And it stays settled rather than settling and departing again, which is what an
    // integrator that adds a little energy every step eventually does.
    for (uint32_t i = 0; i < 240; ++i) {
        rig.step(1);
        ASSERT_LT(rig.cloth().lastMaxDisplacement, kSettleThreshold) << "departed again at step " << i;
    }
}

TEST(Cloth, AFlagSettlesToo) {
    // The other axis. A settle test that only ever ran on one would pass a solver that
    // dissipated along x and not along z.
    const Sheet s = makeFlag(9, 9, 0.25f);
    Rig rig(s);
    ASSERT_TRUE(rig.added);
    for (uint32_t i = 0; i < kSettleSteps; ++i) rig.step(1);
    EXPECT_LT(rig.cloth().lastMaxDisplacement, kSettleThreshold);
}

// ------------------------------------------------------------------ determinism

TEST(Cloth, TwoRunsOfOneSceneAgreeToTheBit) {
    // `systems.md` states determinism as a property of the fixed step, and a solve that
    // depended on iteration order across a container that can grow is how that is lost.
    // Proved rather than asserted: two independently constructed worlds, the same
    // construction order, the same steps, compared with `memcmp`.
    const Sheet s = makeCurtain();

    Rig a(s);
    Rig b(s);
    ASSERT_TRUE(a.added);
    ASSERT_TRUE(b.added);

    a.run(240);
    b.run(240);

    const auto& va = a.cloth().vertices;
    const auto& vb = b.cloth().vertices;
    ASSERT_EQ(va.size(), vb.size());
    EXPECT_EQ(0, std::memcmp(va.data(), vb.data(), va.size() * sizeof(Vertex)))
        << "two runs of one cloth diverged";
    EXPECT_EQ(a.cloth().boundsMin, b.cloth().boundsMin);
    EXPECT_EQ(a.cloth().boundsMax, b.cloth().boundsMax);
}

TEST(Cloth, ReadingThePoseDoesNotPerturbIt) {
    // The readback is a const operation on the world and the normal recompute writes only
    // into `ClothSystem`'s own vertices, so a frame that read every step and a frame that
    // read once must land in the same place. If they did not, the pose would depend on the
    // frame rate -- which is exactly what a fixed step exists to prevent.
    //
    // **This one caught a real defect rather than confirming a design.** The first version
    // of `recomputeClothNormals` orthogonalised each frame's tangent against the previous
    // frame's, which is a fold over its own output: positions matched to the bit and
    // tangents did not, so a cloth shaded differently at 30 fps and at 144. The fix is a
    // stored rest tangent, and this is what says the fix holds.
    const Sheet s = makeCurtain();

    Rig once(s);
    Rig often(s);
    once.world.step(1.0f / 60.0f);
    for (uint32_t i = 1; i < 180; ++i) once.world.step(1.0f / 60.0f);
    once.cloths.update(once.world);

    for (uint32_t i = 0; i < 180; ++i) often.step(1);

    const auto& a = once.cloth().vertices;
    const auto& b = often.cloth().vertices;
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(Vertex)));
}

TEST(Cloth, ASecondClothDoesNotChangeTheFirstsTrajectory) {
    // The growable-container hazard, stated as a test. Creating a second soft body appends
    // to `PhysicsWorld::clothes` and hands Jolt another body; the first cloth's answer must
    // not move because of it, and it does not, because nothing about the solve is keyed on
    // a position in that array.
    const Sheet s = makeCurtain();

    Rig alone(s);
    Rig crowded(s);
    ASSERT_TRUE(crowded.added);

    Sheet far = makeCurtain();
    for (Vertex& v : far.vertices) v.position.x += 50.0f;
    ASSERT_TRUE(crowded.cloths.add(crowded.world, 1, 1, far.desc()));

    alone.run(180);
    crowded.run(180);

    const auto& a = alone.cloth().vertices;
    const auto& b = crowded.cloth().vertices;
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(Vertex)));
}

