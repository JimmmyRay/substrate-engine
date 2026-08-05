#pragma once

#include "core/Clip.h"
#include "core/Handle.h"
#include "scene/Node.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file engine/scene/AnimationRig.h
 * @brief What a glTF document authors about a rig, and the maths that reads it back out.
 *
 * The description half, beside `ParticleEmitter.h` and `AudioSource.h` for the same reason:
 * the loaders, the `.scene` sidecar and the instance table all reach these, and none of them
 * may name the module that drives them. `anim::SceneAnimator` is the other half.
 *
 * Blend trees, retargeting and IK are declined -- see limitations.md.
 */
namespace scene {

/// What an animation channel drives on its target node.
enum class AnimationPath : uint32_t {
    Translation,
    Rotation,
    Scale,
    Weights,
};

/// How values between two keyframes are combined.
enum class AnimationInterpolation : uint32_t {
    Linear,
    Step,
    /// Hermite spline with in/out tangents; each keyframe stores three values.
    CubicSpline,
};

/**
 * @brief Keyframe times and values for one channel.
 *
 * **`stride` is what says which array holds the values**: zero for TRS in `values`,
 * non-zero for morph weights in `weights`, never both. A TRS key is a `vec4` whatever
 * the path is, xyz for translation and scale.
 */
struct AnimationSampler {
    std::vector<float> times;
    std::vector<glm::vec4> values;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    std::vector<float> weights;
    /// Morph targets per keyframe, times three for CubicSpline.
    uint32_t stride = 0;
};

struct AnimationChannel {
    uint32_t node = 0;
    AnimationPath path = AnimationPath::Translation;
    uint32_t sampler = 0;
};

struct AnimationClip {
    std::string name;
    /// Instants a game may react to, **in ascending time order** -- `crossedEvents` finds a
    /// crossing without sorting, so an unordered list silently misses events.
    std::vector<core::AnimationEvent> events;
    /// Seconds; the latest keyframe time across every sampler. Zero for a clip with no
    /// keys, which the sampler holds at the bind pose rather than dividing by.
    float duration = 0.0f;
    std::vector<AnimationSampler> samplers;
    std::vector<AnimationChannel> channels;
};

/// One node of the retained glTF hierarchy. Every transform here is local to `parent`;
/// the world transform is the product down the parent chain.
struct SceneNode {
    int32_t parent = -1;
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    /// This node's range in the pose's flat weight array. A count of zero is a node with
    /// no morphed mesh, and a `weights` channel targeting it writes nothing rather than
    /// off the end.
    uint32_t firstWeight = 0;
    uint32_t weightCount = 0;
};

/// A glTF skin. `inverseBind` is parallel to `joints` and takes each joint from model
/// space into its own bind pose.
struct Skin {
    std::vector<uint32_t> joints;
    std::vector<glm::mat4> inverseBind;
};

/// What a clip evaluates into: local node transforms and morph weights.
struct Pose {
    std::vector<SceneNode> nodes;
    std::vector<float> weights;
};

/**
 * @brief The immutable half of an animated character: what the file declares once.
 *
 * Shared by every character that plays it, so a write after load reaches all of them at
 * once. `anim::SceneAnimator::merge` is the only sanctioned mutation.
 */
struct AnimationRig {
    /// The hierarchy as the file declares it. Sampling starts from this pose every
    /// frame, so a clip that drives only rotation leaves translations alone.
    Pose bind;
    /// Parallel to `bind.nodes`. Entries may be empty; glTF does not require a node to
    /// be named, and it does not require names to be unique.
    std::vector<std::string> nodeNames;
    std::vector<Skin> skins;
    std::vector<AnimationClip> clips;

    [[nodiscard]] bool empty() const { return clips.empty(); }
};

/// Compose a node's local transform. Order is fixed by the glTF spec: translate,
/// rotate, scale, applied to a point right to left.
glm::mat4 localTransform(const SceneNode& n);

/**
 * @brief Evaluate `clip` at `time` seconds, wrapped into [0, duration), into `pose`.
 *
 * Nodes no channel targets keep whatever they already hold, so a caller that wants a
 * clean pose has to reset to the bind pose first.
 */
void sampleClip(const AnimationClip& clip, float time, Pose& pose);

/**
 * @brief Move `dst` a fraction `t` of the way toward `src`, in place.
 *
 * **Rotation is spherical** -- a lerp between two quaternions takes the chord rather than
 * the arc, and a cross-fade built on one makes every limb dip toward the body through the
 * middle of the transition. `t` outside [0, 1] is clamped, not extrapolated: overshooting
 * turns a rig inside out. Poses of different lengths blend over the shorter of the two.
 */
void blendPose(Pose& dst, const Pose& src, float t);

/// `core::advance`, against a clip's own duration.
bool advance(core::ClipPlayback& p, const AnimationClip& clip, float dt);

/// `core::crossedEvents`, against a clip's own events and duration.
void crossedEvents(const core::ClipPlayback& p, const AnimationClip& clip, float from, std::vector<uint32_t>& out);

/// How a transition condition compares a parameter against its value.
enum class ConditionTest : uint32_t {
    Greater,
    Less,
    /// Exact float equality -- right for a bool-shaped parameter, a coin flip for a
    /// continuous one.
    Equal,
    NotEqual,
};

struct AnimationCondition {
    uint32_t parameter = 0;
    ConditionTest test = ConditionTest::Greater;
    float value = 0.0f;
};

/// A named parameter a transition can test. **A `trigger` is cleared by the transition
/// that consumes it**, so a caller that also clears it takes the transition away.
struct AnimationParameter {
    std::string name;
    bool trigger = false;
};

struct AnimationState {
    std::string name;
    uint32_t clip = 0;
    core::LoopMode loop = core::LoopMode::Loop;
    float speed = 1.0f;
};

/// Sentinel `from` meaning "from any state".
inline constexpr uint32_t kAnyState = 0xFFFFFFFFu;

/// "No skin" -- a hierarchy whose clips move rigid placements rather than joints. Also what
/// `Primitive::skinOffset` and `Placement::skin` carry for geometry nothing deforms.
inline constexpr uint32_t kNoSkin = 0xFFFFFFFFu;

struct AnimationTransition {
    uint32_t from = kAnyState;
    uint32_t to = 0;
    /// Every condition must hold; an "or" is two transitions.
    std::vector<AnimationCondition> conditions;
    /// Cross-fade length in seconds. Zero is a cut, and a legitimate answer.
    float duration = 0.2f;
    /// Only once the source state's clip has finished. A looping source never satisfies
    /// it, so a transition set on one never fires.
    bool waitForExit = false;
};

/// States, transitions and parameters. The current state and the parameter values are
/// per character and live in `anim::SceneAnimator`.
struct AnimationStateMachine {
    std::vector<AnimationState> states;
    std::vector<AnimationTransition> transitions;
    std::vector<AnimationParameter> parameters;

    [[nodiscard]] bool empty() const { return states.empty(); }
    /// Index of `name`, or UINT32_MAX.
    [[nodiscard]] uint32_t findState(const std::string& name) const;
    [[nodiscard]] uint32_t findParameter(const std::string& name) const;
};

/// Tag for an animator character. Declared, never defined -- see `core/Handle.h`.
struct AnimatorCharacterTag;
/**
 * @brief One driven copy of a rig: a pose and a clip playhead.
 *
 * Not a `PhysicsCharacterId`, which is a capsule the solver sweeps; a scene can have either
 * without the other. Here rather than with `anim::SceneAnimator` for the reason `SoundId` is
 * here rather than with the mixer: the engine hands these back and stores them, and a handle
 * declared in a module is one the engine cannot name.
 */
using AnimatorId = core::Handle<AnimatorCharacterTag>;

} // namespace scene
