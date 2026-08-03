#pragma once

#include "scene/Node.h"
#include "core/Handle.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scene {

/**
 * @file Animation.h
 * @brief Clips, poses, playback and state machines.
 *
 * Turns a time into node transforms, and node transforms into the joint matrices a
 * skinning dispatch multiplies by. Per-character playback (a rig is shared, a character is
 * not), two-clip cross-fades driven by a state machine, and morph weights sampled beside
 * the node transforms because they come out of the same clip.
 *
 * Plain structs and free functions over them. `SceneAnimator` is the one type with state,
 * because its working arrays are reused every frame rather than reallocated.
 *
 * Blend trees, retargeting and IK are declined -- see limitations.md for the triggers.
 */

/// What an animation channel drives on its target node.
enum class AnimationPath : uint32_t {
    Translation,
    Rotation,
    Scale,
    /// Morph target weights: one float per target rather than a vec4, which is why
    /// `AnimationSampler` carries a second value array.
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
 * TRS values are `vec4` whatever the path is: a translation or scale uses xyz and a
 * rotation uses all four. One array rather than a variant keeps the sampler a single
 * function.
 *
 * A weights channel gets its own flat array instead, `stride` floats per key. **`stride`
 * is what says which of the two a sampler is** -- zero for TRS, non-zero for weights, and
 * never both.
 */
struct AnimationSampler {
    std::vector<float> times;
    std::vector<glm::vec4> values;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    /// Morph weights, `stride` per keyframe (times `3` for CUBICSPLINE, as ever).
    std::vector<float> weights;
    /// Morph targets per keyframe. Zero means this is a TRS sampler.
    uint32_t stride = 0;
};

struct AnimationChannel {
    uint32_t node = 0;
    AnimationPath path = AnimationPath::Translation;
    uint32_t sampler = 0;
};

/// A named instant on a clip's timeline. The engine never interprets the name; it only
/// reports that the instant was crossed.
struct AnimationEvent {
    /// Seconds from the start of the clip.
    float time = 0.0f;
    std::string name;
};

/// One event fired during an update, reported by `SceneAnimator::firedEvents`.
struct FiredEvent {
    /// Which character's playback crossed it.
    uint32_t character = 0;
    /// Index into `SceneAnimator::clip(clip).events`.
    uint32_t clip = 0;
    uint32_t event = 0;
};

struct AnimationClip {
    std::string name;
    /// Instants a game may react to, **in ascending time order** -- `crossedEvents` relies
    /// on that to find a crossing without sorting per frame. Empty for every clip a glTF
    /// declares; the importer has nowhere to read them from, so they are authored in code.
    std::vector<AnimationEvent> events;
    /// Latest keyframe time across every sampler. Zero for a clip with no keys, which
    /// the sampler treats as "hold the bind pose" rather than dividing by it.
    float duration = 0.0f;
    std::vector<AnimationSampler> samplers;
    std::vector<AnimationChannel> channels;
};

/// One node of the retained glTF hierarchy. Retained because animation drives *local*
/// transforms and a joint's world transform is the product down its parent chain, which
/// flattened world transforms cannot express.
struct SceneNode {
    int32_t parent = -1;
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    /// Where this node's morph weights start in the pose's flat weight array, and how
    /// many it has. A node with no morphed mesh has a count of zero, and a `weights`
    /// channel targeting it writes nothing rather than off the end.
    uint32_t firstWeight = 0;
    uint32_t weightCount = 0;
};

/// A glTF skin: which nodes are joints, and the matrix taking each from model space
/// into its own bind pose.
struct Skin {
    std::vector<uint32_t> joints;
    std::vector<glm::mat4> inverseBind;
};

/// Everything a clip evaluates into: local node transforms and morph weights. One struct
/// rather than two arguments, because a blend that updated one and not the other is a
/// character whose face lags its body by a frame.
struct Pose {
    std::vector<SceneNode> nodes;
    std::vector<float> weights;
};

/**
 * @brief The immutable half of an animated character: what the file declares once.
 *
 * Shared by every character that plays it. Nothing in here changes after load, which
 * is what makes forty copies of a skeleton cost forty *poses* rather than forty rigs.
 */
struct AnimationRig {
    /// The hierarchy as the file declares it. Sampling starts from this pose every
    /// frame, so a clip that drives only rotation leaves translations alone.
    Pose bind;
    /// What the file called each node, parallel to `bind.nodes`. **Without it
    /// `setRootNode` is unreachable** -- the opt-in takes a node index and a game knows
    /// joints by name. Empty entries are legal; glTF does not require a node to be named.
    std::vector<std::string> nodeNames;
    std::vector<Skin> skins;
    std::vector<AnimationClip> clips;

    [[nodiscard]] bool empty() const { return clips.empty(); }
};

/// Compose a node's local transform. Order is fixed by the glTF spec: translate,
/// rotate, scale, applied to a point right to left.
glm::mat4 localTransform(const SceneNode& n);

/**
 * @brief Evaluate `clip` at `time` into `pose`.
 *
 * Nodes no channel targets keep whatever they already hold, which is what makes a
 * caller's "reset to the bind pose, then sample" the same thing as "sample" for every
 * node the clip actually drives.
 *
 * `time` is wrapped into [0, duration). Wrapping here rather than at the call site is
 * the difference between a clip that loops and one that freezes on its last key the
 * moment somebody forgets a fmod.
 */
void sampleClip(const AnimationClip& clip, float time, Pose& pose);

/**
 * @brief Move `dst` a fraction `t` of the way toward `src`, in place.
 *
 * Translation, scale and weights are linear; **rotation is spherical** -- a lerp between
 * two quaternions takes the chord rather than the arc, and a cross-fade built on one makes
 * every limb dip toward the body through the middle of the transition.
 *
 * `t` outside [0, 1] is clamped rather than extrapolated: overshooting turns a rig inside
 * out for the frames a transition timer ran past its duration.
 *
 * Poses of different lengths blend over the shorter of the two.
 */
void blendPose(Pose& dst, const Pose& src, float t);

/// What happens when a clip reaches its last key.
enum class LoopMode : uint32_t {
    /// Wrap to the start. The default, and what every locomotion clip wants.
    Loop,
    /// Hold the final pose. What a transition clip wants, and what makes
    /// `AnimationTransition::waitForExit` a question with an answer.
    ClampToEnd,
};

/**
 * @brief One clip playing on one character.
 *
 * Absolute `time` rather than a phase, so a clip's own duration stays the only thing
 * that decides when it wraps.
 */
struct ClipPlayback {
    uint32_t clip = 0;
    float time = 0.0f;
    float speed = 1.0f;
    LoopMode loop = LoopMode::Loop;
    bool playing = true;
};

/**
 * @brief Advance `p` by `dt` along a timeline `duration` seconds long.
 *
 * @return true if a `ClampToEnd` playback is sitting on its final key -- which is what
 *         a "play this once, then move on" transition tests. A looping playback never
 *         reports it, because it never finishes.
 *
 * A paused playback advances by nothing and still reports whether it has finished: the
 * question is about where the time is, not about whether it moved this frame.
 *
 * **Takes a duration rather than a clip** so `SpriteTable`'s flipbooks -- a length, a loop
 * mode and events, with no samplers or channels -- can call it without a synthetic
 * `AnimationClip`. The overload below is the clip.
 */
bool advance(ClipPlayback& p, float duration, float dt);

/// The same, against a clip's own duration. What `SceneAnimator` calls.
bool advance(ClipPlayback& p, const AnimationClip& clip, float dt);

/**
 * @brief Which of `events` a step from `from` to `p.time` crossed.
 *
 * Appends indices into `events` to `out`. **Split from `advance` rather than folded into
 * it** because the fading-*out* clip advances too, and firing its events would give a
 * character two footsteps for one step during every cross-fade.
 *
 * **`events` must be in ascending time order** -- the one thing a caller owes this
 * function, and what lets a crossing be found without sorting per frame.
 *
 * **Every crossing in the interval fires, not just the nearest**: a 20 Hz frame over a
 * clip with three events in that 50 ms produces three.
 *
 * Handles the wrap: a looping clip whose step crossed the end reports the events after
 * `from` *and* the events before the new time. An event fires **at most once per call**
 * even if `dt` lapped the clip several times -- a stated cap, so a game that dropped a
 * frame gets one footstep and not eleven.
 */
void crossedEvents(const ClipPlayback& p, const std::vector<AnimationEvent>& events, float duration, float from,
                   std::vector<uint32_t>& out);

/// The same, against a clip's own events and duration. What `SceneAnimator` calls.
void crossedEvents(const ClipPlayback& p, const AnimationClip& clip, float from, std::vector<uint32_t>& out);

/// How a transition condition compares a parameter against its value.
enum class ConditionTest : uint32_t {
    Greater,
    Less,
    /// Exact equality, which is what a bool-shaped float wants. A parameter that
    /// carries a continuous quantity should use Greater or Less instead.
    Equal,
    NotEqual,
};

struct AnimationCondition {
    uint32_t parameter = 0;
    ConditionTest test = ConditionTest::Greater;
    float value = 0.0f;
};

/// A named parameter a transition can test. **`trigger` is cleared the moment a transition
/// consumes it**, so "jump" fires once per press; a trigger the caller had to clear
/// re-fires its transition the instant the state it leads to ends.
struct AnimationParameter {
    std::string name;
    bool trigger = false;
};

struct AnimationState {
    std::string name;
    uint32_t clip = 0;
    LoopMode loop = LoopMode::Loop;
    float speed = 1.0f;
};

/// Sentinel `from` meaning "from any state". An interrupt -- death, damage, a hit
/// reaction -- is a transition out of everything, and enumerating it per state is how
/// the one state somebody forgot becomes the one the character cannot be hit in.
inline constexpr uint32_t kAnyState = 0xFFFFFFFFu;

struct AnimationTransition {
    uint32_t from = kAnyState;
    uint32_t to = 0;
    /// Every condition must hold. An "or" is two transitions, which keeps the
    /// evaluation a single loop and the data a table rather than an expression tree.
    std::vector<AnimationCondition> conditions;
    /// Cross-fade length in seconds. Zero is a cut, and a legitimate answer.
    float duration = 0.2f;
    /// Only once the source state's clip has finished. Meaningless for a looping
    /// state, and evaluated as such: a looping source never satisfies it.
    bool waitForExit = false;
};

/// States, transitions and parameters, shared across every character that runs it. The
/// current state and the parameter values are per character and live in `SceneAnimator`.
struct AnimationStateMachine {
    std::vector<AnimationState> states;
    std::vector<AnimationTransition> transitions;
    std::vector<AnimationParameter> parameters;

    [[nodiscard]] bool empty() const { return states.empty(); }
    /// Index of `name`, or UINT32_MAX. Linear, and deliberately: a state machine with
    /// enough states for a map to pay for itself is one that wanted a sub-machine.
    [[nodiscard]] uint32_t findState(const std::string& name) const;
    [[nodiscard]] uint32_t findParameter(const std::string& name) const;
};

/// Tag for an animator character. Declared, never defined -- see `core/Handle.h`.
struct AnimatorCharacterTag;
/// One driven copy of a rig. Distinct from `PhysicsCharacterId`: that is a capsule the
/// solver sweeps, this is a pose and a clip playhead, and a scene can have one without the
/// other.
using AnimatorId = core::Handle<AnimatorCharacterTag>;

/**
 * @brief Drives N characters over one shared rig and produces their joint matrices.
 *
 * Every character owns a pose, a world-transform array and a joint block, all sized at
 * creation so a frame allocates nothing. **The joint blocks are laid end to end in one
 * flat numbering** -- `jointOffset(c)` says where character `c`'s begin -- because that
 * numbering is the skinning dispatch's `jointBase` push constant, and a second layout
 * would have to be kept in step with it.
 */

class SceneAnimator {
  public:
    /// Sentinel for "no skin" -- a hierarchy whose clips move rigid placements. Still a
    /// bare `uint32_t` because a skin is an index into the rig's own array, has no
    /// lifetime of its own, and is never destroyed.
    static constexpr uint32_t kNoSkin = 0xFFFFFFFFu;
    /// "No such clip", returned by `findClip`. Its own name rather than a character-index
    /// sentinel borrowed because the two numbers happen to agree -- principles.md rule 8.
    static constexpr uint32_t kNoClip = 0xFFFFFFFFu;

    /// Take ownership of the rig and create one character per skin, so character `i`
    /// drives skin `i` and an instance table recording a skin index already names the
    /// right character. More copies come from `create`.
    void init(AnimationRig rig);

    /**
     * @brief Append a second rig, keeping every character already playing (C22).
     *
     * @return the index the first appended skin landed at, which is what a caller adds to
     *         a `Placement::skin` from the same file. `kNoSkin` when `extra` has no skins,
     *         because there is then nothing to renumber against.
     *
     * **`init` cannot be reused for this and calling it twice is the bug this replaces.**
     * It clears `characters`, so re-initialising after an import destroys every character
     * already animating -- which on an imported *second* rig is the entire base scene.
     *
     * Everything the appended rig names by index is renumbered here and nowhere else: node
     * parents, a skin's joints, a channel's target and a node's first morph weight. Nothing
     * already in the rig moves, so an `AnimatorId`, a root-motion node, a clip index and a
     * `GpuInstance::meta.w` from before the merge all still mean what they meant.
     *
     * One character is created per appended skin, at the end, so their joint and weight
     * blocks land past every existing one. That is the same arrangement `create` already
     * makes and it is why the merge does not have to touch a single existing base.
     */
    uint32_t merge(const AnimationRig& extra);

    [[nodiscard]] bool empty() const { return rigData.clips.empty() && rigData.skins.empty(); }
    [[nodiscard]] size_t clipCount() const { return rigData.clips.size(); }
    [[nodiscard]] const AnimationClip& clip(size_t i) const { return rigData.clips[i]; }
    /// Index of the clip named `name`, or `kNoClip`. What a state machine built from a
    /// config file needs, since a file names clips and cannot know their order.
    [[nodiscard]] uint32_t findClip(const std::string& name) const;

    /// Add another character driven by `skin`, or by no skin at all -- a hierarchy whose
    /// clips move rigid placements has world transforms and no joints.
    /// @return an invalid handle if the skin does not exist.
    AnimatorId create(uint32_t skin = kNoSkin);

    /**
     * @brief Add a character with no skeleton whose `targets` morph weights are the
     *        caller's to write -- the door a mesh made in code goes through.
     *
     * `create` sizes a character's weight block from what the *file* declared, so a
     * procedural mesh's targets have nowhere to live. This asks for the block directly.
     *
     * **The weights survive `update`**, which is why this is a second verb rather than an
     * argument to the first: a rig-driven pose is rebuilt from the bind pose every step so
     * a rotation-only clip cannot accumulate drift, and that copy would resize away a block
     * belonging to no node. These are held beside the pose and written back after sampling.
     *
     * No clip and no state machine reaches these targets. `setMorphWeight` is the only
     * writer.
     *
     * @return an invalid handle for `targets == 0`.
     */
    AnimatorId createMorphed(uint32_t targets);

    /// Write one of `createMorphed`'s weights. Out of range is **ignored rather than
    /// clamped** -- a caller off by one wants to know.
    void setMorphWeight(AnimatorId character, uint32_t target, float weight);
    [[nodiscard]] float morphWeight(AnimatorId character, uint32_t target) const;

    /**
     * @brief Retire a character.
     *
     * **A dead character keeps its joint block, filled with identity, and keeps its base.**
     * A character's index crosses to the GPU in `GpuInstance::meta.w`, so unlike a body or
     * a sound there is no way to make a stale reference fail. Both alternatives alias:
     * repacking the prefix sums moves every later character's matrices under any instance
     * still naming this one, and reusing the block for another skin points it at a
     * different skeleton. Inert means a stale instance draws its bind pose.
     *
     * **The caller must clear or repoint any instance whose `meta.w` names this
     * character** -- `InstanceTable::setCharacter`. This class cannot, and must not learn
     * about instances in order to.
     */
    void destroy(AnimatorId id);

    [[nodiscard]] bool valid(AnimatorId id) const {
        return id.valid() && id.index < characters.size() && characters[id.index].generation == id.generation &&
               characters[id.index].live;
    }

    /// The handle occupying a slot, for the walkers. Invalid for a retired slot.
    [[nodiscard]] AnimatorId characterAt(uint32_t slot) const {
        if (slot >= characters.size() || !characters[slot].live) return {};
        return AnimatorId{slot, characters[slot].generation};
    }

    /// Slots, not live characters -- what a walker pairs with `characterAt`, and what
    /// `meta.w` indexes.
    [[nodiscard]] uint32_t characterCount() const { return static_cast<uint32_t>(characters.size()); }

    /**
     * @brief Events crossed by the most recent `update`.
     *
     * Refilled every update and read after it -- a list rather than a callback, which would
     * be the engine calling into a game mid-update.
     *
     * Only the *current* clip of each character fires; a clip fading out does not, or a
     * cross-fade would give one step two footsteps.
     */
    [[nodiscard]] const std::vector<FiredEvent>& firedEvents() const { return fired; }

    /**
     * @brief How far the clip moved the root during the last `update`, in the character's
     *        own space.
     *
     * Hand it to the controller rather than to the transform --
     * `physics.setCharacterInput(id, animator.rootMotion(c) / step, jump)` -- so the motion
     * still collides with the world.
     *
     * **Zero unless `setRootNode` named one.** Opt-in because taking the motion out of the
     * pose is only correct when the clip was authored to contain it; a clip that was not
     * would have its animation silently deleted.
     */
    [[nodiscard]] glm::vec3 rootMotion(AnimatorId character) const;

    /**
     * @brief Which node's translation is root motion, and take it out of the pose.
     *
     * `kNoNode` -- the default -- disables it. Naming a node makes `update` record that
     * node's per-step translation delta and then **hold the node still**, so the pose
     * animates in place and the motion is the caller's to apply. One call rather than two,
     * because reporting the delta while leaving the node moving moves the character twice.
     *
     * **Still in X and Z only.** The vertical axis is animation rather than locomotion --
     * it is the bob of a walk cycle and the crouch of an idle -- so the clip keeps it, in
     * the pose and out of the delta. Holding it as well stands the character off the floor
     * by however far its bind pose sits above its clips. See `update` for the arithmetic.
     */
    /// Every character, which is what a scene with one rig wants and every scene in this
    /// tree is.
    void setRootNode(uint32_t node);
    /// One character (C23). A rig whose pelvis is not called what the last caller's was
    /// needs its own answer.
    void setRootNode(AnimatorId character, uint32_t node);
    /// This character's root node. Symmetric with `stateMachine(id)`.
    [[nodiscard]] uint32_t rootNode(AnimatorId character) const;
    /// The root node new characters are given. What `setRootNode(node)` last set, and the
    /// value a one-rig game compares against to keep from restarting its own measurement.
    [[nodiscard]] uint32_t rootNode() const { return defaultRootMotionNode; }

    /**
     * @brief The node the file gave this name, or `kNoNode`.
     *
     * What makes `setRootNode` usable: a game knows joints by name, not by index. Linear
     * over the rig, because this is a load-time call rather than a per-frame one.
     *
     * First match wins -- glTF does not require names to be unique.
     */
    [[nodiscard]] uint32_t findNode(std::string_view name) const;

    /// "No root node", and therefore no root motion. An alias; the declaration is
    /// `scene/Node.h`.
    static constexpr uint32_t kNoNode = scene::kNoNode;

    // ------------------------------------------------------------------------ playback

    /// Cross-fade `character` onto `clip` over `fade` seconds. A fade of zero cuts.
    /// **Playing the clip that is already playing is a no-op, not a restart** -- "keep
    /// walking" is called every frame by anything driving this from input.
    void play(AnimatorId character, uint32_t clip, float fade = 0.0f, LoopMode loop = LoopMode::Loop,
              float speed = 1.0f);
    /// Restart the current clip from its first key, fade included -- what `play` refuses
    /// to do.
    void restart(AnimatorId character);
    void setPlaying(AnimatorId character, bool playing);
    void setSpeed(AnimatorId character, float speed);
    /// The clip currently being faded *to*. Mid-fade the previous clip is still
    /// contributing.
    [[nodiscard]] uint32_t playingClip(AnimatorId character) const;
    [[nodiscard]] float playingTime(AnimatorId character) const;
    /// 1.0 when no fade is in progress, so "has the transition landed" needs no inference
    /// from two clips.
    [[nodiscard]] float fadeWeight(AnimatorId character) const;

    // ------------------------------------------------------------------ state machine

    /**
     * @brief Install the machine every character runs. Characters that already exist are
     *        reset onto its entry state; ones created afterwards start there.
     *
     * **The machine is copied per character, not shared** (C23). A scene with one rig
     * cannot tell the difference and this is still the only call it makes; a scene with two
     * gets to give them different clips through the overload below, which is what the
     * animator could not express when it held one machine for everybody.
     *
     * `defaultMachine` is what a character created *after* this call starts with, so the
     * ordering that used to matter -- install the machine, then load the rig -- still does
     * not.
     */
    void setStateMachine(AnimationStateMachine machine);
    /// One character's machine. Resets that character onto the new machine's entry state
    /// and leaves every other character alone.
    void setStateMachine(AnimatorId character, AnimationStateMachine machine);
    /// This character's machine. An empty machine for an id the animator does not hold,
    /// which reads as "no states" everywhere a machine is walked.
    [[nodiscard]] const AnimationStateMachine& stateMachine(AnimatorId character) const;
    /// The machine new characters are given. What `setStateMachine(machine)` last set.
    [[nodiscard]] const AnimationStateMachine& stateMachine() const { return defaultMachine; }
    [[nodiscard]] uint32_t currentState(AnimatorId character) const;
    void setParameter(AnimatorId character, uint32_t parameter, float value);
    [[nodiscard]] float parameter(AnimatorId character, uint32_t parameter) const;
    /// Set a trigger parameter. Identical to `setParameter(p, 1)`, named for what it means.
    void fire(AnimatorId character, uint32_t parameter);

    // ------------------------------------------------------------------- evaluate

    /**
     * @brief Advance every character by `dt` and rebuild every world and joint matrix.
     *
     * **A delta rather than an absolute time**: a state machine has history, and one asked
     * to jump to an absolute time would have to replay every transition to get there.
     *
     * `dt` is the caller's fixed step, so frame N is reached by N identical additions and
     * is the same pose on every run -- which is what the golden images rest on.
     */
    void update(float dt);

    /// World transform per node of `character`, valid after update().
    [[nodiscard]] const std::vector<glm::mat4>& worldTransforms(AnimatorId character) const;
    /**
     * @brief Which character's pose animates `node`, or an invalid handle. Valid after
     *        `update()`, like the transforms it exists to pick between.
     *
     * A `ParticleEmitter::node` and an `AudioSourceDesc::node` are indices into the rig's
     * node array and carry no rig of their own -- the glTF `extras` schemas have nowhere to
     * say which, so with two rigs merged the index alone is ambiguous. Resolved from the
     * skins: a node belongs to the character whose skin lists it as a joint, and failing
     * that to the one whose current clip names it -- which is the *rigid* case, a drawbridge
     * or a clock tower merged beside a character rig.
     *
     * **The first such character, and that is what makes N copies of one rig agree.**
     * `spawnExtraCharacters` gives every copy the same skin, so the joint really is shared
     * between them and any answer but a stable one puts a torch on a different copy each
     * frame. A node nothing animates comes back invalid.
     */
    [[nodiscard]] AnimatorId characterForNode(uint32_t node) const;
    /// Morph weights of `character`, in the rig's flat per-node numbering.
    [[nodiscard]] const std::vector<float>& morphWeights(uint32_t slot) const;
    /// Joint matrices for `character`, laid out as the skinning shader indexes them.
    /// **Slot-based, not handle-based**: these serve the skinning upload, which walks
    /// `0..characterCount()` and whose instances name a slot through `GpuInstance::meta.w`.
    /// A retired slot still answers, with the identity matrices `destroy` left in it, so a
    /// stale `meta.w` draws a bind pose. Taking a handle here would map an invalid one to
    /// slot 0 and reintroduce the alias.
    [[nodiscard]] const std::vector<glm::mat4>& jointMatrices(uint32_t slot) const;
    /// Which skin `character` is driven by, for the influence array the dispatch reads.
    /// The rig every character here is posed from, read-only. Const because the animator
    /// owns it: `create` sizes a character's joint and weight blocks from it, so a caller
    /// that added a skin through a mutable reference would leave every existing block
    /// describing a rig that no longer exists. `merge` is the door for that (C22).
    [[nodiscard]] const AnimationRig& rig() const { return rigData; }

    [[nodiscard]] uint32_t skinOf(AnimatorId character) const;
    /// Joints across every character, which is what the GPU buffer has to hold.
    [[nodiscard]] uint32_t totalJoints() const { return jointTotal; }
    /// Where `character`'s matrices start in that buffer.
    [[nodiscard]] uint32_t jointOffset(uint32_t slot) const;
    /// Weights across every character, the same way. Zero for a rig with no morphs.
    [[nodiscard]] uint32_t totalWeights() const { return weightTotal; }
    [[nodiscard]] uint32_t weightOffset(uint32_t slot) const;

  private:
    /// One character: playback, state machine cursor, and the pose they produce.
    /// `previous` is a second playback rather than a frozen snapshot, which is what makes a
    /// fade *out of a moving clip* work.
    struct Character {
        /// Generation starts at 1 because `Handle::valid()` reserves 0 for "never issued".
        uint32_t generation = 1;
        bool live = true;
        uint32_t skin = 0;
        ClipPlayback current;
        ClipPlayback previous;
        /// Weight of `current`. 1.0 means no fade is in progress and `previous` is not
        /// evaluated at all.
        float fade = 1.0f;
        float fadeRate = 0.0f;

        uint32_t state = kAnyState;
        std::vector<float> parameters;
        /// **This character's machine, not the animator's** (C23). Two rigs in one scene
        /// want different clips, different parameters and different transitions, and the
        /// animator held one of these for every character until a second rig made that a
        /// limit rather than a simplification. `setStateMachine` with no character still
        /// writes it to all of them, which is what keeps a one-rig scene a one-call scene.
        AnimationStateMachine machine;
        /// Which node's translation this character's root motion comes out of, or
        /// `kNoNode`. Per character for the same reason as `machine`: a rig that calls its
        /// pelvis something else needs its own answer, not the last one anybody set.
        uint32_t rootMotionNode = scene::kNoNode;

        Pose pose;
        Pose scratch;
        /// Morph weights nothing in the rig can reach, for a character `createMorphed`
        /// made. Empty for every character a file produced, and that emptiness *is* the
        /// flag rather than a second field that could disagree with it.
        std::vector<float> held;
        std::vector<glm::mat4> world;
        std::vector<glm::mat4> joints;

        /// The delta the last update recorded, and the node's translation as of the
        /// previous update, which is what the delta is measured against.
        /// `hasPreviousRoot` keeps the first update after a clip change from reporting the
        /// whole distance from the origin as one step.
        glm::vec3 rootDelta{0.0f};
        glm::vec3 previousRoot{0.0f};
        bool hasPreviousRoot = false;

        /// Where this slot's matrices sit in the packed buffer, and how much room it has.
        /// **Both belong to the slot rather than to the character occupying it** -- the
        /// base can never move, because `GpuInstance::meta.w` names the slot and a stale
        /// one would otherwise read a different skeleton's matrices. See `destroy`.
        uint32_t jointBase = 0;
        uint32_t weightBase = 0;
        uint32_t jointCapacity = 0;
        uint32_t weightCapacity = 0;
    };

    /// Start `to` on `c`, moving whatever is playing into the fade-out slot.
    void beginFade(Character& c, const ClipPlayback& to, float duration);
    /// Test every transition out of `c`'s current state and take the **first** that holds
    /// -- the table's order is the priority. Trigger parameters that fired one are cleared
    /// here.
    void stepStateMachine(Character& c);
    /// Rebuild `world` and `joints` from `pose`.
    void resolve(Character& c);
    /// Take a slot with room for `joints` matrices and `weights` floats, reusing a retired
    /// one only when its block is big enough. Shared by `create` and `createMorphed`,
    /// which differ in exactly where the weight count comes from.
    AnimatorId createSlot(uint32_t skin, uint32_t weights);

    /// Put a character on the entry state of the machine it now holds. Shared by the two
    /// `setStateMachine` overloads so "installed for everybody" and "installed for one"
    /// cannot come to mean different things.
    static void enterMachine(Character& c);

    AnimationRig rigData;
    /// What a character created after `setStateMachine(machine)` starts with. The machine a
    /// character actually runs lives on the character (C23); this is the template, and it
    /// exists so installing a machine before any rig is loaded still works.
    AnimationStateMachine defaultMachine;
    std::vector<Character> characters;
    /// Slots whose character was destroyed, reusable by a skin that fits their block.
    std::vector<uint32_t> freeCharacterSlots;
    /// This update's events, and the scratch `crossedEvents` appends into. Members rather
    /// than locals so a frame allocates nothing.
    std::vector<FiredEvent> fired;
    std::vector<uint32_t> eventScratch;
    /// Slot of the character that animates each node, or `0xFFFFFFFF`. Rebuilt at the top of
    /// every `update` rather than kept in step by the three calls that could invalidate it --
    /// `create`, `destroy` and `merge` all move a node's owner, and one rebuild over the
    /// joints is cheaper than three dirty flags that have to agree.
    std::vector<uint32_t> nodeOwner;
    /// Which nodes `resolve` has already placed. A member so a local is not a heap
    /// allocation per character per fixed step.
    ///
    /// **Sharing one buffer across characters cannot alias, because it carries no length
    /// of its own.** `resolve` opens with `assign(c.pose.nodes.size(), false)`, setting
    /// both the length and every element from the character being resolved before the
    /// first read, and the loop that reads it is bounded by that same `nodes.size()`.
    std::vector<bool> resolvedNodes;
    /// The root node a character created after `setRootNode(node)` starts with, for the same
    /// reason `defaultMachine` exists.
    uint32_t defaultRootMotionNode = scene::kNoNode;

    uint32_t jointTotal = 0;
    uint32_t weightTotal = 0;

    /// Returned by every accessor asked about a character that does not exist. The
    /// renderer walks instance slots, and a slot whose character was never created should
    /// draw the bind pose rather than abort.
    static const std::vector<glm::mat4> kNoMatrices;
    static const std::vector<float> kNoWeights;
    /// Answered for `stateMachine(id)` on an id this animator does not hold. No states, so
    /// every walk over it terminates immediately rather than needing a null check.
    static const AnimationStateMachine kNoMachine;
};

} // namespace scene
