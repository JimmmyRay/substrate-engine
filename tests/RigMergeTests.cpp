#include "anim/SceneAnimator.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <vector>

using namespace scene;

using namespace anim;

/**
 * @file tests/RigMergeTests.cpp
 * @brief The offset arithmetic a rig arriving at runtime has to get right.
 *
 * **This is the check the golden suite cannot be.** A wrong offset does not crash: it draws
 * somebody else's vertices for the frames the mesh is visible, and no golden case has two
 * rigs in it, so the whole class of defect is invisible to eleven byte-identical images.
 *
 * Every case here is about a number being renumbered or deliberately not renumbered, and
 * about the characters that were already playing when it happened.
 */

namespace {

/// A chain of `nodes` nodes, each the child of the one before, translated one unit apart so
/// a resolved world transform says which node it came from.
AnimationRig chainRig(uint32_t nodes, const char* prefix, bool withSkin, bool withClip) {
    AnimationRig rig;
    rig.bind.nodes.resize(nodes);
    rig.nodeNames.resize(nodes);
    for (uint32_t i = 0; i < nodes; ++i) {
        rig.bind.nodes[i].parent = i == 0 ? -1 : static_cast<int32_t>(i - 1);
        rig.bind.nodes[i].translation = {1.0f, 0.0f, 0.0f};
        rig.nodeNames[i] = std::string(prefix) + std::to_string(i);
    }

    if (withSkin) {
        Skin s;
        for (uint32_t i = 0; i < nodes; ++i) {
            s.joints.push_back(i);
            s.inverseBind.emplace_back(1.0f);
        }
        rig.skins.push_back(std::move(s));
    }

    if (withClip) {
        AnimationSampler sampler;
        sampler.times = {0.0f, 1.0f};
        sampler.values = {glm::vec4(5.0f, 0.0f, 0.0f, 0.0f), glm::vec4(5.0f, 0.0f, 0.0f, 0.0f)};
        sampler.stride = 1;

        AnimationChannel channel;
        // The *last* node of the chain, so a channel that was not shifted lands somewhere
        // a test can tell apart from a channel that was.
        channel.node = nodes - 1;
        channel.path = AnimationPath::Translation;
        channel.sampler = 0;

        AnimationClip clip;
        clip.name = std::string(prefix) + "clip";
        clip.duration = 1.0f;
        clip.samplers.push_back(std::move(sampler));
        clip.channels.push_back(channel);
        rig.clips.push_back(std::move(clip));
    }
    return rig;
}

} // namespace

TEST(RigMerge, NodesKeepTheirIndicesAndTheAppendedOnesFollow) {
    SceneAnimator anim;
    anim.init(chainRig(3, "base", true, false));
    ASSERT_EQ(anim.characterCount(), 1u);

    const uint32_t base = anim.merge(chainRig(2, "extra", true, false));
    EXPECT_EQ(base, 1u);

    // Nothing before the merge moved, which is what makes an `AnimatorId`, a root-motion
    // node index and a `GpuInstance::meta.w` from before it still mean what they meant.
    EXPECT_EQ(anim.findNode("base0"), 0u);
    EXPECT_EQ(anim.findNode("base2"), 2u);
    EXPECT_EQ(anim.findNode("extra0"), 3u);
    EXPECT_EQ(anim.findNode("extra1"), 4u);
}

TEST(RigMerge, AnAppendedRootStaysARoot) {
    // The appended file's own root is a root of the merged rig. Grafting it onto a node of
    // the base scene would be a decision about *content*, and the caller's transform has
    // already placed the import.
    SceneAnimator anim;
    anim.init(chainRig(3, "base", false, false));
    anim.merge(chainRig(2, "extra", false, false));

    const std::vector<glm::mat4>& world = anim.worldTransforms(anim.characterAt(0));
    ASSERT_EQ(world.size(), 5u);
    // base0 at x=1, base1 at 2, base2 at 3 -- and extra0 back at 1 because it is a root.
    EXPECT_FLOAT_EQ(world[0][3][0], 1.0f);
    EXPECT_FLOAT_EQ(world[2][3][0], 3.0f);
    EXPECT_FLOAT_EQ(world[3][3][0], 1.0f) << "the appended root inherited a parent";
    EXPECT_FLOAT_EQ(world[4][3][0], 2.0f);
}

TEST(RigMerge, AnAppendedSkinsJointsNameTheAppendedNodes) {
    // The failure this exists for: a joint list left file-local makes the imported character
    // deform by the *base scene's* skeleton, silently, for the frames it is on screen.
    SceneAnimator anim;
    anim.init(chainRig(3, "base", true, false));
    anim.merge(chainRig(2, "extra", true, false));

    ASSERT_EQ(anim.rig().skins.size(), 2u);
    EXPECT_EQ(anim.rig().skins[0].joints, (std::vector<uint32_t>{0, 1, 2}));
    EXPECT_EQ(anim.rig().skins[1].joints, (std::vector<uint32_t>{3, 4}));
}

TEST(RigMerge, AnAppendedClipDrivesTheAppendedNodeAndNotTheBaseScenes) {
    SceneAnimator anim;
    anim.init(chainRig(3, "base", true, false));
    const uint32_t base = anim.merge(chainRig(2, "extra", true, true));
    ASSERT_EQ(base, 1u);
    ASSERT_EQ(anim.rig().clips.size(), 1u);

    // The clip drove node 1 of its own file, which is node 4 of the merged rig.
    EXPECT_EQ(anim.rig().clips[0].channels[0].node, 4u);

    const AnimatorId appended = anim.characterAt(1);
    ASSERT_TRUE(anim.valid(appended));
    anim.play(appended, anim.findClip("extraclip"));
    anim.update(0.5f);

    // The appended chain: root at 1, and its child driven to 5 by the clip.
    const std::vector<glm::mat4>& world = anim.worldTransforms(appended);
    ASSERT_EQ(world.size(), 5u);
    EXPECT_FLOAT_EQ(world[3][3][0], 1.0f);
    EXPECT_FLOAT_EQ(world[4][3][0], 6.0f) << "the appended clip did not reach the appended node";

    // And the base scene's character is where it was, untouched by a clip that is not its.
    const std::vector<glm::mat4>& baseWorld = anim.worldTransforms(anim.characterAt(0));
    EXPECT_FLOAT_EQ(baseWorld[2][3][0], 3.0f);
}

TEST(RigMerge, ACharacterPlayingBeforeTheMergeIsStillPlayingAfterIt) {
    // `init` clears every character, which is why it cannot be reused for this: an import
    // into a running world would destroy the world's own characters. The card's leak arm is
    // about the arrays; this is about the handles.
    SceneAnimator anim;
    anim.init(chainRig(3, "base", true, true));
    const AnimatorId first = anim.characterAt(0);
    anim.play(first, anim.findClip("baseclip"));
    anim.update(0.25f);
    const float before = anim.playingTime(first);

    anim.merge(chainRig(2, "extra", true, false));

    EXPECT_TRUE(anim.valid(first)) << "the merge invalidated a live character";
    EXPECT_EQ(anim.playingClip(first), 0u);
    EXPECT_FLOAT_EQ(anim.playingTime(first), before) << "the merge restarted a playing clip";
}

TEST(RigMerge, EveryCharacterSPoseGrowsWithTheRig) {
    // A pose is copied from `rig.bind` each step, so growth reaches an existing character
    // without the merge touching it -- and `merge` resolves once so an instance drawn
    // before the next step draws the appended bind pose rather than whatever was there.
    SceneAnimator anim;
    anim.init(chainRig(3, "base", true, false));
    ASSERT_EQ(anim.worldTransforms(anim.characterAt(0)).size(), 3u);

    anim.merge(chainRig(2, "extra", true, false));
    EXPECT_EQ(anim.worldTransforms(anim.characterAt(0)).size(), 5u);
}

TEST(RigMerge, MorphWeightOffsetsAreShiftedAndAWeightlessNodeIsNot) {
    AnimationRig base = chainRig(2, "base", false, false);
    base.bind.weights = {0.25f, 0.5f};
    base.bind.nodes[1].firstWeight = 0;
    base.bind.nodes[1].weightCount = 2;

    AnimationRig extra = chainRig(2, "extra", false, false);
    extra.bind.weights = {0.75f};
    extra.bind.nodes[0].firstWeight = 0;
    extra.bind.nodes[0].weightCount = 1;

    SceneAnimator anim;
    anim.init(std::move(base));
    anim.merge(extra);

    const AnimationRig& merged = anim.rig();
    ASSERT_EQ(merged.bind.weights.size(), 3u);
    EXPECT_EQ(merged.bind.nodes[1].firstWeight, 0u);
    EXPECT_EQ(merged.bind.nodes[2].firstWeight, 2u) << "the appended weight block was not shifted";
    EXPECT_EQ(merged.bind.nodes[2].weightCount, 1u);
    // A node with no weights keeps `firstWeight` at zero rather than being shifted into
    // somebody else's block -- the count is what says it has none, so a shifted zero would
    // be a live-looking index nothing bounds.
    EXPECT_EQ(merged.bind.nodes[3].weightCount, 0u);
    EXPECT_EQ(merged.bind.nodes[3].firstWeight, 0u);
}

TEST(RigMerge, ARigWithNoSkinAddsNodesAndNoCharacter) {
    // Unlike `init`, whose lone character exists so a scene of animated crates has a
    // hierarchy to resolve. The base scene already has one, and a second would be a second
    // copy of the whole pose for a file that only added nodes.
    SceneAnimator anim;
    anim.init(chainRig(3, "base", true, false));
    ASSERT_EQ(anim.characterCount(), 1u);

    EXPECT_EQ(anim.merge(chainRig(2, "extra", false, false)), SceneAnimator::kNoSkin);
    EXPECT_EQ(anim.characterCount(), 1u);
    EXPECT_EQ(anim.rig().bind.nodes.size(), 5u);
}

TEST(RigMerge, TwoImportsInARowEachLandPastTheLast) {
    SceneAnimator anim;
    anim.init(chainRig(2, "base", true, false));
    EXPECT_EQ(anim.merge(chainRig(2, "a", true, false)), 1u);
    EXPECT_EQ(anim.merge(chainRig(3, "b", true, false)), 2u);

    EXPECT_EQ(anim.findNode("a0"), 2u);
    EXPECT_EQ(anim.findNode("b0"), 4u);
    EXPECT_EQ(anim.rig().skins[1].joints, (std::vector<uint32_t>{2, 3}));
    EXPECT_EQ(anim.rig().skins[2].joints, (std::vector<uint32_t>{4, 5, 6}));
    EXPECT_EQ(anim.characterCount(), 3u);
}

TEST(RigMerge, NodeNamesStayParallelToTheNodesEvenWhenAFileNamesNothing) {
    // A short `nodeNames` makes `findNode` search a prefix and `setRootNode` unreachable
    // for every node past it -- two vectors laid out to match, one of them grown, which is
    // the shape of defect this whole card is about one array along.
    AnimationRig extra = chainRig(3, "extra", true, false);
    extra.nodeNames.clear();

    SceneAnimator anim;
    anim.init(chainRig(2, "base", true, false));
    anim.merge(extra);

    EXPECT_EQ(anim.rig().nodeNames.size(), anim.rig().bind.nodes.size());
    EXPECT_EQ(anim.findNode("base1"), 1u);
    EXPECT_EQ(anim.findNode("extra0"), SceneAnimator::kNoNode);
}

// ------------------------------------------- which rig a bare node index belongs to (bug)

/**
 * Both of `Engine`'s attachment sweeps resolved every node index against character 0, so
 * the second character's torch burned at the first one's hand and four-player co-op put
 * every player's effects on player one. A `ParticleEmitter::node` and an
 * `AudioSourceDesc::node` are indices into the merged rig and carry no rig of their own --
 * the glTF `extras` schemas have nowhere to put one -- so the animator is what has to be
 * able to say which character a node belongs to.
 *
 * No golden case has two animated characters, which is why this never showed up as a moved
 * pixel.
 */
TEST(RigMerge, ANodeNamesTheCharacterThatAnimatesItRatherThanTheFirstOne) {
    SceneAnimator anim;
    anim.init(chainRig(3, "base", true, true));
    ASSERT_EQ(anim.merge(chainRig(2, "extra", true, true)), 1u);
    ASSERT_EQ(anim.characterCount(), 2u);

    const AnimatorId first = anim.characterAt(0);
    const AnimatorId second = anim.characterAt(1);
    anim.play(first, anim.findClip("baseclip"));
    anim.play(second, anim.findClip("extraclip"));
    anim.update(0.5f);

    // Node 4 is the appended rig's second node: a joint of the appended skin, and the one
    // the appended clip drives.
    EXPECT_TRUE(anim.characterForNode(4) == second);
    EXPECT_TRUE(anim.characterForNode(2) == first);
    // A node past the rig belongs to nobody. Answering slot 0 here is the whole defect in
    // miniature, so it comes back invalid and the caller decides.
    EXPECT_FALSE(anim.characterForNode(99).valid());

    // And that resolution is exactly what an attachment sweep needs. The same index read out
    // of the two characters is two different places -- one the joint as posed, the other the
    // bind pose a character that does not animate it leaves behind.
    EXPECT_FLOAT_EQ(anim.worldTransforms(anim.characterForNode(4))[4][3][0], 6.0f);
    EXPECT_FLOAT_EQ(anim.worldTransforms(first)[4][3][0], 2.0f) << "character 0 leaves it at bind";
}

TEST(RigMerge, TwoCopiesOfOneSkinAgreeOnWhoOwnsASharedJoint) {
    // The case the old comment was right about, kept: `spawnExtraCharacters` gives every copy
    // the same skin, so the joint really is shared and any answer but a stable one puts an
    // attachment on a different copy from one frame to the next.
    SceneAnimator anim;
    anim.init(chainRig(3, "base", true, true));
    const AnimatorId clone = anim.create(0);
    ASSERT_TRUE(clone.valid());
    anim.update(0.1f);

    EXPECT_TRUE(anim.characterForNode(1) == anim.characterAt(0));
    anim.update(0.1f);
    EXPECT_TRUE(anim.characterForNode(1) == anim.characterAt(0)) << "the owner moved between frames";
}

TEST(RigMerge, ARigidNodeBelongsToWhoeverIsPlayingTheClipThatMovesIt) {
    // The third case the sweep silently mixed in: an animated hierarchy of its own -- a
    // drawbridge, a clock tower -- merged beside a character rig. No skin claims its nodes,
    // so the skin pass cannot answer and the clip pass is what does.
    //
    // Both rigs carry a clip of their own so that clip 0 -- what a character plays before
    // anybody says otherwise -- is the base scene's rather than the tower's.
    SceneAnimator anim;
    anim.init(chainRig(3, "base", true, true));
    ASSERT_EQ(anim.merge(chainRig(2, "extra", true, true)), 1u);
    EXPECT_EQ(anim.merge(chainRig(2, "tower", false, true)), SceneAnimator::kNoSkin);
    ASSERT_EQ(anim.characterCount(), 2u);

    const AnimatorId second = anim.characterAt(1);
    anim.play(second, anim.findClip("towerclip"));
    anim.update(0.5f);

    // The tower's nodes are 5 and 6, and its clip drives the second of them.
    EXPECT_TRUE(anim.characterForNode(6) == second);
    EXPECT_FLOAT_EQ(anim.worldTransforms(anim.characterForNode(6))[6][3][0], 6.0f);
}
