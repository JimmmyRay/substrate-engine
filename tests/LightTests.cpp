#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include "gfx/Decal.h"
#include "gfx/Light.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using namespace gfx;

/**
 * @file tests/LightTests.cpp
 * @brief The importance metric both light budgets rank by (0.9, 0.10).
 *
 * This is the function that decides which lights a scene over budget keeps, and the
 * defect it replaced -- `if (lightScratch.size() >= kMaxLights) break;` -- was invisible
 * precisely because its answer was "whichever came first in vector order", which is a
 * plausible-looking image every time. A wrong ranking is the same kind of invisible, so
 * it gets asserted rather than eyeballed.
 */

namespace {

constexpr glm::vec3 kWhite{1.0f, 1.0f, 1.0f};
constexpr glm::vec3 kOrigin{0.0f, 0.0f, 0.0f};

/// The selection half of 0.9, in the form the renderer applies it: rank, keep the top
/// `room`, then restore scene order. Mirrored here rather than reached into because the
/// renderer's copy is four lines inside a function that needs a VkDevice -- and the
/// property worth pinning is *which* lights survive, which is this.
std::vector<size_t> keptIndices(const std::vector<GpuLight>& lights, size_t room, const glm::vec3& viewPos) {
    std::vector<size_t> order(lights.size());
    std::iota(order.begin(), order.end(), size_t{0});
    if (lights.size() <= room) return order;

    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return lightImportance(lights[a], viewPos) > lightImportance(lights[b], viewPos);
    });
    order.resize(room);
    std::sort(order.begin(), order.end());
    return order;
}

} // namespace

// ================================================================= the metric

TEST(LightImportance, ADirectionalLightOutranksEveryPunctualOne) {
    // A sun has no position for a distance to be measured from and lights the whole
    // scene rather than a neighbourhood. It is never what a budget should drop -- and
    // the cascades are fitted to it specifically, so dropping it would break more than
    // its own contribution.
    const GpuLight sun = makeDirectionalLight(glm::vec3(0.0f, 1.0f, 0.0f), kWhite, 1.0f);
    const GpuLight blinding = makePointLight(kOrigin, 100.0f, kWhite, 1.0e9f);

    EXPECT_TRUE(std::isinf(lightImportance(sun, kOrigin)));
    EXPECT_GT(lightImportance(sun, kOrigin), lightImportance(blinding, kOrigin));
}

TEST(LightImportance, FallsOffWithTheSquareOfDistance) {
    const GpuLight near = makePointLight(glm::vec3(0.0f, 0.0f, 10.0f), 100.0f, kWhite, 100.0f);
    const GpuLight far = makePointLight(glm::vec3(0.0f, 0.0f, 20.0f), 100.0f, kWhite, 100.0f);

    // Twice as far is a quarter as important, which is what "over squared distance"
    // has to mean for the ranking to be an irradiance comparison at all.
    EXPECT_NEAR(lightImportance(near, kOrigin) / lightImportance(far, kOrigin), 4.0f, 1e-3f);
}

TEST(LightImportance, ScalesLinearlyWithIntensity) {
    const glm::vec3 at(0.0f, 0.0f, 5.0f);
    const GpuLight dim = makePointLight(at, 100.0f, kWhite, 1.0f);
    const GpuLight bright = makePointLight(at, 100.0f, kWhite, 20.0f);

    EXPECT_NEAR(lightImportance(bright, kOrigin) / lightImportance(dim, kOrigin), 20.0f, 1e-3f);
}

TEST(LightImportance, WeightsColourByLumaRatherThanTreatingChannelsAlike) {
    // Equal intensity, different hue. Ranking on intensity alone would call these
    // equally worth a slot, and they are not: green carries most of the perceived
    // brightness and blue almost none.
    const glm::vec3 at(0.0f, 0.0f, 5.0f);
    const GpuLight green = makePointLight(at, 100.0f, glm::vec3(0.0f, 1.0f, 0.0f), 10.0f);
    const GpuLight blue = makePointLight(at, 100.0f, glm::vec3(0.0f, 0.0f, 1.0f), 10.0f);

    EXPECT_GT(lightImportance(green, kOrigin), lightImportance(blue, kOrigin));
    EXPECT_NEAR(lightImportance(green, kOrigin) / lightImportance(blue, kOrigin), 0.7152f / 0.0722f, 1e-2f);
}

TEST(LightImportance, ALightAtTheCameraDoesNotOutrankTheSun) {
    // The distance floor is a metre rather than an epsilon. Walking through a lamp must
    // not evict the rest of the scene's lighting for a frame, and with a tiny epsilon
    // it would -- by ten orders of magnitude.
    const GpuLight sun = makeDirectionalLight(glm::vec3(0.0f, 1.0f, 0.0f), kWhite, 3.0f);
    const GpuLight inside = makePointLight(kOrigin, 10.0f, kWhite, 5.0f);

    EXPECT_TRUE(std::isfinite(lightImportance(inside, kOrigin)));
    EXPECT_GT(lightImportance(sun, kOrigin), lightImportance(inside, kOrigin));

    // And inside the floor, distance stops mattering rather than exploding.
    const GpuLight halfMetre = makePointLight(glm::vec3(0.5f, 0.0f, 0.0f), 10.0f, kWhite, 5.0f);
    EXPECT_FLOAT_EQ(lightImportance(inside, kOrigin), lightImportance(halfMetre, kOrigin));
}

TEST(LightImportance, SpotsAreRankedLikePointsRatherThanByWhereTheyAim) {
    // Stated in the header as a deliberate limitation, so it is pinned here: a spot
    // aimed away from the camera is still lighting whatever it points at, and that may
    // be most of what is on screen.
    const glm::vec3 at(0.0f, 0.0f, 5.0f);
    const GpuLight toward = makeSpotLight(at, glm::vec3(0.0f, 0.0f, -1.0f), 20.0f, 0.3f, 0.5f, kWhite, 10.0f);
    const GpuLight away = makeSpotLight(at, glm::vec3(0.0f, 0.0f, 1.0f), 20.0f, 0.3f, 0.5f, kWhite, 10.0f);

    EXPECT_FLOAT_EQ(lightImportance(toward, kOrigin), lightImportance(away, kOrigin));
}

// ============================================================== the selection

TEST(LightBudget, UnderBudgetKeepsEveryLightInSceneOrder) {
    // Order is not incidental. The shader accumulates radiance in buffer order and
    // floating-point addition is not associative, so reordering a set nobody is
    // dropping from would move pixels -- which the golden images would report as a
    // regression, correctly.
    std::vector<GpuLight> lights;
    for (int i = 0; i < 5; ++i) {
        lights.push_back(makePointLight(glm::vec3(static_cast<float>(i) * 3.0f, 0.0f, 0.0f), 20.0f, kWhite,
                                        static_cast<float>(5 - i)));
    }

    const std::vector<size_t> kept = keptIndices(lights, 8, kOrigin);
    ASSERT_EQ(kept.size(), 5u);
    for (size_t i = 0; i < kept.size(); ++i) EXPECT_EQ(kept[i], i);
}

TEST(LightBudget, OverBudgetKeepsTheBrightNearOnesAndDropsTheDimFarOnes) {
    // The shape of game/demo/assets/stress.gltf: a *ring* of alternating bright-warm and
    // dim-cold lights, all the same distance from the centre. Equal distance on purpose
    // -- it leaves brightness as the only discriminator, so the parity assertion below
    // means what it says. On a line it would not: a dim light a metre away legitimately
    // outranks a bright one ten metres away, and asserting otherwise would be asserting
    // that the metric is not luminance over squared distance.
    std::vector<GpuLight> lights;
    for (int i = 0; i < 20; ++i) {
        const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / 20.0f;
        const bool bright = (i % 2) == 0;
        lights.push_back(makePointLight(glm::vec3(5.0f * std::cos(angle), 2.0f, 5.0f * std::sin(angle)), 12.0f,
                                        kWhite, bright ? 60.0f : 3.0f));
    }

    // A budget that kept the first N would keep half of each, which is exactly why the
    // test scene alternates rather than being uniform.
    const std::vector<size_t> kept = keptIndices(lights, 10, kOrigin);
    ASSERT_EQ(kept.size(), 10u);
    for (size_t index : kept) {
        EXPECT_EQ(index % 2, 0u) << "slot " << index << " is a dim light that outranked a bright one";
    }
}

TEST(LightBudget, ANearDimLightOutranksAFarBrightOne) {
    // The converse of the ring, and the reason the ring has to be a ring. This is the
    // metric behaving correctly, not a wrinkle in it: irradiance is what decides, and a
    // 3-intensity lamp at one metre delivers more of it than a 60-intensity lamp at ten.
    std::vector<GpuLight> lights;
    lights.push_back(makePointLight(glm::vec3(10.0f, 0.0f, 0.0f), 50.0f, kWhite, 60.0f)); // far, bright
    lights.push_back(makePointLight(glm::vec3(1.0f, 0.0f, 0.0f), 50.0f, kWhite, 3.0f));   // near, dim

    EXPECT_EQ(keptIndices(lights, 1, kOrigin), (std::vector<size_t>{1}));
}

TEST(LightBudget, SurvivorsComeBackInSceneOrderNotImportanceOrder) {
    // Importance decides *which* lights, never in what order they are summed.
    std::vector<GpuLight> lights;
    lights.push_back(makePointLight(glm::vec3(0.0f, 0.0f, 30.0f), 50.0f, kWhite, 1.0f));  // 0: dim, far
    lights.push_back(makePointLight(glm::vec3(0.0f, 0.0f, 2.0f), 50.0f, kWhite, 90.0f));  // 1: brightest
    lights.push_back(makePointLight(glm::vec3(0.0f, 0.0f, 40.0f), 50.0f, kWhite, 0.5f));  // 2: dimmest
    lights.push_back(makePointLight(glm::vec3(0.0f, 0.0f, 4.0f), 50.0f, kWhite, 40.0f));  // 3: second

    const std::vector<size_t> kept = keptIndices(lights, 2, kOrigin);
    ASSERT_EQ(kept.size(), 2u);
    EXPECT_EQ(kept[0], 1u);
    EXPECT_EQ(kept[1], 3u) << "the two brightest, emitted ascending rather than brightest-first";
}

TEST(LightBudget, TiesKeepSceneOrderSoTheFrameStaysBitIdentical) {
    // stable_sort, not sort. Equal-importance lights must resolve the same way on every
    // run or 5.3's golden images stop meaning anything -- and a scene of identical
    // fixtures on a grid is the common case, not a contrived one.
    std::vector<GpuLight> lights;
    for (int i = 0; i < 6; ++i) {
        lights.push_back(makePointLight(glm::vec3(0.0f, 0.0f, 10.0f), 20.0f, kWhite, 7.0f));
    }

    const std::vector<size_t> first = keptIndices(lights, 3, kOrigin);
    for (int repeat = 0; repeat < 8; ++repeat) {
        EXPECT_EQ(keptIndices(lights, 3, kOrigin), first);
    }
    EXPECT_EQ(first, (std::vector<size_t>{0, 1, 2}));
}

TEST(LightBudget, MovingTheCameraChangesWhichLightsSurvive) {
    // The budget is a *view-dependent* policy, which is the point of ranking by
    // distance at all. A fixed ranking would be a prettier version of vector order.
    std::vector<GpuLight> lights;
    lights.push_back(makePointLight(glm::vec3(-50.0f, 0.0f, 0.0f), 100.0f, kWhite, 10.0f));
    lights.push_back(makePointLight(glm::vec3(50.0f, 0.0f, 0.0f), 100.0f, kWhite, 10.0f));

    EXPECT_EQ(keptIndices(lights, 1, glm::vec3(-45.0f, 0.0f, 0.0f)), (std::vector<size_t>{0}));
    EXPECT_EQ(keptIndices(lights, 1, glm::vec3(45.0f, 0.0f, 0.0f)), (std::vector<size_t>{1}));
}

// The `LightOverrides` suite stood here -- four tests over
// `parseSceneLightOverrides` and the `castsShadows` boolean it read out of
// `nodes[i].extras.substrate_light`. It went with the shadow system along with the thing
// it tested; see `gfx::Light.h` for what the schema was for and why it comes back when
// shadows do.

// ============================================== runtime decals
//
// `decalAt` is the other half of "spawn something where the hit was". It lives in
// gfx/Decal.h rather than gfx/Renderer.h precisely so it can be tested here: everything
// in Renderer.h reaches VkDevice, and nothing that does can be in the hosted suite.

TEST(DecalPlacement, SitsAtThePointAndFacesAlongTheNormal) {
    const glm::vec3 point{3.0f, 1.0f, -2.0f};
    const glm::vec3 normal{0.0f, 0.0f, 1.0f};
    const gfx::Decal d = gfx::decalAt(point, normal, 0.5f);

    EXPECT_EQ(glm::vec3(d.transform[3]), point);
    // A decal projects down its local Y, so local +Y is the surface normal.
    EXPECT_NEAR(glm::dot(glm::normalize(glm::vec3(d.transform[1])), normal), 1.0f, 1e-5f);
}

TEST(DecalPlacement, IsSquareAndSizedByItsArgument) {
    const gfx::Decal d = gfx::decalAt({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 2.0f);
    EXPECT_NEAR(glm::length(glm::vec3(d.transform[0])), 2.0f, 1e-5f);
    EXPECT_NEAR(glm::length(glm::vec3(d.transform[1])), 2.0f, 1e-5f);
    // The projection depth matches the footprint, which is what stops a decal on a thin
    // wall bleeding through to the far side.
    EXPECT_NEAR(glm::length(glm::vec3(d.transform[2])), 2.0f, 1e-5f);
}

TEST(DecalPlacement, TheBasisIsOrthonormalForEveryNormalIncludingStraightUp) {
    // The seed axis is swapped near the poles; without that the cross product degenerates
    // and the basis collapses, which is a decal that renders as nothing.
    for (const glm::vec3& n : {glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
                               glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.3f, 0.9f, -0.2f)}) {
        const gfx::Decal d = gfx::decalAt({0.0f, 0.0f, 0.0f}, n, 1.0f);
        const glm::vec3 x = glm::normalize(glm::vec3(d.transform[0]));
        const glm::vec3 y = glm::normalize(glm::vec3(d.transform[1]));
        const glm::vec3 z = glm::normalize(glm::vec3(d.transform[2]));
        EXPECT_NEAR(glm::dot(x, y), 0.0f, 1e-5f) << "normal " << n.x << "," << n.y << "," << n.z;
        EXPECT_NEAR(glm::dot(y, z), 0.0f, 1e-5f);
        EXPECT_NEAR(glm::dot(x, z), 0.0f, 1e-5f);
        EXPECT_NEAR(glm::dot(y, glm::normalize(n)), 1.0f, 1e-5f);
    }
}

TEST(DecalPlacement, AZeroNormalLeavesItUnrotatedRatherThanDegenerate) {
    const gfx::Decal d = gfx::decalAt({1.0f, 2.0f, 3.0f}, glm::vec3(0.0f), 1.0f);
    EXPECT_EQ(glm::vec3(d.transform[3]), glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_NEAR(glm::length(glm::vec3(d.transform[1])), 1.0f, 1e-5f);
}

// ============================================ light volume culling
//
// The property everything else rests on: `lightVisible` is conservative and exact, so
// culling by it cannot change a shaded pixel. If any of these are wrong, the golden set
// moves -- which is the check, but only after the fact. These are the check before it.

namespace {

/// A camera at the origin looking down -Z, with a 90-degree field of view.
glm::mat4 testViewProj() {
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::half_pi<float>(), 1.0f, 0.1f, 100.0f);
    return proj * view;
}

} // namespace

TEST(LightCulling, ADirectionalLightIsNeverCulled) {
    const gfx::Frustum f = gfx::extractFrustum(testViewProj());
    // Pointing away from the camera, behind it, at any angle: a sun lights everything.
    EXPECT_TRUE(gfx::lightVisible(gfx::makeDirectionalLight({0.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f), f));
    EXPECT_TRUE(gfx::lightVisible(gfx::makeDirectionalLight({0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f), f));
}

TEST(LightCulling, AnUnboundedPointLightIsNeverCulled) {
    // Range 0 means unbounded by GpuLight's own convention. Culling one would delete
    // lights from any scene that authored no range.
    const gfx::Frustum f = gfx::extractFrustum(testViewProj());
    const gfx::GpuLight far = gfx::makePointLight({0.0f, 0.0f, 5000.0f}, 0.0f, {1.0f, 1.0f, 1.0f}, 1.0f);
    EXPECT_TRUE(gfx::lightVisible(far, f)) << "range 0 is unbounded, not empty";
}

TEST(LightCulling, ALightInFrontOfTheCameraSurvives) {
    const gfx::Frustum f = gfx::extractFrustum(testViewProj());
    EXPECT_TRUE(gfx::lightVisible(gfx::makePointLight({0.0f, 0.0f, -10.0f}, 5.0f, {1.0f, 1.0f, 1.0f}, 1.0f), f));
}

TEST(LightCulling, ALightWellBehindTheCameraIsCulled) {
    const gfx::Frustum f = gfx::extractFrustum(testViewProj());
    // 50 metres behind, reaching 2. It cannot touch anything the camera can see.
    EXPECT_FALSE(gfx::lightVisible(gfx::makePointLight({0.0f, 0.0f, 50.0f}, 2.0f, {1.0f, 1.0f, 1.0f}, 1.0f), f));
}

TEST(LightCulling, ALightBehindTheCameraWhoseRangeReachesIntoViewSurvives) {
    // The case that makes the test conservative rather than merely cheap: the light's
    // centre is outside the frustum, but its volume is not.
    const gfx::Frustum f = gfx::extractFrustum(testViewProj());
    EXPECT_TRUE(gfx::lightVisible(gfx::makePointLight({0.0f, 0.0f, 3.0f}, 20.0f, {1.0f, 1.0f, 1.0f}, 1.0f), f));
}

TEST(LightCulling, ALightFarOffToTheSideIsCulled) {
    const gfx::Frustum f = gfx::extractFrustum(testViewProj());
    EXPECT_FALSE(gfx::lightVisible(gfx::makePointLight({500.0f, 0.0f, -10.0f}, 3.0f, {1.0f, 1.0f, 1.0f}, 1.0f), f));
}

TEST(LightCulling, ASpotIsCulledByItsSphereRatherThanItsCone) {
    // A spot far behind the camera goes, whichever way it is aimed -- its range is what
    // bounds it. Aim is deliberately not tested: the sphere bounds the cone, so using it
    // keeps the test conservative, and that is the documented choice.
    const gfx::Frustum f = gfx::extractFrustum(testViewProj());
    const gfx::GpuLight away = gfx::makeSpotLight({0.0f, 0.0f, 80.0f}, {0.0f, 0.0f, -1.0f}, 4.0f, 0.2f, 0.4f,
                                                  {1.0f, 1.0f, 1.0f}, 1.0f);
    EXPECT_FALSE(gfx::lightVisible(away, f));

    const gfx::GpuLight near = gfx::makeSpotLight({0.0f, 0.0f, -10.0f}, {0.0f, 0.0f, 1.0f}, 4.0f, 0.2f, 0.4f,
                                                  {1.0f, 1.0f, 1.0f}, 1.0f);
    EXPECT_TRUE(gfx::lightVisible(near, f)) << "aimed away, but it is lighting what is in front of the camera";
}

TEST(LightCulling, TheFrustumPlanesAreNormalisedAndInwardFacing) {
    const gfx::Frustum f = gfx::extractFrustum(testViewProj());
    for (const glm::vec4& p : f.planes) {
        EXPECT_NEAR(glm::length(glm::vec3(p)), 1.0f, 1e-4f);
    }
    // A point plainly inside is in front of every plane.
    const glm::vec3 inside{0.0f, 0.0f, -10.0f};
    for (const glm::vec4& p : f.planes) {
        EXPECT_GT(glm::dot(glm::vec3(p), inside) + p.w, 0.0f);
    }
}
