#include "scene/Animation.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace core;

using namespace scene;

/**
 * @file tests/AnimationTests.cpp
 * @brief The clip sampler and joint hierarchy from 4.4, and all of S2 (5.1).
 *
 * Pure CPU, deterministic, and invisible in a screenshot -- a rig that interpolates
 * along the chord instead of the arc looks like a rig, just a slightly wrong one. That
 * combination is exactly what the roadmap points at when it says the clip sampler is
 * what 5.1's isolated tests are for, and it applies twice over to S2: a cross-fade that
 * takes the long way round a quaternion and a state machine that re-fires a consumed
 * trigger both produce animation that looks *odd* rather than broken.
 *
 * Determinism is still load-bearing for 5.3, and S2 changed how it is reached rather
 * than whether it holds: `update(dt)` accumulates where 4.4's `update(t)` multiplied,
 * so the tests below check that N identical steps land in the same place every time.
 */

namespace {

constexpr float kEps = 1e-5f;

void expectVecNear(const glm::vec3& a, const glm::vec3& b, float eps = kEps) {
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

void expectMatEq(const glm::mat4& a, const glm::mat4& b, float eps = kEps) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) EXPECT_NEAR(a[c][r], b[c][r], eps) << "column " << c << " row " << r;
    }
}

/// A one-channel clip over one node, so a test says only what it is about.
AnimationClip translationClip(std::vector<float> times, std::vector<glm::vec4> values,
                              AnimationInterpolation interp = AnimationInterpolation::Linear) {
    AnimationClip clip;
    clip.name = "test";
    clip.duration = times.empty() ? 0.0f : times.back();
    clip.samplers.push_back(AnimationSampler{std::move(times), std::move(values), interp});
    clip.channels.push_back(AnimationChannel{0, AnimationPath::Translation, 0});
    return clip;
}

AnimationClip rotationClip(std::vector<float> times, std::vector<glm::vec4> values,
                           AnimationInterpolation interp = AnimationInterpolation::Linear) {
    AnimationClip clip;
    clip.name = "test";
    clip.duration = times.empty() ? 0.0f : times.back();
    clip.samplers.push_back(AnimationSampler{std::move(times), std::move(values), interp});
    clip.channels.push_back(AnimationChannel{0, AnimationPath::Rotation, 0});
    return clip;
}

/// glTF stores a quaternion xyzw; glm's constructor takes wxyz. Getting that backwards
/// is silent and produces a rig that is merely rotated oddly, so it is spelled out here
/// once rather than inline at every call.
glm::vec4 quatXyzw(const glm::quat& q) { return glm::vec4(q.x, q.y, q.z, q.w); }

Pose posed(size_t nodes, size_t weights = 0) {
    Pose p;
    p.nodes.resize(nodes);
    p.weights.assign(weights, 0.0f);
    return p;
}

AnimationRig rigOf(std::vector<SceneNode> nodes, std::vector<Skin> skins, std::vector<AnimationClip> clips,
                   std::vector<float> weights = {}) {
    AnimationRig r;
    r.bind.nodes = std::move(nodes);
    r.bind.weights = std::move(weights);
    r.skins = std::move(skins);
    r.clips = std::move(clips);
    return r;
}

} // namespace

// ============================================================ localTransform

TEST(LocalTransform, AppliesTranslateRotateScaleInTheOrderTheSpecFixes) {
    SceneNode n;
    n.translation = glm::vec3(1.0f, 2.0f, 3.0f);
    n.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    n.scale = glm::vec3(2.0f);

    // A point on +X, scaled to (2,0,0), rotated 90 degrees about Y to (0,0,-2), then
    // translated. Any other order lands somewhere else.
    const glm::vec3 p = glm::vec3(localTransform(n) * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    expectVecNear(p, glm::vec3(1.0f, 2.0f, 1.0f));
}

TEST(LocalTransform, IdentityNodeIsTheIdentityMatrix) {
    expectMatEq(localTransform(SceneNode{}), glm::mat4(1.0f));
}

// =============================================================== sampleClip

TEST(SampleClip, LinearInterpolatesBetweenTheBracketingKeys) {
    const AnimationClip clip = translationClip({0.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(10.0f, 20.0f, 30.0f, 0.0f)});
    Pose pose = posed(1);

    sampleClip(clip, 0.5f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(2.5f, 5.0f, 7.5f));

    sampleClip(clip, 1.5f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(7.5f, 15.0f, 22.5f));
}

TEST(SampleClip, StepHoldsTheEarlierKeyUntilTheNextOne) {
    const AnimationClip clip =
        translationClip({0.0f, 1.0f, 2.0f},
                        {glm::vec4(0.0f), glm::vec4(5.0f, 0.0f, 0.0f, 0.0f), glm::vec4(9.0f, 0.0f, 0.0f, 0.0f)},
                        AnimationInterpolation::Step);
    Pose pose = posed(1);

    sampleClip(clip, 0.9f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(0.0f));

    sampleClip(clip, 1.0f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(5.0f, 0.0f, 0.0f));

    sampleClip(clip, 1.99f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(5.0f, 0.0f, 0.0f));
}

TEST(SampleClip, HoldsTheEndpointsRatherThanExtrapolating) {
    // A clip whose channels start at different times is legal glTF, and extrapolating
    // the earlier ones is how a rig ends up inside out for exactly one frame.
    AnimationClip clip =
        translationClip({1.0f, 2.0f}, {glm::vec4(4.0f, 0.0f, 0.0f, 0.0f), glm::vec4(8.0f, 0.0f, 0.0f, 0.0f)});
    // Longer than the channel's last key, which is what leaves room either side of it
    // for the endpoint hold to be distinguishable from the loop wrapping around.
    clip.duration = 3.0f;
    Pose pose = posed(1);

    sampleClip(clip, 0.5f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(4.0f, 0.0f, 0.0f));

    sampleClip(clip, 2.5f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(8.0f, 0.0f, 0.0f));
}

TEST(SampleClip, WrapsTimeIntoTheClipRatherThanFreezingOnTheLastKey) {
    const AnimationClip clip = translationClip({0.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(10.0f, 0.0f, 0.0f, 0.0f)});
    Pose pose = posed(1);

    sampleClip(clip, 0.5f, pose);
    const glm::vec3 early = pose.nodes[0].translation;

    // Three full loops later, plus the same offset.
    sampleClip(clip, 6.5f, pose);
    expectVecNear(pose.nodes[0].translation, early);
}

TEST(SampleClip, ExactlyTheDurationIsTheEndOfTheClipAndNotTheStartOfIt) {
    // `fmod(d, d)` is 0, so this used to answer with the *first* key -- and a ClampToEnd
    // playback sits on exactly the duration for every frame it holds its last pose. One
    // frame of the first pose in the middle of a hold is the snap at the end of a jump.
    const AnimationClip clip = translationClip({0.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(10.0f, 0.0f, 0.0f, 0.0f)});
    Pose pose = posed(1);

    sampleClip(clip, 2.0f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(10.0f, 0.0f, 0.0f));

    // And a whole loop past it still wraps, which is the behaviour the fix must not cost.
    sampleClip(clip, 4.5f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(2.5f, 0.0f, 0.0f));
}

TEST(SampleClip, NegativeTimeIsClampedToZeroBeforeWrapping) {
    const AnimationClip clip = translationClip({0.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(10.0f, 0.0f, 0.0f, 0.0f)});
    Pose pose = posed(1);

    sampleClip(clip, -5.0f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(0.0f));
}

TEST(SampleClip, ZeroDurationSamplesAtZeroInsteadOfDividingByIt) {
    AnimationClip clip = translationClip({0.0f}, {glm::vec4(3.0f, 0.0f, 0.0f, 0.0f)});
    clip.duration = 0.0f;
    Pose pose = posed(1);

    sampleClip(clip, 12.0f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(3.0f, 0.0f, 0.0f));
}

TEST(SampleClip, LeavesNodesNoChannelTargetsUntouched) {
    AnimationClip clip = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    Pose pose = posed(2);
    pose.nodes[1].translation = glm::vec3(7.0f, 8.0f, 9.0f);

    sampleClip(clip, 0.5f, pose);
    expectVecNear(pose.nodes[1].translation, glm::vec3(7.0f, 8.0f, 9.0f));
}

TEST(SampleClip, OutOfRangeChannelTargetsAreSkippedRatherThanIndexingPastTheEnd) {
    AnimationClip clip = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    clip.channels[0].node = 99;
    clip.channels.push_back(AnimationChannel{0, AnimationPath::Scale, 42}); // sampler out of range too

    Pose pose = posed(1);
    sampleClip(clip, 0.5f, pose); // must not read out of bounds
    expectVecNear(pose.nodes[0].scale, glm::vec3(1.0f));
}

// ================================================================= rotation

TEST(SampleClip, RotationUsesSlerpRatherThanALerp) {
    // At the halfway point nlerp and slerp agree, so a quarter of the way through is
    // where the difference shows: slerp gives exactly 22.5 degrees, a normalised lerp
    // gives about 22.9. That difference is a joint speeding up through the middle of
    // every rotation and slowing at the ends, which reads as bad animation rather than
    // as a bug.
    const glm::quat from(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat to = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    const AnimationClip clip = rotationClip({0.0f, 1.0f}, {quatXyzw(from), quatXyzw(to)});
    Pose pose = posed(1);
    sampleClip(clip, 0.25f, pose);

    EXPECT_NEAR(glm::degrees(glm::angle(glm::normalize(pose.nodes[0].rotation))), 22.5f, 0.01f);
    EXPECT_NEAR(glm::length(pose.nodes[0].rotation), 1.0f, kEps);
}

TEST(SampleClip, CubicSplineReturnsTheKeyItselfAtAKeyframe) {
    // CUBICSPLINE stores in-tangent, point, out-tangent per key. Reading the wrong one
    // of the three is the classic mistake, and it is invisible except exactly here.
    AnimationClip clip;
    clip.duration = 3.0f; // past the last key, so t=2 is a hold rather than a wrap to 0

    AnimationSampler s;
    s.interpolation = AnimationInterpolation::CubicSpline;
    s.times = {0.0f, 1.0f, 2.0f};
    s.values = {
        glm::vec4(-99.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f),  // key 0
        glm::vec4(0.0f),   glm::vec4(5.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f),  // key 1
        glm::vec4(0.0f),   glm::vec4(9.0f, 0.0f, 0.0f, 0.0f), glm::vec4(-99.0f) // key 2
    };
    clip.samplers.push_back(std::move(s));
    clip.channels.push_back(AnimationChannel{0, AnimationPath::Translation, 0});

    Pose pose = posed(1);

    sampleClip(clip, 1.0f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(5.0f, 0.0f, 0.0f));

    // And at the ends, where findKey takes the single-key path and the stride offset is
    // the only thing keeping it off a tangent.
    sampleClip(clip, 0.0f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(0.0f));
    sampleClip(clip, 2.0f, pose);
    expectVecNear(pose.nodes[0].translation, glm::vec3(9.0f, 0.0f, 0.0f));
}

TEST(SampleClip, CubicSplineRotationsComeBackNormalised) {
    // A Hermite interpolation of four quaternion components leaves the result off the
    // unit sphere, and a non-unit quaternion in a joint matrix is a scale nobody
    // authored -- a limb that swells through the middle of a swing.
    const glm::quat from(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat to = glm::angleAxis(glm::radians(120.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    AnimationClip clip;
    clip.duration = 1.0f;
    AnimationSampler s;
    s.interpolation = AnimationInterpolation::CubicSpline;
    s.times = {0.0f, 1.0f};
    s.values = {glm::vec4(2.0f), quatXyzw(from), glm::vec4(2.0f), glm::vec4(2.0f), quatXyzw(to), glm::vec4(2.0f)};
    clip.samplers.push_back(std::move(s));
    clip.channels.push_back(AnimationChannel{0, AnimationPath::Rotation, 0});

    Pose pose = posed(1);
    for (float t : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f}) {
        sampleClip(clip, t, pose);
        EXPECT_NEAR(glm::length(pose.nodes[0].rotation), 1.0f, 1e-4f) << "at t=" << t;
    }
}

// ================================================= morph weight channels (S2.1)

namespace {

/// A weights channel over node 0, with `targets` weights per keyframe.
AnimationClip weightsClip(std::vector<float> times, std::vector<float> weights, uint32_t targets,
                          AnimationInterpolation interp = AnimationInterpolation::Linear) {
    AnimationClip clip;
    clip.duration = times.empty() ? 0.0f : times.back();
    AnimationSampler s;
    s.times = std::move(times);
    s.interpolation = interp;
    s.weights = std::move(weights);
    s.stride = targets;
    clip.samplers.push_back(std::move(s));
    clip.channels.push_back(AnimationChannel{0, AnimationPath::Weights, 0});
    return clip;
}

Pose morphPose(uint32_t targets) {
    Pose p = posed(1, targets);
    p.nodes[0].firstWeight = 0;
    p.nodes[0].weightCount = targets;
    return p;
}

} // namespace

TEST(SampleWeights, LinearInterpolatesEveryTargetIndependently) {
    // Two targets, moving in opposite directions -- which is the case a stride bug
    // cannot survive, because reading the wrong lane swaps the two.
    const AnimationClip clip = weightsClip({0.0f, 1.0f}, {0.0f, 1.0f, 1.0f, 0.0f}, 2);
    Pose pose = morphPose(2);

    sampleClip(clip, 0.25f, pose);
    EXPECT_NEAR(pose.weights[0], 0.25f, kEps);
    EXPECT_NEAR(pose.weights[1], 0.75f, kEps);
}

TEST(SampleWeights, StepHoldsTheEarlierKey) {
    const AnimationClip clip =
        weightsClip({0.0f, 1.0f}, {0.2f, 0.8f, 0.9f, 0.1f}, 2, AnimationInterpolation::Step);
    Pose pose = morphPose(2);

    sampleClip(clip, 0.99f, pose);
    EXPECT_NEAR(pose.weights[0], 0.2f, kEps);
    EXPECT_NEAR(pose.weights[1], 0.8f, kEps);
}

TEST(SampleWeights, CubicSplineReadsThePointAndNotATangent) {
    // Same trap as the TRS path: in-tangent, point, out-tangent per key, per target.
    // The tangents here are absurd values, so reading one lands nowhere near 0.5.
    AnimationClip clip = weightsClip({0.0f, 1.0f},
                                     {-9.0f, -9.0f, 0.0f, 0.5f, 9.0f, 9.0f,  // key 0: in, point, out
                                      -9.0f, -9.0f, 1.0f, 0.5f, 9.0f, 9.0f}, // key 1
                                     2, AnimationInterpolation::CubicSpline);
    // Past the last key, so t=1 is a hold on it rather than a wrap back to zero.
    clip.duration = 2.0f;
    Pose pose = morphPose(2);

    sampleClip(clip, 0.0f, pose);
    EXPECT_NEAR(pose.weights[0], 0.0f, kEps);
    EXPECT_NEAR(pose.weights[1], 0.5f, kEps);

    sampleClip(clip, 1.0f, pose);
    EXPECT_NEAR(pose.weights[0], 1.0f, kEps);
    EXPECT_NEAR(pose.weights[1], 0.5f, kEps);
}

TEST(SampleWeights, AChannelClaimingMoreTargetsThanTheNodeHasWritesOnlyItsOwn) {
    // Malformed input: three weights per key, a node that declares two. Writing the
    // third would land in whatever node's weights come next, which is a character
    // whose face moves when somebody else blinks.
    const AnimationClip clip = weightsClip({0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, 3);

    Pose pose = posed(2, 4);
    pose.nodes[0].firstWeight = 0;
    pose.nodes[0].weightCount = 2;
    pose.nodes[1].firstWeight = 2;
    pose.nodes[1].weightCount = 2;

    sampleClip(clip, 0.5f, pose);
    EXPECT_NEAR(pose.weights[0], 1.0f, kEps);
    EXPECT_NEAR(pose.weights[1], 1.0f, kEps);
    EXPECT_NEAR(pose.weights[2], 0.0f, kEps) << "the next node's weights must be untouched";
    EXPECT_NEAR(pose.weights[3], 0.0f, kEps);
}

TEST(SampleWeights, ANodeWithNoMorphTargetsIsSkipped) {
    const AnimationClip clip = weightsClip({0.0f, 1.0f}, {1.0f, 1.0f}, 1);
    Pose pose = posed(1, 0); // weightCount stays 0
    sampleClip(clip, 0.5f, pose); // must not write anywhere
    EXPECT_TRUE(pose.weights.empty());
}

// ================================================================== blendPose

TEST(BlendPose, HalfwayIsTheMidpointOfEveryChannel) {
    Pose a = posed(1, 1);
    a.nodes[0].translation = glm::vec3(0.0f);
    a.nodes[0].scale = glm::vec3(1.0f);
    a.weights[0] = 0.0f;

    Pose b = posed(1, 1);
    b.nodes[0].translation = glm::vec3(4.0f, 0.0f, 0.0f);
    b.nodes[0].scale = glm::vec3(3.0f);
    b.nodes[0].rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    b.weights[0] = 1.0f;

    blendPose(a, b, 0.5f);

    expectVecNear(a.nodes[0].translation, glm::vec3(2.0f, 0.0f, 0.0f));
    expectVecNear(a.nodes[0].scale, glm::vec3(2.0f));
    EXPECT_NEAR(glm::degrees(glm::angle(a.nodes[0].rotation)), 45.0f, 0.01f);
    EXPECT_NEAR(a.weights[0], 0.5f, kEps);
}

TEST(BlendPose, ZeroLeavesTheDestinationAloneAndOneReplacesIt) {
    Pose a = posed(1);
    Pose b = posed(1);
    b.nodes[0].translation = glm::vec3(5.0f, 6.0f, 7.0f);

    Pose zero = a;
    blendPose(zero, b, 0.0f);
    expectVecNear(zero.nodes[0].translation, glm::vec3(0.0f));

    Pose one = a;
    blendPose(one, b, 1.0f);
    expectVecNear(one.nodes[0].translation, glm::vec3(5.0f, 6.0f, 7.0f));
}

TEST(BlendPose, ClampsRatherThanExtrapolating) {
    // A transition timer that runs past its duration must not turn the rig inside out.
    Pose a = posed(1);
    Pose b = posed(1);
    b.nodes[0].translation = glm::vec3(2.0f, 0.0f, 0.0f);

    blendPose(a, b, 4.0f);
    expectVecNear(a.nodes[0].translation, glm::vec3(2.0f, 0.0f, 0.0f));
}

TEST(BlendPose, TakesTheShortArcBetweenOppositeHemispheres) {
    // q and -q are the same rotation. A blend that ignores the sign sends the joint
    // 270 degrees the wrong way, which reads as an arm swinging through the torso.
    Pose a = posed(1);
    a.nodes[0].rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

    Pose b = posed(1);
    const glm::quat target = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    b.nodes[0].rotation = -target; // same rotation, opposite hemisphere

    blendPose(a, b, 0.5f);
    EXPECT_NEAR(glm::degrees(glm::angle(a.nodes[0].rotation)), 45.0f, 0.01f);
}

TEST(BlendPose, PosesOfDifferentLengthsBlendOverTheShorterOne) {
    Pose a = posed(3);
    Pose b = posed(1);
    b.nodes[0].translation = glm::vec3(1.0f, 0.0f, 0.0f);

    blendPose(a, b, 1.0f); // must not read past b
    expectVecNear(a.nodes[0].translation, glm::vec3(1.0f, 0.0f, 0.0f));
    expectVecNear(a.nodes[2].translation, glm::vec3(0.0f));
}

// ==================================================================== advance

TEST(Advance, LoopWrapsAndNeverReportsFinished) {
    const AnimationClip clip = translationClip({0.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    ClipPlayback p;
    p.loop = LoopMode::Loop;

    EXPECT_FALSE(advance(p, clip, 1.5f));
    EXPECT_NEAR(p.time, 1.5f, kEps);
    EXPECT_FALSE(advance(p, clip, 1.0f));
    EXPECT_NEAR(p.time, 0.5f, kEps);
}

TEST(Advance, ClampHoldsTheEndAndReportsFinished) {
    const AnimationClip clip = translationClip({0.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    ClipPlayback p;
    p.loop = LoopMode::ClampToEnd;

    EXPECT_FALSE(advance(p, clip, 1.0f));
    EXPECT_TRUE(advance(p, clip, 5.0f));
    EXPECT_NEAR(p.time, 2.0f, kEps);
}

TEST(Advance, SpeedScalesTheStepAndNegativeSpeedWrapsBackwards) {
    const AnimationClip clip = translationClip({0.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});

    ClipPlayback fast;
    fast.speed = 2.0f;
    advance(fast, clip, 0.5f);
    EXPECT_NEAR(fast.time, 1.0f, kEps);

    // fmod of a negative time is negative, so a clip played backwards walks off the
    // front unless one add brings it back.
    ClipPlayback back;
    back.speed = -1.0f;
    advance(back, clip, 0.5f);
    EXPECT_NEAR(back.time, 1.5f, kEps);
}

TEST(Advance, PausedDoesNotMoveButStillReportsWhereItIs) {
    const AnimationClip clip = translationClip({0.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    ClipPlayback p;
    p.loop = LoopMode::ClampToEnd;
    p.time = 2.0f;
    p.playing = false;

    EXPECT_TRUE(advance(p, clip, 1.0f));
    EXPECT_NEAR(p.time, 2.0f, kEps);
}

TEST(Advance, AZeroDurationClipDoesNotDivideByIt) {
    AnimationClip clip = translationClip({0.0f}, {glm::vec4(0.0f)});
    clip.duration = 0.0f;
    ClipPlayback p;
    advance(p, clip, 1.0f);
    EXPECT_NEAR(p.time, 0.0f, kEps);
}

// ============================================================ SceneAnimator

namespace {

/// Three nodes in a chain, deliberately declared child-before-parent so the world
/// transform pass has to sort them.
std::vector<SceneNode> outOfOrderChain() {
    std::vector<SceneNode> nodes(3);
    nodes[0].parent = 1; // grandchild
    nodes[0].translation = glm::vec3(0.0f, 1.0f, 0.0f);
    nodes[1].parent = 2; // child
    nodes[1].translation = glm::vec3(0.0f, 1.0f, 0.0f);
    nodes[2].parent = -1; // root
    nodes[2].translation = glm::vec3(5.0f, 0.0f, 0.0f);
    return nodes;
}

} // namespace

TEST(SceneAnimator, ResolvesParentsBeforeChildrenWhateverTheArrayOrder) {
    // glTF does not require the node array to be topologically ordered. A single pass
    // in array order would leave the root's translation missing from its descendants.
    SceneAnimator anim;
    anim.init(rigOf(outOfOrderChain(), {}, {}));

    const std::vector<glm::mat4>& world = anim.worldTransforms(anim.characterAt(0));
    ASSERT_EQ(world.size(), 3u);
    expectVecNear(glm::vec3(world[2][3]), glm::vec3(5.0f, 0.0f, 0.0f));
    expectVecNear(glm::vec3(world[1][3]), glm::vec3(5.0f, 1.0f, 0.0f));
    expectVecNear(glm::vec3(world[0][3]), glm::vec3(5.0f, 2.0f, 0.0f));
}

TEST(SceneAnimator, ARigWithNoSkinStillGetsACharacter) {
    // A clip that drives node transforms animates rigid placements, and a scene of
    // moving crates has a hierarchy to resolve and nothing to skin.
    SceneAnimator anim;
    anim.init(rigOf(outOfOrderChain(), {}, {}));

    EXPECT_EQ(anim.characterCount(), 1u);
    EXPECT_EQ(anim.skinOf(anim.characterAt(0)), SceneAnimator::kNoSkin);
    EXPECT_EQ(anim.totalJoints(), 0u);
}

TEST(SceneAnimator, ACycleInTheParentLinksTerminates) {
    // Malformed input rather than a state to recover from. Leaving the rest at identity
    // is a visible wrong answer, which beats looping forever.
    std::vector<SceneNode> nodes(2);
    nodes[0].parent = 1;
    nodes[1].parent = 0;

    SceneAnimator anim;
    anim.init(rigOf(nodes, {}, {})); // must return
    EXPECT_EQ(anim.worldTransforms(anim.characterAt(0)).size(), 2u);
}

TEST(SceneAnimator, BindPoseIsBuiltEvenWithNoClips) {
    SceneAnimator anim;
    anim.init(rigOf(outOfOrderChain(), {}, {}));

    EXPECT_TRUE(anim.empty());
    EXPECT_EQ(anim.clipCount(), 0u);
    expectVecNear(glm::vec3(anim.worldTransforms(anim.characterAt(0))[0][3]), glm::vec3(5.0f, 2.0f, 0.0f));
}

TEST(SceneAnimator, JointMatricesAreWorldTimesInverseBind) {
    std::vector<SceneNode> nodes(2);
    nodes[0].parent = -1;
    nodes[0].translation = glm::vec3(1.0f, 0.0f, 0.0f);
    nodes[1].parent = 0;
    nodes[1].translation = glm::vec3(0.0f, 2.0f, 0.0f);

    Skin skin;
    skin.joints = {0, 1};
    skin.inverseBind = {glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, 0.0f)),
                        glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, -2.0f, 0.0f))};

    SceneAnimator anim;
    anim.init(rigOf(nodes, {skin}, {}));

    EXPECT_EQ(anim.characterCount(), 1u);
    EXPECT_EQ(anim.totalJoints(), 2u);
    EXPECT_EQ(anim.jointOffset(0), 0u);

    // At the bind pose the joint matrices are the identity by construction, which is
    // what makes a skinned mesh draw as authored before anything animates it.
    const std::vector<glm::mat4>& j = anim.jointMatrices(0);
    expectMatEq(j[0], glm::mat4(1.0f));
    expectMatEq(j[1], glm::mat4(1.0f));
}

TEST(SceneAnimator, JointOffsetsPackCharactersBackToBack) {
    Skin a;
    a.joints = {0, 1, 2};
    a.inverseBind.assign(3, glm::mat4(1.0f));
    Skin b;
    b.joints = {0, 1};
    b.inverseBind.assign(2, glm::mat4(1.0f));

    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(3), {a, b}, {}));

    EXPECT_EQ(anim.jointOffset(0), 0u);
    EXPECT_EQ(anim.jointOffset(1), 3u);
    EXPECT_EQ(anim.totalJoints(), 5u);

    // A third copy of the first skin extends the same flat numbering, which is the
    // whole of what makes `jointBase` a push constant rather than a lookup.
    const AnimatorId third = anim.create(0);
    EXPECT_EQ(third.index, 2u);
    EXPECT_EQ(anim.jointOffset(2), 5u);
    EXPECT_EQ(anim.totalJoints(), 8u);
}

TEST(SceneAnimator, AddCharacterRejectsASkinThatDoesNotExist) {
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {}));
    EXPECT_FALSE(anim.create(7).valid());
    EXPECT_EQ(anim.characterCount(), 1u);
}

TEST(SceneAnimator, EqualStepsReachTheSamePoseEveryRun) {
    // 5.3's golden images rest on this. S2 changed how it is reached -- accumulation
    // rather than `frame * fixedStep` -- so the property is worth re-checking rather
    // than assuming it survived.
    AnimationClip clip = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(10.0f, 0.0f, 0.0f, 0.0f)});

    const auto poseAfter = [&clip](int steps) {
        SceneAnimator anim;
        anim.init(rigOf(std::vector<SceneNode>(1), {}, {clip}));
        for (int i = 0; i < steps; ++i) anim.update(1.0f / 60.0f);
        return anim.worldTransforms(anim.characterAt(0))[0];
    };

    expectMatEq(poseAfter(37), poseAfter(37));
    EXPECT_FALSE(glm::all(glm::equal(poseAfter(37)[3], poseAfter(38)[3], kEps)));
}

TEST(SceneAnimator, ResolveDoesNotDependOnTheCharactersResolvedBeforeIt) {
    // `resolve` marks placed nodes in one buffer shared by every character, rather than
    // allocating one per character per step. Determinism says a pose may not depend on
    // what ran before it, so the same character is resolved alone and then behind three
    // whose clips are at different times, and the two have to agree exactly.
    //
    // The chain is declared child-before-parent on purpose: that is the case where the
    // marks are read across several passes of the resolve loop, so a mark left set by the
    // previous character would stop a node being written at all -- the world transform
    // would keep whatever the last update put there, which is a wrong answer that still
    // looks like a pose.
    AnimationClip clip = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(0.0f, 0.0f, 6.0f, 0.0f)});

    const auto observedPose = [&clip](uint32_t ahead) {
        SceneAnimator anim;
        anim.init(rigOf(outOfOrderChain(), {}, {clip})); // and that is character 0
        AnimatorId observed = anim.characterAt(0);
        for (uint32_t i = 0; i < ahead; ++i) {
            // Whoever would have been observed becomes a leader instead, on the same clip
            // at its own speed -- so the poses resolved ahead genuinely differ from the
            // one under test, and `ahead == 0` leaves it resolving first and alone.
            anim.play(observed, 0, 0.0f, LoopMode::Loop, 0.25f * static_cast<float>(i + 1));
            observed = anim.create(SceneAnimator::kNoSkin);
        }
        anim.play(observed, 0, 0.0f, LoopMode::Loop, 1.0f);
        for (int step = 0; step < 20; ++step) anim.update(1.0f / 60.0f);
        return anim.worldTransforms(observed);
    };

    const std::vector<glm::mat4> alone = observedPose(0);
    const std::vector<glm::mat4> behindThree = observedPose(3);
    ASSERT_EQ(alone.size(), 3u);
    ASSERT_EQ(behindThree.size(), alone.size());
    // Exact, not near: a leaked mark does not perturb a matrix, it leaves one unwritten.
    for (size_t i = 0; i < alone.size(); ++i) EXPECT_TRUE(alone[i] == behindThree[i]) << "node " << i;
}

TEST(SceneAnimator, SamplingRestartsFromTheBindPoseEachUpdate) {
    // A clip that drives only rotation must not inherit whatever translation a previous
    // sample left behind. The bug that causes looks like a rig that drifts.
    std::vector<SceneNode> nodes(1);
    nodes[0].translation = glm::vec3(3.0f, 0.0f, 0.0f);

    const glm::quat to = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    AnimationClip clip = rotationClip({0.0f, 1.0f}, {quatXyzw(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), quatXyzw(to)});

    SceneAnimator anim;
    anim.init(rigOf(nodes, {}, {clip}));

    for (int i = 0; i < 10; ++i) anim.update(0.1f);

    expectVecNear(glm::vec3(anim.worldTransforms(anim.characterAt(0))[0][3]), glm::vec3(3.0f, 0.0f, 0.0f));
}

// ========================================================= playback (S2.3)

namespace {

/// Two clips that translate node 0 to different places, so which one is playing is
/// readable straight off the world transform.
AnimationRig twoClipRig() {
    AnimationClip a = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(0.0f)});
    a.name = "hold";
    AnimationClip b = translationClip({0.0f, 1.0f},
                                      {glm::vec4(10.0f, 0.0f, 0.0f, 0.0f), glm::vec4(10.0f, 0.0f, 0.0f, 0.0f)});
    b.name = "over_there";

    Skin skin;
    skin.joints = {0};
    skin.inverseBind = {glm::mat4(1.0f)};
    return rigOf(std::vector<SceneNode>(1), {skin}, {a, b});
}

float xOf(const SceneAnimator& anim, uint32_t slot) {
    return anim.worldTransforms(anim.characterAt(slot))[0][3].x;
}

} // namespace

TEST(Playback, CharactersOverOneRigKeepIndependentTimes) {
    SceneAnimator anim;
    anim.init(twoClipRig());
    const AnimatorId second = anim.create(0);
    ASSERT_EQ(second.index, 1u);

    anim.play(anim.characterAt(0), 1);
    anim.update(0.1f);

    EXPECT_NEAR(xOf(anim, 0), 10.0f, kEps);
    EXPECT_NEAR(xOf(anim, 1), 0.0f, kEps) << "the second character was never told to switch";
    EXPECT_EQ(anim.playingClip(anim.characterAt(0)), 1u);
    EXPECT_EQ(anim.playingClip(anim.characterAt(1)), 0u);
}

TEST(Playback, SpeedAndPauseAreOwnedPerCharacter) {
    SceneAnimator anim;
    anim.init(twoClipRig());
    anim.create(0);

    anim.setSpeed(anim.characterAt(0), 2.0f);
    anim.setPlaying(anim.characterAt(1), false);
    anim.update(0.25f);

    EXPECT_NEAR(anim.playingTime(anim.characterAt(0)), 0.5f, kEps);
    EXPECT_NEAR(anim.playingTime(anim.characterAt(1)), 0.0f, kEps);
}

TEST(Playback, PlayingTheClipAlreadyPlayingDoesNotRestartIt) {
    // Anything driving this from held input calls play() every frame.
    SceneAnimator anim;
    anim.init(twoClipRig());

    anim.update(0.4f);
    anim.play(anim.characterAt(0), 0, 0.2f);
    EXPECT_NEAR(anim.playingTime(anim.characterAt(0)), 0.4f, kEps);

    anim.restart(anim.characterAt(0));
    EXPECT_NEAR(anim.playingTime(anim.characterAt(0)), 0.0f, kEps);
}

TEST(Playback, AFadeMovesTheWeightFromZeroToOneOverItsDuration) {
    SceneAnimator anim;
    anim.init(twoClipRig());

    anim.update(0.0f);
    EXPECT_NEAR(anim.fadeWeight(anim.characterAt(0)), 1.0f, kEps);

    anim.play(anim.characterAt(0), 1, 1.0f);
    anim.update(0.25f);
    EXPECT_NEAR(anim.fadeWeight(anim.characterAt(0)), 0.25f, kEps);
    // A quarter of the way from clip 0 (x=0) to clip 1 (x=10).
    EXPECT_NEAR(xOf(anim, 0), 2.5f, 1e-4f);

    anim.update(0.75f);
    EXPECT_NEAR(anim.fadeWeight(anim.characterAt(0)), 1.0f, kEps);
    EXPECT_NEAR(xOf(anim, 0), 10.0f, 1e-4f);

    // And it does not overshoot once it has landed.
    anim.update(1.0f);
    EXPECT_NEAR(xOf(anim, 0), 10.0f, 1e-4f);
}

TEST(Playback, AZeroLengthFadeCuts) {
    SceneAnimator anim;
    anim.init(twoClipRig());

    anim.play(anim.characterAt(0), 1, 0.0f);
    anim.update(0.0f);
    EXPECT_NEAR(anim.fadeWeight(anim.characterAt(0)), 1.0f, kEps);
    EXPECT_NEAR(xOf(anim, 0), 10.0f, 1e-4f);
}

TEST(Playback, OutOfRangeCharactersAndClipsAreIgnoredRatherThanIndexed) {
    SceneAnimator anim;
    anim.init(twoClipRig());

    anim.play(anim.characterAt(99), 0);
    anim.play(anim.characterAt(0), 99);
    anim.setSpeed(anim.characterAt(99), 3.0f);
    anim.setParameter(anim.characterAt(99), 0, 1.0f);

    EXPECT_EQ(anim.playingClip(anim.characterAt(0)), 0u);
    EXPECT_EQ(anim.playingClip(anim.characterAt(99)), SceneAnimator::kNoClip);
    EXPECT_TRUE(anim.jointMatrices(99).empty());
    EXPECT_TRUE(anim.worldTransforms(anim.characterAt(99)).empty());
}

TEST(Playback, FindClipLooksUpByName) {
    SceneAnimator anim;
    anim.init(twoClipRig());
    EXPECT_EQ(anim.findClip("over_there"), 1u);
    EXPECT_EQ(anim.findClip("nothing"), SceneAnimator::kNoSkin);
}

// ==================================================== state machine (S2.4)

namespace {

/// idle -> walk on `speed > 0.5`, walk -> idle on `speed < 0.5`, and a one-shot
/// `jump` trigger from anywhere into a clamped clip that falls back to idle when it
/// finishes. The smallest machine that exercises every branch in stepStateMachine.
AnimationStateMachine locomotion() {
    AnimationStateMachine m;
    m.states = {{"idle", 0, LoopMode::Loop, 1.0f},
                {"walk", 1, LoopMode::Loop, 1.0f},
                {"jump", 2, LoopMode::ClampToEnd, 1.0f}};
    m.parameters = {{"speed", false}, {"jump", true}};
    m.transitions = {
        {kAnyState, 2, {{1, ConditionTest::Greater, 0.5f}}, 0.0f, false},
        {2, 0, {}, 0.0f, true},
        {0, 1, {{0, ConditionTest::Greater, 0.5f}}, 0.0f, false},
        {1, 0, {{0, ConditionTest::Less, 0.5f}}, 0.0f, false},
    };
    return m;
}

AnimationRig threeClipRig() {
    AnimationClip idle = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(0.0f)});
    idle.name = "idle";
    AnimationClip walk = translationClip({0.0f, 1.0f},
                                         {glm::vec4(1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    walk.name = "walk";
    AnimationClip jump = translationClip({0.0f, 0.5f},
                                         {glm::vec4(2.0f, 0.0f, 0.0f, 0.0f), glm::vec4(2.0f, 0.0f, 0.0f, 0.0f)});
    jump.name = "jump";

    Skin skin;
    skin.joints = {0};
    skin.inverseBind = {glm::mat4(1.0f)};
    return rigOf(std::vector<SceneNode>(1), {skin}, {idle, walk, jump});
}

} // namespace

TEST(StateMachine, StartsInTheFirstStateAndPlaysItsClip) {
    SceneAnimator anim;
    anim.init(threeClipRig());
    anim.setStateMachine(locomotion());

    anim.update(0.1f);
    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 0u);
    EXPECT_NEAR(xOf(anim, 0), 0.0f, kEps);
}

TEST(StateMachine, TakesATransitionWhenItsConditionHolds) {
    SceneAnimator anim;
    anim.init(threeClipRig());
    anim.setStateMachine(locomotion());

    anim.setParameter(anim.characterAt(0), 0, 1.0f); // speed
    anim.update(0.1f);
    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 1u);
    EXPECT_NEAR(xOf(anim, 0), 1.0f, kEps);

    anim.setParameter(anim.characterAt(0), 0, 0.0f);
    anim.update(0.1f);
    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 0u);
}

TEST(StateMachine, ATriggerIsConsumedByTheTransitionThatReadsIt) {
    // A trigger left set re-fires the moment the state it led to ends, which reads as a
    // character that will not stop jumping.
    SceneAnimator anim;
    anim.init(threeClipRig());
    anim.setStateMachine(locomotion());

    anim.fire(anim.characterAt(0), 1);
    anim.update(0.1f);
    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 2u);
    EXPECT_NEAR(anim.parameter(anim.characterAt(0), 1), 0.0f, kEps) << "the trigger must be cleared by the transition";

    // Run the clamped jump clip out; it should fall back to idle and stay there.
    for (int i = 0; i < 20; ++i) anim.update(0.1f);
    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 0u);
}

TEST(StateMachine, WaitForExitHoldsUntilTheClampedClipFinishes) {
    SceneAnimator anim;
    anim.init(threeClipRig());
    anim.setStateMachine(locomotion());

    anim.fire(anim.characterAt(0), 1);
    anim.update(0.1f);
    ASSERT_EQ(anim.currentState(anim.characterAt(0)), 2u);

    anim.update(0.1f); // jump is 0.5s long, so this is nowhere near the end
    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 2u);

    // The state machine steps *before* the clips advance, so that a state entered this
    // frame gets a full step of its own clip rather than stalling for one. The cost is
    // exactly here: the frame that reaches the end is not the frame that leaves.
    anim.update(0.5f);
    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 2u);
    anim.update(0.0f);
    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 0u);
}

TEST(StateMachine, FromAnyStateReachesEveryState) {
    SceneAnimator anim;
    anim.init(threeClipRig());
    anim.setStateMachine(locomotion());

    anim.setParameter(anim.characterAt(0), 0, 1.0f);
    anim.update(0.1f);
    ASSERT_EQ(anim.currentState(anim.characterAt(0)), 1u) << "walking";

    anim.fire(anim.characterAt(0), 1);
    anim.update(0.1f);
    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 2u) << "the jump transition is from kAnyState";
}

TEST(StateMachine, TheFirstMatchingTransitionWinsSoTableOrderIsPriority) {
    SceneAnimator anim;
    anim.init(threeClipRig());

    AnimationStateMachine m = locomotion();
    // Both hold at once. `jump` is listed first, so it is the one that fires.
    anim.setStateMachine(m);
    anim.setParameter(anim.characterAt(0), 0, 1.0f);
    anim.fire(anim.characterAt(0), 1);
    anim.update(0.1f);
    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 2u);
}

TEST(StateMachine, ParametersAreOwnedPerCharacter) {
    SceneAnimator anim;
    anim.init(threeClipRig());
    anim.setStateMachine(locomotion());
    anim.create(0);

    anim.setParameter(anim.characterAt(0), 0, 1.0f);
    anim.update(0.1f);

    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 1u);
    EXPECT_EQ(anim.currentState(anim.characterAt(1)), 0u);
    EXPECT_NEAR(anim.parameter(anim.characterAt(1), 0), 0.0f, kEps);
}

TEST(StateMachine, LookupByNameFindsStatesAndParameters) {
    const AnimationStateMachine m = locomotion();
    EXPECT_EQ(m.findState("walk"), 1u);
    EXPECT_EQ(m.findState("fly"), kAnyState);
    EXPECT_EQ(m.findParameter("jump"), 1u);
    EXPECT_EQ(m.findParameter("crouch"), kAnyState);
}

TEST(StateMachine, ACharacterAddedAfterTheMachineStartsInItsEntryState) {
    SceneAnimator anim;
    anim.init(threeClipRig());
    anim.setStateMachine(locomotion());

    const AnimatorId late = anim.create(0);
    anim.update(0.1f);
    EXPECT_EQ(anim.currentState(late), 0u);
    EXPECT_EQ(anim.playingClip(late), 0u);
}

TEST(StateMachine, ATransitionWithAFadeBlendsRatherThanCuts) {
    SceneAnimator anim;
    anim.init(threeClipRig());

    AnimationStateMachine m = locomotion();
    m.transitions[2].duration = 1.0f; // idle -> walk
    anim.setStateMachine(m);

    anim.setParameter(anim.characterAt(0), 0, 1.0f);
    anim.update(0.5f);

    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 1u);
    EXPECT_NEAR(anim.fadeWeight(anim.characterAt(0)), 0.5f, kEps);
    EXPECT_NEAR(xOf(anim, 0), 0.5f, 1e-4f) << "halfway between the idle and walk clips";
}

// ========================================== lifetimes: create and destroy (C1)
//
// The animator is the one subsystem where a handle cannot protect every reference: a
// character's *slot* is written into GpuInstance::meta.w and crosses to the GPU. These
// pin the decision that follows from that, because the golden set cannot -- its one
// skinned case is a single character at a fixed pose, so a joint-buffer bug moves no pixel.

TEST(AnimatorLifetime, DestroyingACharacterMakesTheHandleStale) {
    SceneAnimator anim;
    anim.init(twoClipRig());
    const AnimatorId second = anim.create(0);
    ASSERT_TRUE(anim.valid(second));

    anim.destroy(second);
    EXPECT_TRUE(second.valid());
    EXPECT_FALSE(anim.valid(second));
    EXPECT_EQ(anim.playingClip(second), SceneAnimator::kNoClip);
    EXPECT_EQ(anim.skinOf(second), SceneAnimator::kNoSkin);
    anim.update(0.1f); // must not advance a character that is gone
}

TEST(AnimatorLifetime, ADeadSlotKeepsItsJointBlockAtIdentity) {
    // The decision, stated as a test. A stale meta.w must read *this slot's* matrices --
    // left at identity -- rather than another character's, so the worst outcome of a
    // dangling instance reference is a bind pose rather than someone else's animation.
    SceneAnimator anim;
    anim.init(twoClipRig());
    const AnimatorId second = anim.create(0);
    const uint32_t slot = second.index;
    const uint32_t base = anim.jointOffset(slot);
    const uint32_t total = anim.totalJoints();

    anim.play(second, 1);
    anim.update(0.1f);

    anim.destroy(second);
    EXPECT_EQ(anim.jointOffset(slot), base) << "the base moved, so a stale meta.w now reads someone else";
    EXPECT_EQ(anim.totalJoints(), total) << "the block was reclaimed, so the packing shifted under the GPU";
    for (const glm::mat4& m : anim.jointMatrices(slot)) {
        EXPECT_EQ(m, glm::mat4(1.0f)) << "a dead slot must read as bind pose, not as stale matrices";
    }
}

TEST(AnimatorLifetime, AReusedSlotKeepsTheBaseAndDoesNotAlias) {
    SceneAnimator anim;
    anim.init(twoClipRig());
    const AnimatorId first = anim.create(0);
    const uint32_t base = anim.jointOffset(first.index);
    const uint32_t total = anim.totalJoints();

    anim.destroy(first);
    const AnimatorId reused = anim.create(0); // same skin, so the block fits

    EXPECT_EQ(reused.index, first.index) << "the slot should have been reused";
    EXPECT_NE(reused.generation, first.generation);
    EXPECT_EQ(anim.jointOffset(reused.index), base) << "the base belongs to the slot, not the character";
    EXPECT_EQ(anim.totalJoints(), total) << "reuse must not extend the packed buffer";
    EXPECT_TRUE(anim.valid(reused));
    EXPECT_FALSE(anim.valid(first));
}

TEST(AnimatorLifetime, ASkinThatDoesNotFitTakesAFreshSlotRatherThanTheHole) {
    // Two skins of different joint counts. A block cannot move, so a bigger skin cannot
    // take a smaller hole -- it appends, and the hole waits for one that fits.
    Skin small;
    small.joints = {0};
    small.inverseBind = {glm::mat4(1.0f)};
    Skin large;
    large.joints = {0, 0, 0};
    large.inverseBind = {glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)};

    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {small, large}, {}));
    // init() made one character per skin: slot 0 (1 joint), slot 1 (3 joints).
    const AnimatorId smallOne = anim.characterAt(0);
    ASSERT_TRUE(smallOne.valid());
    const uint32_t totalBefore = anim.totalJoints();

    anim.destroy(smallOne);
    const AnimatorId big = anim.create(1); // 3 joints; the 1-joint hole cannot hold it
    ASSERT_TRUE(big.valid());
    EXPECT_NE(big.index, smallOne.index) << "a skin was placed in a block too small for it";
    EXPECT_GT(anim.totalJoints(), totalBefore) << "it should have extended the buffer";

    // And one that does fit takes the hole.
    const AnimatorId sm = anim.create(0);
    ASSERT_TRUE(sm.valid());
    EXPECT_EQ(sm.index, smallOne.index);
}

TEST(AnimatorLifetime, FindClipReportsItsOwnSentinelRatherThanACharacterOne) {
    // The D3 defect, fixed here because C1 removed the sentinel it was borrowing.
    SceneAnimator anim;
    anim.init(twoClipRig());
    EXPECT_EQ(anim.findClip("no such clip"), SceneAnimator::kNoClip);
    EXPECT_NE(anim.findClip("hold"), SceneAnimator::kNoClip);
}

// ================================================ animation events (C7)
//
// "A footstep on the frame the foot lands" is the milestone, and the property that makes
// it work at a bad frame rate is that *every* crossing in the step fires, not the nearest.

namespace {

/// A one-second clip with events at 0.25 and 0.75, driving one node's translation.
AnimationRig eventRig() {
    AnimationClip clip = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    clip.name = "walk";
    clip.events = {{0.25f, "left"}, {0.75f, "right"}};
    return rigOf(std::vector<SceneNode>(1), {}, {clip});
}

std::vector<std::string> firedNames(const SceneAnimator& anim) {
    std::vector<std::string> out;
    for (const FiredEvent& f : anim.firedEvents()) out.push_back(anim.clip(f.clip).events[f.event].name);
    return out;
}

} // namespace

TEST(AnimationEvents, AnEventFiresOnTheStepThatCrossesIt) {
    SceneAnimator anim;
    anim.init(eventRig());
    anim.play(anim.characterAt(0), 0);

    anim.update(0.2f); // 0 -> 0.2, before the first event
    EXPECT_TRUE(firedNames(anim).empty());

    anim.update(0.1f); // 0.2 -> 0.3, crossing 0.25
    EXPECT_EQ(firedNames(anim), (std::vector<std::string>{"left"}));

    anim.update(0.1f); // 0.3 -> 0.4, nothing
    EXPECT_TRUE(firedNames(anim).empty());
}

TEST(AnimationEvents, AnEventFiresOnceRatherThanEveryFrameAfterIt) {
    SceneAnimator anim;
    anim.init(eventRig());
    anim.play(anim.characterAt(0), 0);

    anim.update(0.3f);
    ASSERT_EQ(firedNames(anim).size(), 1u);
    for (int i = 0; i < 4; ++i) {
        anim.update(0.05f);
        EXPECT_TRUE(firedNames(anim).empty()) << "an event re-fired while the playhead sat past it";
    }
}

TEST(AnimationEvents, ALowFrameRateStillFiresEveryCrossing) {
    // The half that matters: one big step spanning both events must produce both, or a
    // game drops footsteps exactly when it is already struggling.
    SceneAnimator anim;
    anim.init(eventRig());
    anim.play(anim.characterAt(0), 0);

    anim.update(0.9f); // 0 -> 0.9, crossing both
    EXPECT_EQ(firedNames(anim), (std::vector<std::string>{"left", "right"}));
}

TEST(AnimationEvents, ALoopWrapFiresTheEventsOnBothSidesOfTheSeam) {
    SceneAnimator anim;
    anim.init(eventRig());
    anim.play(anim.characterAt(0), 0);

    anim.update(0.8f); // 0 -> 0.8, crossing both 0.25 and 0.75
    ASSERT_EQ(firedNames(anim), (std::vector<std::string>{"left", "right"}));

    // 0.8 -> 1.1, which wraps to 0.1. Nothing lies in (0.8, 1.0] or (0, 0.1].
    anim.update(0.3f);
    EXPECT_TRUE(firedNames(anim).empty());

    // 0.1 -> 0.4, crossing 0.25.
    anim.update(0.3f);
    EXPECT_EQ(firedNames(anim), (std::vector<std::string>{"left"}));
}

TEST(AnimationEvents, AnEventFiresAtMostOnceEvenIfTheStepLappedTheClip) {
    // A stated cap: a game that dropped a frame wants one footstep, not eleven.
    SceneAnimator anim;
    anim.init(eventRig());
    anim.play(anim.characterAt(0), 0);

    anim.update(11.0f);
    const std::vector<std::string> names = firedNames(anim);
    EXPECT_LE(names.size(), 2u) << "eleven laps produced more than one fire per event";
}

TEST(AnimationEvents, APausedPlaybackFiresNothing) {
    SceneAnimator anim;
    anim.init(eventRig());
    anim.play(anim.characterAt(0), 0);
    anim.setPlaying(anim.characterAt(0), false);

    anim.update(2.0f);
    EXPECT_TRUE(firedNames(anim).empty());
}

TEST(AnimationEvents, TheFadingOutClipDoesNotFire) {
    // Otherwise one step gives a character two footsteps through every cross-fade.
    AnimationClip a = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    a.name = "one";
    a.events = {{0.05f, "from_a"}};
    AnimationClip b = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    b.name = "two";

    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {a, b}));
    const AnimatorId c = anim.characterAt(0);
    anim.play(c, 0);
    anim.update(0.5f); // past the event once
    anim.play(c, 1, 0.5f);

    // Clip "one" is now fading out and its playhead keeps moving; it must stay silent.
    for (int i = 0; i < 20; ++i) {
        anim.update(0.05f);
        for (const std::string& n : firedNames(anim)) EXPECT_NE(n, "from_a");
    }
}

TEST(AnimationEvents, EventsAreReportedPerCharacter) {
    SceneAnimator anim;
    anim.init(eventRig());
    const AnimatorId second = anim.create(SceneAnimator::kNoSkin);
    anim.play(anim.characterAt(0), 0);
    anim.play(second, 0);

    anim.update(0.3f);
    ASSERT_EQ(anim.firedEvents().size(), 2u);
    EXPECT_NE(anim.firedEvents()[0].character, anim.firedEvents()[1].character);
}

// ================================================== root motion (C7)
//
// Without this a locomotion clip authored with the feet planted makes the mesh walk while
// the controller stands still, and the character slides. The two halves have to happen
// together: report the delta *and* hold the node, or the character moves twice.

TEST(RootMotion, IsZeroUntilARootNodeIsNamed) {
    // Opt-in, because taking translation out of a clip that was not authored to carry it
    // would silently delete that clip's animation.
    SceneAnimator anim;
    anim.init(twoClipRig());
    anim.play(anim.characterAt(0), 1);
    anim.update(0.1f);
    EXPECT_EQ(anim.rootMotion(anim.characterAt(0)), glm::vec3(0.0f));
    EXPECT_EQ(anim.rootNode(), SceneAnimator::kNoNode);
}

// **The half that makes the opt-in reachable.** `setRootNode` takes an index, a rig's joints
// are a file's business, and until `findNode` existed nothing in the tree could turn a joint's
// name into one -- so the feature had six unit tests naming a synthetic node 0 and no caller,
// and the showcase character walked at twice its speed and snapped home on every release for
// want of one lookup.
TEST(RootMotion, AJointIsFoundByTheNameTheFileGaveIt) {
    AnimationRig rig = rigOf(std::vector<SceneNode>(3), {}, {});
    rig.nodeNames = {"Armature", "Hips", ""};
    SceneAnimator anim;
    anim.init(rig);

    EXPECT_EQ(anim.findNode("Hips"), 1u);
    EXPECT_EQ(anim.findNode("Armature"), 0u);
    // A name no node carries is `kNoNode` rather than 0, or a game would silently hold the
    // first joint of a rig that does not have the one it asked for.
    EXPECT_EQ(anim.findNode("mixamorig:Hips"), SceneAnimator::kNoNode);
    // And the empty name is not a wildcard: an unnamed node matches only the empty string,
    // which is a lookup no game makes.
    EXPECT_EQ(anim.findNode(""), 2u);

    anim.setRootNode(anim.findNode("Hips"));
    EXPECT_EQ(anim.rootNode(), 1u);
}

TEST(RootMotion, ANameNoRigCarriesLeavesTheHoldOff) {
    // The failure a game must survive: a scene whose rig is not the one it was written for.
    // `findNode` answers `kNoNode`, `setRootNode` is never called, and the pose keeps whatever
    // the clip authored -- wrong for that game, and not a crash or a held node 0.
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(2), {}, {}));
    EXPECT_EQ(anim.findNode("Hips"), SceneAnimator::kNoNode);
    EXPECT_EQ(anim.rootNode(), SceneAnimator::kNoNode);
}

TEST(RootMotion, ReportsThePerStepTranslationDelta) {
    // A one-second clip translating the node from x=0 to x=10.
    AnimationClip walk = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(10.0f, 0.0f, 0.0f, 0.0f)});
    walk.name = "walk";
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {walk}));
    anim.setRootNode(0);
    anim.play(anim.characterAt(0), 0);

    // The first update establishes the baseline rather than reporting the whole distance
    // from the origin as one step.
    anim.update(0.1f);
    EXPECT_EQ(anim.rootMotion(anim.characterAt(0)), glm::vec3(0.0f));

    anim.update(0.1f);
    EXPECT_NEAR(anim.rootMotion(anim.characterAt(0)).x, 1.0f, 1e-4f);

    anim.update(0.2f);
    EXPECT_NEAR(anim.rootMotion(anim.characterAt(0)).x, 2.0f, 1e-4f);
}

TEST(RootMotion, HoldsTheNodeStillSoTheCharacterDoesNotMoveTwice) {
    AnimationClip walk = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(10.0f, 0.0f, 0.0f, 0.0f)});
    walk.name = "walk";
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {walk}));
    anim.setRootNode(0);
    anim.play(anim.characterAt(0), 0);

    for (int i = 0; i < 5; ++i) anim.update(0.1f);
    // The pose animates in place: the world transform must not have walked off with it.
    EXPECT_NEAR(anim.worldTransforms(anim.characterAt(0))[0][3].x, 0.0f, 1e-4f);
}

TEST(RootMotion, TheVerticalBobIsAnimationRatherThanLocomotionAndSurvivesTheHold) {
    // The bug this pins: the hold pinned all three axes to the *bind* translation, so a rig
    // whose clips sit lower than its bind pose was lifted by the difference and stood on
    // nothing. The showcase rig binds its hips at 1.043 and idles them between 0.946 and
    // 0.978 -- eight centimetres of float, and no bob, because Y was pinned flat.
    //
    // Root motion is horizontal. X and Z are held, because the controller applies those and
    // leaving them in would move the character twice; Y is the clip's to say.
    AnimationClip walk = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f, 2.0f, 0.0f, 0.0f),
                                                        glm::vec4(10.0f, 3.0f, 10.0f, 0.0f)});
    walk.name = "walk";
    std::vector<SceneNode> nodes(1);
    nodes[0].translation = {0.0f, 5.0f, 0.0f}; // a bind height nothing in the clip agrees with
    SceneAnimator anim;
    anim.init(rigOf(nodes, {}, {walk}));
    anim.setRootNode(0);
    anim.play(anim.characterAt(0), 0);

    for (int i = 0; i < 5; ++i) anim.update(0.1f);

    const glm::mat4 world = anim.worldTransforms(anim.characterAt(0))[0];
    EXPECT_NEAR(world[3][0], 0.0f, 1e-4f); // held
    EXPECT_NEAR(world[3][2], 0.0f, 1e-4f); // held
    EXPECT_NEAR(world[3][1], 2.5f, 1e-4f); // the clip's, not the bind's 5.0

    // And the delta stays horizontal, or whatever consumes it would drive the character
    // into the floor and the ceiling on alternate steps.
    EXPECT_EQ(anim.rootMotion(anim.characterAt(0)).y, 0.0f);
}

TEST(RootMotion, WithoutARootNodeTheNodeStillMoves) {
    // The control for the test above: the holding is what setRootNode turns on, not
    // something the sampler does anyway.
    AnimationClip walk = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(10.0f, 0.0f, 0.0f, 0.0f)});
    walk.name = "walk";
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {walk}));
    anim.play(anim.characterAt(0), 0);

    for (int i = 0; i < 5; ++i) anim.update(0.1f);
    EXPECT_GT(anim.worldTransforms(anim.characterAt(0))[0][3].x, 1.0f);
}

TEST(RootMotion, ChangingTheRootNodeRestartsTheMeasurement) {
    AnimationClip walk = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(10.0f, 0.0f, 0.0f, 0.0f)});
    walk.name = "walk";
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {walk}));
    anim.setRootNode(0);
    anim.play(anim.characterAt(0), 0);
    anim.update(0.1f);
    anim.update(0.1f);
    ASSERT_GT(anim.rootMotion(anim.characterAt(0)).x, 0.0f);

    anim.setRootNode(0); // re-set: the baseline is dropped
    anim.update(0.1f);
    EXPECT_EQ(anim.rootMotion(anim.characterAt(0)), glm::vec3(0.0f))
        << "a delta measured across a root change is meaningless and must not be reported";
}

TEST(RootMotion, IsReportedPerCharacter) {
    AnimationClip walk = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(10.0f, 0.0f, 0.0f, 0.0f)});
    walk.name = "walk";
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {walk}));
    anim.setRootNode(0);
    const AnimatorId a = anim.characterAt(0);
    const AnimatorId b = anim.create(SceneAnimator::kNoSkin);
    anim.play(a, 0);
    anim.play(b, 0);
    anim.setSpeed(b, 2.0f);

    anim.update(0.1f);
    anim.update(0.1f);
    EXPECT_GT(anim.rootMotion(b).x, anim.rootMotion(a).x) << "the faster character covered more ground";
}

// ================================== weights a game writes rather than a clip (G11)
//
// The half of G11 the row turned on. `create` sizes a character's weight block from
// `rig.bind.weights` -- what the *file* declared -- so a mesh made in code has nowhere for
// its weights to live in a scene whose glTF has no morph target at all, which is every
// scene in this repository but one. `createMorphed` asks for the block directly.
//
// Everything below is the packing, not the deformation. What the weighted sum does to a
// vertex is `skinning.comp` and only a device can run it; what these check is that the
// numbers the dispatch is handed are the right ones -- which is where the defect class
// G12 found in `PhysicsWorld::snapshot` lives, two arrays laid out to match and one of
// them grown.

TEST(CodeMadeMorphWeights, ARigWithNoMorphTargetAtAllStillGivesABlock) {
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {}));
    ASSERT_EQ(anim.totalWeights(), 0u) << "the rig declares no weights, which is the whole problem";

    const AnimatorId banner = anim.createMorphed(3);
    ASSERT_TRUE(anim.valid(banner));
    EXPECT_EQ(anim.totalWeights(), 3u);
    EXPECT_EQ(anim.morphWeights(banner.index).size(), 3u);
}

TEST(CodeMadeMorphWeights, ZeroTargetsIsNoCharacter) {
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {}));
    const uint32_t before = anim.characterCount();

    EXPECT_FALSE(anim.createMorphed(0).valid()) << "a mesh with no targets has nothing to drive";
    EXPECT_EQ(anim.characterCount(), before) << "and must not cost a slot";
}

TEST(CodeMadeMorphWeights, AWeightSurvivesEveryUpdate) {
    // The reason this is a second creation verb rather than an argument to `create`. A
    // rig-driven pose is rebuilt from the bind pose every step so a clip driving rotation
    // alone cannot accumulate drift; a block belonging to no node of the rig would be
    // resized away by exactly that copy, and a game's weight would last one frame.
    AnimationClip walk = translationClip({0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {walk}));

    const AnimatorId banner = anim.createMorphed(2);
    anim.setMorphWeight(banner, 0, 0.75f);
    anim.setMorphWeight(banner, 1, -0.25f);

    for (int i = 0; i < 20; ++i) anim.update(1.0f / 60.0f);

    ASSERT_EQ(anim.morphWeights(banner.index).size(), 2u) << "the block must not have been resized by the bind copy";
    EXPECT_NEAR(anim.morphWeights(banner.index)[0], 0.75f, kEps);
    EXPECT_NEAR(anim.morphWeights(banner.index)[1], -0.25f, kEps) << "a morph weight is a coefficient, not a fraction";
    EXPECT_NEAR(anim.morphWeight(banner, 0), 0.75f, kEps);
}

TEST(CodeMadeMorphWeights, AWeightIsVisibleBeforeTheNextUpdate) {
    // The renderer uploads `morphWeights` every frame whether or not the fixed step ran
    // between the two, so a setter that wrote only the held copy would be a frame late
    // exactly when a game stepped the animator less often than it drew.
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {}));
    const AnimatorId banner = anim.createMorphed(1);
    anim.setMorphWeight(banner, 0, 0.4f);
    EXPECT_NEAR(anim.morphWeights(banner.index)[0], 0.4f, kEps);
}

TEST(CodeMadeMorphWeights, TwoMorphedCharactersDoNotAlias) {
    // Two blocks packed back to back, of different lengths. Writing one must not be
    // readable through the other's offset, and neither offset may move when the second
    // is created -- which is the property `GpuInstance::meta.w` crossing to the GPU makes
    // load-bearing rather than tidy.
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {}));

    const AnimatorId a = anim.createMorphed(2);
    const uint32_t aBase = anim.weightOffset(a.index);
    const AnimatorId b = anim.createMorphed(3);

    EXPECT_EQ(anim.weightOffset(a.index), aBase) << "an existing base can never move";
    EXPECT_EQ(anim.weightOffset(b.index), aBase + 2u);
    EXPECT_EQ(anim.totalWeights(), aBase + 5u);

    anim.setMorphWeight(a, 0, 1.0f);
    anim.setMorphWeight(a, 1, 1.0f);
    anim.update(1.0f / 60.0f);

    for (uint32_t t = 0; t < 3; ++t) EXPECT_NEAR(anim.morphWeight(b, t), 0.0f, kEps);

    // And the flat buffer the renderer builds out of the two: `weightOffset(c)` is where
    // character `c`'s run starts, and the runs must tile it without overlapping.
    std::vector<float> flat(anim.totalWeights(), -1.0f);
    for (uint32_t c = 0; c < anim.characterCount(); ++c) {
        const std::vector<float>& w = anim.morphWeights(c);
        ASSERT_LE(anim.weightOffset(c) + w.size(), flat.size()) << "character " << c << " overruns the buffer";
        std::copy(w.begin(), w.end(), flat.begin() + anim.weightOffset(c));
    }
    EXPECT_EQ(std::count(flat.begin(), flat.end(), -1.0f), 0) << "every float in the region belongs to somebody";
}

TEST(CodeMadeMorphWeights, AMorphedBlockPacksBehindASkinnedOne) {
    // The interleaving the demo actually produces: a skinned character out of the file,
    // then a morphed one made in code. The joint and the weight prefix sums are separate
    // arrays advanced by separate counts, which is precisely the pair that can drift.
    Skin skin;
    skin.joints = {0, 1, 2};
    skin.inverseBind.assign(3, glm::mat4(1.0f));

    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(3), {skin}, {}, {0.0f, 0.0f}));
    ASSERT_EQ(anim.characterCount(), 1u);
    EXPECT_EQ(anim.totalJoints(), 3u);
    EXPECT_EQ(anim.totalWeights(), 2u) << "the file's own two, from the rig";

    const AnimatorId banner = anim.createMorphed(4);
    EXPECT_EQ(anim.weightOffset(banner.index), 2u) << "behind the file's, not over them";
    EXPECT_EQ(anim.totalWeights(), 6u);
    EXPECT_EQ(anim.totalJoints(), 3u) << "a character with no skeleton adds no joints";
    EXPECT_EQ(anim.jointOffset(banner.index), 3u);
}

TEST(CodeMadeMorphWeights, ARetiredMorphedCharacterGoesInertAndKeepsItsBase) {
    // C1's rule, which applies to a weight block for the same reason it applies to a
    // joint block: `meta.w` names the slot and crosses to the GPU, so a stale instance
    // must read something harmless rather than somebody else's expression.
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {}));

    const AnimatorId first = anim.createMorphed(3);
    const uint32_t base = anim.weightOffset(first.index);
    anim.setMorphWeight(first, 1, 1.0f);
    anim.destroy(first);

    EXPECT_EQ(anim.weightOffset(first.index), base) << "the base belongs to the slot, not the character";
    for (float w : anim.morphWeights(first.index)) EXPECT_NEAR(w, 0.0f, kEps) << "zero is the undeformed mesh";
    EXPECT_FALSE(anim.valid(first));
    anim.setMorphWeight(first, 0, 1.0f);
    EXPECT_NEAR(anim.morphWeights(first.index)[0], 0.0f, kEps) << "a stale handle writes nothing";

    // And the slot comes back for a block that fits, at the same base.
    const AnimatorId second = anim.createMorphed(2);
    EXPECT_EQ(second.index, first.index);
    EXPECT_EQ(anim.weightOffset(second.index), base);
    EXPECT_EQ(anim.totalWeights(), 3u) << "reuse must not grow the region";
}

TEST(CodeMadeMorphWeights, ATargetOutsideTheBlockIsIgnored) {
    SceneAnimator anim;
    anim.init(rigOf(std::vector<SceneNode>(1), {}, {}));
    const AnimatorId banner = anim.createMorphed(2);
    const AnimatorId neighbour = anim.createMorphed(2);

    anim.setMorphWeight(banner, 2, 1.0f); ///< one past its own block: the neighbour's first
    anim.setMorphWeight(banner, 99, 1.0f);
    for (uint32_t t = 0; t < 2; ++t) EXPECT_NEAR(anim.morphWeight(neighbour, t), 0.0f, kEps);
    EXPECT_NEAR(anim.morphWeight(banner, 2), 0.0f, kEps);
}

// ============================================ a machine per character (C23)

namespace {

/// The same three states as `locomotion()`, but the speed threshold is inverted: `speed`
/// above 0.5 goes *back* to idle and below it walks. A second character running this reaches
/// the opposite state from the same parameter write, which is what makes "did they get their
/// own machine" answerable without reading the machine back out.
AnimationStateMachine mirroredLocomotion() {
    AnimationStateMachine m;
    m.states = {{"idle", 0, LoopMode::Loop, 1.0f}, {"walk", 1, LoopMode::Loop, 1.0f}};
    m.parameters = {{"speed", false}};
    m.transitions = {
        {0, 1, {{0, ConditionTest::Less, 0.5f}}, 0.0f, false},
        {1, 0, {{0, ConditionTest::Greater, 0.5f}}, 0.0f, false},
    };
    return m;
}

} // namespace

TEST(StateMachinePerCharacter, TwoCharactersRunDifferentMachinesFromTheSameParameterWrite) {
    SceneAnimator anim;
    anim.init(threeClipRig());
    const AnimatorId second = anim.create(0);
    ASSERT_TRUE(second.valid());
    const AnimatorId first = anim.characterAt(0);
    ASSERT_NE(first.index, second.index);

    anim.setStateMachine(locomotion());          // both
    anim.setStateMachine(second, mirroredLocomotion()); // then one of them

    // Parameter 0 is `speed` in both machines, so this is one write meaning two things.
    anim.setParameter(first, 0, 1.0f);
    anim.setParameter(second, 0, 1.0f);
    anim.update(0.1f);

    EXPECT_EQ(anim.currentState(first), 1u) << "the shared machine walks above 0.5";
    EXPECT_EQ(anim.currentState(second), 0u) << "the mirrored machine idles above 0.5";
}

TEST(StateMachinePerCharacter, InstallingOneCharactersMachineLeavesTheOthersAlone) {
    SceneAnimator anim;
    anim.init(threeClipRig());
    const AnimatorId second = anim.create(0);
    const AnimatorId first = anim.characterAt(0);

    anim.setStateMachine(locomotion());
    anim.setParameter(first, 0, 1.0f);
    anim.update(0.1f);
    ASSERT_EQ(anim.currentState(first), 1u);

    // Giving `second` its own machine must not reset `first` onto an entry state.
    anim.setStateMachine(second, mirroredLocomotion());
    EXPECT_EQ(anim.currentState(first), 1u);
    EXPECT_EQ(anim.stateMachine(first).states.size(), 3u);
    EXPECT_EQ(anim.stateMachine(second).states.size(), 2u);
}

TEST(StateMachinePerCharacter, TheAnimatorWideCallStillReachesEveryCharacter) {
    // The one-rig path, which is every scene in the tree: one call, and a character created
    // *after* it still starts on the machine's entry state rather than on nothing.
    SceneAnimator anim;
    anim.init(threeClipRig());
    anim.setStateMachine(locomotion());
    const AnimatorId late = anim.create(0);
    ASSERT_TRUE(late.valid());

    anim.setParameter(anim.characterAt(0), 0, 1.0f);
    anim.setParameter(late, 0, 1.0f);
    anim.update(0.1f);

    EXPECT_EQ(anim.currentState(anim.characterAt(0)), 1u);
    EXPECT_EQ(anim.currentState(late), 1u) << "a character created after the install got no machine";
    EXPECT_EQ(anim.stateMachine().states.size(), 3u) << "the template is what a late character copies";
}

TEST(StateMachinePerCharacter, ARootNodeSetOnOneCharacterDoesNotHoldTheOthers) {
    // Node 0 is the only node in `threeClipRig`, and `walk` translates it to x = 1. Holding
    // it pins the pose at the bind translation; not holding it lets the clip through.
    SceneAnimator anim;
    anim.init(threeClipRig());
    const AnimatorId second = anim.create(0);
    const AnimatorId first = anim.characterAt(0);

    anim.setStateMachine(locomotion());
    anim.setParameter(first, 0, 1.0f);
    anim.setParameter(second, 0, 1.0f);
    anim.setRootNode(second, 0);
    EXPECT_EQ(anim.rootNode(first), SceneAnimator::kNoNode);
    EXPECT_EQ(anim.rootNode(second), 0u);

    anim.update(0.1f);
    anim.update(0.1f);

    EXPECT_NEAR(anim.worldTransforms(first)[0][3].x, 1.0f, kEps) << "the unheld character kept its clip";
    EXPECT_NEAR(anim.worldTransforms(second)[0][3].x, 0.0f, kEps) << "the held character animates in place";
    // And the delta is reported to the one that asked for it, not to both.
    EXPECT_EQ(anim.rootMotion(first), glm::vec3(0.0f));
}
