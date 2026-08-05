#pragma once

#include "core/Clip.h"
#include "scene/AnimationRig.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file engine/anim/SceneAnimator.h
 * @brief The half of animation that runs: playheads, cross-fades, state machines and the
 *        joint matrices they produce.
 *
 * What a glTF document authors -- clips, rigs, poses, skins and the machine's description --
 * is `scene/AnimationRig.h`, where the loaders can reach it.
 */
namespace anim {

/// One event crossed during an update.
struct FiredEvent {
    uint32_t character = 0;
    uint32_t clip = 0;
    /// Index into `SceneAnimator::clip(clip).events`.
    uint32_t event = 0;
};

/**
 * @brief Drives N characters over one shared rig and produces their joint matrices.
 *
 * **The joint blocks are laid end to end in one flat numbering** -- `jointOffset(c)` says
 * where character `c`'s begin -- because that numbering is the skinning dispatch's
 * `jointBase` push constant. A second layout has to be kept in step with the shader.
 */

class SceneAnimator {
  public:
    /// Sentinel for "no skin" -- a hierarchy whose clips move rigid placements.
    static constexpr uint32_t kNoSkin = scene::kNoSkin;
    /// "No such clip", returned by `findClip`.
    static constexpr uint32_t kNoClip = 0xFFFFFFFFu;

    /// Take ownership of the rig and create one character per skin, so character `i`
    /// drives skin `i` and an instance table recording a skin index already names the
    /// right character. More copies come from `create`.
    void init(scene::AnimationRig rig);

    /**
     * @brief Append a second rig, keeping every character already playing.
     *
     * @return the index the first appended skin landed at, which is what a caller adds to
     *         a `Placement::skin` from the same file. `kNoSkin` when `extra` has no skins.
     *
     * **Not `init`**, which clears `characters` and so destroys every character already
     * animating. Everything the appended rig names by index is renumbered here and nowhere
     * else -- node parents, a skin's joints, a channel's target, a node's first morph
     * weight -- and nothing already in the rig moves, so an `AnimatorId`, a root-motion
     * node, a clip index and a `GpuInstance::meta.w` from before the merge all survive it.
     */
    uint32_t merge(const scene::AnimationRig& extra);

    [[nodiscard]] bool empty() const { return rigData.clips.empty() && rigData.skins.empty(); }
    [[nodiscard]] size_t clipCount() const { return rigData.clips.size(); }
    [[nodiscard]] const scene::AnimationClip& clip(size_t i) const { return rigData.clips[i]; }
    /// Index of the clip named `name`, or `kNoClip`.
    [[nodiscard]] uint32_t findClip(const std::string& name) const;

    /// Add another character driven by `skin`, or by no skin at all -- a hierarchy whose
    /// clips move rigid placements has world transforms and no joints.
    /// @return an invalid handle if the skin does not exist.
    scene::AnimatorId create(uint32_t skin = kNoSkin);

    /**
     * @brief Add a character with no skeleton whose `targets` morph weights are the
     *        caller's to write -- the door a mesh made in code goes through.
     *
     * **These weights survive `update`.** A rig-driven pose is rebuilt from the bind pose
     * every step, which would resize away a block belonging to no node, so these are held
     * beside the pose and written back after sampling.
     *
     * @return an invalid handle for `targets == 0`.
     */
    scene::AnimatorId createMorphed(uint32_t targets);

    /// Write one of `createMorphed`'s weights. A `target` out of range is **ignored rather
    /// than clamped** -- a caller off by one wants to know.
    void setMorphWeight(scene::AnimatorId character, uint32_t target, float weight);
    [[nodiscard]] float morphWeight(scene::AnimatorId character, uint32_t target) const;

    /**
     * @brief Retire a character.
     *
     * **A dead character keeps its joint block, filled with identity, and keeps its base.**
     * A character's index crosses to the GPU in `GpuInstance::meta.w`, where a stale
     * reference cannot be made to fail: repacking the prefix sums would slide every later
     * character's matrices under an instance still naming this one, and reusing the block
     * for another skin would point it at a different skeleton.
     *
     * **The caller must clear or repoint any instance whose `meta.w` names this
     * character** -- `InstanceTable::setCharacter`.
     */
    void destroy(scene::AnimatorId id);

    [[nodiscard]] bool valid(scene::AnimatorId id) const {
        return id.valid() && id.index < characters.size() && characters[id.index].generation == id.generation &&
               characters[id.index].live;
    }

    /// The handle occupying a slot. Invalid for a retired slot.
    [[nodiscard]] scene::AnimatorId characterAt(uint32_t slot) const {
        if (slot >= characters.size() || !characters[slot].live) return {};
        return scene::AnimatorId{slot, characters[slot].generation};
    }

    /// Slots, not live characters -- what a walker pairs with `characterAt`, and what
    /// `meta.w` indexes.
    [[nodiscard]] uint32_t characterCount() const { return static_cast<uint32_t>(characters.size()); }

    /**
     * @brief Events crossed by the most recent `update`, refilled by each one.
     *
     * Only the *current* clip of each character fires; a clip fading out does not, or a
     * cross-fade would give one step two footsteps.
     */
    [[nodiscard]] const std::vector<FiredEvent>& firedEvents() const { return fired; }

    /**
     * @brief How far the clip moved the root during the last `update`, in the character's
     *        own space.
     *
     * **Zero unless `setRootNode` named a node.** Feeding it to the character controller
     * rather than to the transform is what keeps the motion colliding with the world.
     */
    [[nodiscard]] glm::vec3 rootMotion(scene::AnimatorId character) const;

    /**
     * @brief Which node's translation is root motion, and take it out of the pose.
     *
     * `kNoNode` -- the default -- disables it. Naming a node makes `update` record that
     * node's per-step translation delta and then **hold the node still**: reporting the
     * delta while leaving the node moving moves the character twice.
     *
     * **X and Z only.** The vertical axis is the bob of a walk and the crouch of an idle,
     * so the clip keeps it, in the pose and out of the delta. Holding it too stands the
     * character off the floor by however far its bind pose sits above its clips.
     */
    void setRootNode(uint32_t node);
    /// One character's root node, leaving every other character's alone.
    void setRootNode(scene::AnimatorId character, uint32_t node);
    [[nodiscard]] uint32_t rootNode(scene::AnimatorId character) const;
    /// The root node new characters are given. What `setRootNode(node)` last set.
    [[nodiscard]] uint32_t rootNode() const { return defaultRootMotionNode; }

    /**
     * @brief The node the file gave this name, or `kNoNode`.
     *
     * First match wins -- glTF does not require names to be unique.
     */
    [[nodiscard]] uint32_t findNode(std::string_view name) const;

    /// "No root node", and therefore no root motion.
    static constexpr uint32_t kNoNode = scene::kNoNode;

    /// Cross-fade `character` onto `clip` over `fade` seconds. A fade of zero cuts.
    /// **Playing the clip that is already playing is a no-op, not a restart** -- "keep
    /// walking" is called every frame by anything driving this from input.
    void play(scene::AnimatorId character, uint32_t clip, float fade = 0.0f,
              core::LoopMode loop = core::LoopMode::Loop, float speed = 1.0f);
    /// Restart the current clip from its first key, fade included -- what `play` refuses
    /// to do.
    void restart(scene::AnimatorId character);
    void setPlaying(scene::AnimatorId character, bool playing);
    void setSpeed(scene::AnimatorId character, float speed);
    /// The clip currently being faded *to*. Mid-fade the previous clip is still
    /// contributing.
    [[nodiscard]] uint32_t playingClip(scene::AnimatorId character) const;
    [[nodiscard]] float playingTime(scene::AnimatorId character) const;
    /// 1.0 when no fade is in progress.
    [[nodiscard]] float fadeWeight(scene::AnimatorId character) const;

    /**
     * @brief Install the machine every character runs.
     *
     * Characters that already exist are reset onto its entry state; ones created afterwards
     * start there. **The machine is copied per character, not shared**, so a later
     * `setStateMachine(character, ...)` reaches only the one it names.
     */
    void setStateMachine(scene::AnimationStateMachine machine);
    /// One character's machine. Resets that character onto the new machine's entry state
    /// and leaves every other character alone.
    void setStateMachine(scene::AnimatorId character, scene::AnimationStateMachine machine);
    /// This character's machine, or an empty one for an id the animator does not hold.
    [[nodiscard]] const scene::AnimationStateMachine& stateMachine(scene::AnimatorId character) const;
    /// The machine new characters are given. What `setStateMachine(machine)` last set.
    [[nodiscard]] const scene::AnimationStateMachine& stateMachine() const { return defaultMachine; }
    [[nodiscard]] uint32_t currentState(scene::AnimatorId character) const;
    void setParameter(scene::AnimatorId character, uint32_t parameter, float value);
    [[nodiscard]] float parameter(scene::AnimatorId character, uint32_t parameter) const;
    void fire(scene::AnimatorId character, uint32_t parameter);

    /**
     * @brief Advance every character by `dt` seconds and rebuild every world and joint
     *        matrix.
     *
     * `dt` must be the caller's fixed step: frame N is then reached by N identical
     * additions and is the same pose on every run, which is what the golden images rest on.
     */
    void update(float dt);

    /// World transform per node of `character`, valid after update().
    [[nodiscard]] const std::vector<glm::mat4>& worldTransforms(scene::AnimatorId character) const;
    /**
     * @brief Which character's pose animates `node`, or an invalid handle. Valid after
     *        `update()`, like the transforms it exists to pick between.
     *
     * Resolved from the skins, and **the first such character wins, which is what makes N
     * copies of one rig agree**: `spawnExtraCharacters` gives every copy the same skin, so
     * any answer but a stable one puts a torch on a different copy each frame.
     */
    [[nodiscard]] scene::AnimatorId characterForNode(uint32_t node) const;
    /// Morph weights of a slot, in the rig's flat per-node numbering.
    [[nodiscard]] const std::vector<float>& morphWeights(uint32_t slot) const;
    /// Joint matrices for a slot, laid out as the skinning shader indexes them.
    /// **Slot-based, not handle-based**: instances name a slot through
    /// `GpuInstance::meta.w`, and a retired slot still answers with the identity matrices
    /// `destroy` left in it. Taking a handle here would map an invalid one to slot 0.
    [[nodiscard]] const std::vector<glm::mat4>& jointMatrices(uint32_t slot) const;
    [[nodiscard]] const scene::AnimationRig& rig() const { return rigData; }

    [[nodiscard]] uint32_t skinOf(scene::AnimatorId character) const;
    /// Joints across every character -- what the GPU buffer has to hold.
    [[nodiscard]] uint32_t totalJoints() const { return jointTotal; }
    [[nodiscard]] uint32_t jointOffset(uint32_t slot) const;
    /// Weights across every character. Zero for a rig with no morphs.
    [[nodiscard]] uint32_t totalWeights() const { return weightTotal; }
    [[nodiscard]] uint32_t weightOffset(uint32_t slot) const;

  private:
    /// One character: playback, state machine cursor, and the pose they produce.
    struct Character {
        /// Starts at 1 because `Handle::valid()` reserves 0 for "never issued".
        uint32_t generation = 1;
        bool live = true;
        uint32_t skin = 0;
        core::ClipPlayback current;
        /// The clip being faded *out of*, still advancing, which is what makes a fade out
        /// of a moving clip work.
        core::ClipPlayback previous;
        /// Weight of `current`. 1.0 means no fade is in progress and `previous` is not
        /// evaluated at all.
        float fade = 1.0f;
        float fadeRate = 0.0f;

        uint32_t state = scene::kAnyState;
        std::vector<float> parameters;
        /// **This character's machine, not the animator's.** `setStateMachine` with no
        /// character writes it to all of them; anything else here reaches one.
        scene::AnimationStateMachine machine;
        uint32_t rootMotionNode = scene::kNoNode;

        scene::Pose pose;
        scene::Pose scratch;
        /// `createMorphed`'s weights, which nothing in the rig can reach. Empty for every
        /// character a file produced, and that emptiness *is* the flag -- a second field
        /// could disagree with it.
        std::vector<float> held;
        std::vector<glm::mat4> world;
        std::vector<glm::mat4> joints;

        /// `previousRoot` is what `rootDelta` is measured against; `hasPreviousRoot` keeps
        /// the first update after a clip change from reporting the whole distance from the
        /// origin as one step.
        glm::vec3 rootDelta{0.0f};
        glm::vec3 previousRoot{0.0f};
        bool hasPreviousRoot = false;

        /// **The bases belong to the slot, not to the character occupying it, and can
        /// never move**: `GpuInstance::meta.w` names the slot, so a stale one would
        /// otherwise read a different skeleton's matrices. See `destroy`.
        uint32_t jointBase = 0;
        uint32_t weightBase = 0;
        uint32_t jointCapacity = 0;
        uint32_t weightCapacity = 0;
    };

    void beginFade(Character& c, const core::ClipPlayback& to, float duration);
    /// Take the **first** transition out of `c`'s current state that holds -- the table's
    /// order is the priority. Trigger parameters that fired one are cleared here.
    void stepStateMachine(Character& c);
    void resolve(Character& c);
    /// Take a slot with room for `joints` matrices and `weights` floats, reusing a retired
    /// one only when its block is big enough.
    scene::AnimatorId createSlot(uint32_t skin, uint32_t weights);

    static void enterMachine(Character& c);

    scene::AnimationRig rigData;
    /// What a character created after `setStateMachine(machine)` starts with, so installing
    /// a machine before any rig is loaded still works.
    scene::AnimationStateMachine defaultMachine;
    std::vector<Character> characters;
    /// Slots whose character was destroyed, reusable by a skin that fits their block.
    std::vector<uint32_t> freeCharacterSlots;
    /// Members rather than locals so a frame allocates nothing.
    std::vector<FiredEvent> fired;
    std::vector<uint32_t> eventScratch;
    /// Slot of the character that animates each node, or `0xFFFFFFFF`. Rebuilt at the top of
    /// every `update`, because `create`, `destroy` and `merge` all move a node's owner.
    std::vector<uint32_t> nodeOwner;
    /// Which nodes `resolve` has already placed. **Sharing one buffer across characters
    /// cannot alias, because it carries no length of its own**: `resolve` opens with
    /// `assign(c.pose.nodes.size(), false)`, and the loop that reads it is bounded by that
    /// same size.
    std::vector<bool> resolvedNodes;
    /// The root node a character created after `setRootNode(node)` starts with.
    uint32_t defaultRootMotionNode = scene::kNoNode;

    uint32_t jointTotal = 0;
    uint32_t weightTotal = 0;

    /// Returned for a character that does not exist, so a slot the renderer walks draws its
    /// bind pose rather than aborting.
    static const std::vector<glm::mat4> kNoMatrices;
    static const std::vector<float> kNoWeights;
    /// No states, so every walk over it terminates immediately rather than needing a null
    /// check.
    static const scene::AnimationStateMachine kNoMachine;
};

} // namespace anim
