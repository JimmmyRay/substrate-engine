#include "anim/SceneAnimator.h"

#include "core/Profiler.h"

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>

namespace anim {

const std::vector<glm::mat4> SceneAnimator::kNoMatrices;
const std::vector<float> SceneAnimator::kNoWeights;
const scene::AnimationStateMachine SceneAnimator::kNoMachine;

void SceneAnimator::init(scene::AnimationRig r) {
    rigData = std::move(r);
    characters.clear();
    freeCharacterSlots.clear();
    jointTotal = 0;
    weightTotal = 0;

    // A rig with no skin still gets one character: a clip that drives *node* transforms
    // animates rigid placements, and a scene of moving crates has a hierarchy to resolve
    // and nothing to skin.
    if (rigData.skins.empty()) {
        create(kNoSkin);
    } else {
        for (uint32_t s = 0; s < static_cast<uint32_t>(rigData.skins.size()); ++s) create(s);
    }

    // A hierarchy with no clip still needs its world transforms: without this a skinned
    // mesh draws from whatever the joint buffer held until the first step.
    update(0.0f);
}

uint32_t SceneAnimator::merge(const scene::AnimationRig& extra) {
    const auto nodeBase = static_cast<uint32_t>(rigData.bind.nodes.size());
    const auto weightBase = static_cast<uint32_t>(rigData.bind.weights.size());
    const auto skinBase = static_cast<uint32_t>(rigData.skins.size());

    // The parent shift is signed because -1 is a root, and shifting one makes it a child
    // of an unrelated node of the base scene.
    rigData.bind.nodes.reserve(rigData.bind.nodes.size() + extra.bind.nodes.size());
    for (scene::SceneNode n : extra.bind.nodes) {
        if (n.parent >= 0) n.parent += static_cast<int32_t>(nodeBase);
        if (n.weightCount > 0) n.firstWeight += weightBase;
        rigData.bind.nodes.push_back(n);
    }
    rigData.bind.weights.insert(rigData.bind.weights.end(), extra.bind.weights.begin(), extra.bind.weights.end());

    // Kept parallel to `bind.nodes` whether or not the appended file named anything. A
    // short `nodeNames` silently makes `findNode` search a prefix, leaving every node past
    // it unnameable.
    rigData.nodeNames.resize(nodeBase);
    rigData.nodeNames.insert(rigData.nodeNames.end(), extra.nodeNames.begin(), extra.nodeNames.end());
    rigData.nodeNames.resize(rigData.bind.nodes.size());

    for (scene::Skin s : extra.skins) {
        for (uint32_t& joint : s.joints) joint += nodeBase;
        rigData.skins.push_back(std::move(s));
    }

    for (scene::AnimationClip c : extra.clips) {
        for (scene::AnimationChannel& ch : c.channels) ch.node += nodeBase;
        rigData.clips.push_back(std::move(c));
    }

    // One character per *appended* skin, and none for a skinless rig: the base scene
    // already has a character resolving the hierarchy these nodes joined, so a fallback
    // like `init`'s would be a second copy of the whole pose.
    for (uint32_t s = skinBase; s < static_cast<uint32_t>(rigData.skins.size()); ++s) create(s);

    // **Every existing character's `world` has to grow with the rig; its `pose` does not.**
    // `pose` is re-copied from `rigData.bind` at the top of every update and follows on its
    // own, but `world` is sized once by `createSlot` and written *by index* in `resolve`.
    // Left alone it is a heap overflow on the first step after an import.
    for (Character& c : characters) {
        if (c.world.size() < rigData.bind.nodes.size()) c.world.resize(rigData.bind.nodes.size(), glm::mat4(1.0f));
    }

    // Without this an instance drawn before the next step reads whatever the joint buffer
    // held rather than the appended bind pose.
    update(0.0f);

    return extra.skins.empty() ? kNoSkin : skinBase;
}

uint32_t SceneAnimator::findClip(const std::string& name) const {
    for (size_t i = 0; i < rigData.clips.size(); ++i) {
        if (rigData.clips[i].name == name) return static_cast<uint32_t>(i);
    }
    return kNoClip;
}

scene::AnimatorId SceneAnimator::create(uint32_t skin) {
    return createSlot(skin, static_cast<uint32_t>(rigData.bind.weights.size()));
}

scene::AnimatorId SceneAnimator::createMorphed(uint32_t targets) {
    // A valid handle to an empty block is a handle whose every `setMorphWeight` silently
    // does nothing.
    if (targets == 0) return {};

    const scene::AnimatorId id = createSlot(kNoSkin, targets);
    if (!id.valid()) return id;

    Character& c = characters[id.index];
    c.held.assign(targets, 0.0f);
    // Replaces whatever `rig.bind` sized the block to -- nothing at all, for a scene whose
    // glTF declares no morph target. `update` restores it from `held` after every sample
    // for the same reason.
    c.pose.weights = c.held;
    return id;
}

void SceneAnimator::setMorphWeight(scene::AnimatorId character, uint32_t target, float weight) {
    if (!valid(character)) return;
    Character& c = characters[character.index];
    if (target >= c.held.size()) return;
    c.held[target] = weight;
    // Written through to the pose as well, not only to `held`. The renderer uploads
    // `morphWeights` every frame whether or not `update` ran between the two, so writing
    // `held` alone makes a weight a frame late whenever a game draws more often than it
    // steps.
    if (target < c.pose.weights.size()) c.pose.weights[target] = weight;
}

float SceneAnimator::morphWeight(scene::AnimatorId character, uint32_t target) const {
    if (!valid(character)) return 0.0f;
    const Character& c = characters[character.index];
    return target < c.held.size() ? c.held[target] : 0.0f;
}

scene::AnimatorId SceneAnimator::createSlot(uint32_t skin, uint32_t weights) {
    // kNoSkin is the deliberate case -- a hierarchy with no skin. Any other out-of-range
    // value is a caller error that would silently index a skin later.
    if (skin != kNoSkin && skin >= rigData.skins.size()) return {};

    const auto joints = static_cast<uint32_t>(skin == kNoSkin ? 0u : rigData.skins[skin].joints.size());

    // A retired slot is reused only when its blocks are big enough for the new skin: the
    // block cannot move -- see destroy() -- so growing one here would slide every later
    // character's matrices under an instance still naming it.
    uint32_t slot = kNoSkin;
    for (size_t i = 0; i < freeCharacterSlots.size(); ++i) {
        const Character& dead = characters[freeCharacterSlots[i]];
        if (dead.jointCapacity < joints || dead.weightCapacity < weights) continue;
        slot = freeCharacterSlots[i];
        freeCharacterSlots.erase(freeCharacterSlots.begin() + static_cast<ptrdiff_t>(i));
        break;
    }

    Character c;
    c.skin = skin;
    c.pose = rigData.bind;
    c.scratch = rigData.bind;
    c.world.assign(rigData.bind.nodes.size(), glm::mat4(1.0f));
    if (skin != kNoSkin) c.joints.assign(rigData.skins[skin].joints.size(), glm::mat4(1.0f));
    // Seeded from the templates, so installing a machine before any rig is loaded still
    // reaches the characters that rig produces.
    c.machine = defaultMachine;
    c.rootMotionNode = defaultRootMotionNode;
    c.parameters.assign(c.machine.parameters.size(), 0.0f);
    if (!c.machine.states.empty()) {
        c.state = 0;
        c.current = {c.machine.states[0].clip, 0.0f, c.machine.states[0].speed, c.machine.states[0].loop, true};
    }

    if (slot != kNoSkin) {
        // The base and the block size are the slot's, not the character's, and outlive
        // every character that occupies it.
        c.generation = characters[slot].generation;
        c.jointBase = characters[slot].jointBase;
        c.weightBase = characters[slot].weightBase;
        c.jointCapacity = characters[slot].jointCapacity;
        c.weightCapacity = characters[slot].weightCapacity;
        characters[slot] = std::move(c);
        return scene::AnimatorId{slot, characters[slot].generation};
    }

    c.jointBase = jointTotal;
    c.weightBase = weightTotal;
    c.jointCapacity = joints;
    c.weightCapacity = weights;
    jointTotal += joints;
    weightTotal += weights;
    slot = static_cast<uint32_t>(characters.size());
    characters.push_back(std::move(c));
    return scene::AnimatorId{slot, characters[slot].generation};
}

void SceneAnimator::destroy(scene::AnimatorId id) {
    if (!valid(id)) return;

    Character& c = characters[id.index];
    c.live = false;
    ++c.generation;
    // Identity, not freed: an instance whose meta.w still names this slot draws its bind
    // pose rather than whatever the next character to reuse the block is doing.
    std::fill(c.joints.begin(), c.joints.end(), glm::mat4(1.0f));
    std::fill(c.pose.weights.begin(), c.pose.weights.end(), 0.0f);
    // Zeroed and kept at its length, not cleared. `update` skips a dead slot, so these are
    // the weights a stale instance goes on reading, and zero is the undeformed mesh.
    std::fill(c.held.begin(), c.held.end(), 0.0f);
    c.current = {};
    c.previous = {};
    c.fade = 1.0f;
    c.fadeRate = 0.0f;
    freeCharacterSlots.push_back(id.index);
}

void SceneAnimator::play(scene::AnimatorId character, uint32_t clip, float fade, core::LoopMode loop, float speed) {
    if (!valid(character) || clip >= rigData.clips.size()) return;
    Character& c = characters[character.index];
    // Already playing it, and not still fading out of something else: restarting here
    // would reset the clip every frame for anything driving this from held input.
    if (c.current.clip == clip && c.fade >= 1.0f) {
        c.current.loop = loop;
        c.current.speed = speed;
        return;
    }
    beginFade(c, core::ClipPlayback{clip, 0.0f, speed, loop, true}, fade);
}

void SceneAnimator::restart(scene::AnimatorId character) {
    if (!valid(character)) return;
    Character& c = characters[character.index];
    c.current.time = 0.0f;
    c.fade = 1.0f;
    c.fadeRate = 0.0f;
}

void SceneAnimator::setPlaying(scene::AnimatorId character, bool playing) {
    if (!valid(character)) return;
    characters[character.index].current.playing = playing;
    characters[character.index].previous.playing = playing;
}

void SceneAnimator::setSpeed(scene::AnimatorId character, float speed) {
    if (!valid(character)) return;
    characters[character.index].current.speed = speed;
}

uint32_t SceneAnimator::playingClip(scene::AnimatorId character) const {
    return valid(character) ? characters[character.index].current.clip : kNoClip;
}

float SceneAnimator::playingTime(scene::AnimatorId character) const {
    return valid(character) ? characters[character.index].current.time : 0.0f;
}

float SceneAnimator::fadeWeight(scene::AnimatorId character) const {
    return valid(character) ? characters[character.index].fade : 1.0f;
}

void SceneAnimator::enterMachine(Character& c) {
    c.parameters.assign(c.machine.parameters.size(), 0.0f);
    c.state = c.machine.states.empty() ? scene::kAnyState : 0;
    if (!c.machine.states.empty()) {
        const scene::AnimationState& s = c.machine.states[0];
        c.current = {s.clip, 0.0f, s.speed, s.loop, true};
        c.fade = 1.0f;
        c.fadeRate = 0.0f;
    }
}

void SceneAnimator::setStateMachine(scene::AnimationStateMachine m) {
    defaultMachine = std::move(m);
    for (Character& c : characters) {
        c.machine = defaultMachine;
        enterMachine(c);
    }
}

void SceneAnimator::setStateMachine(scene::AnimatorId character, scene::AnimationStateMachine m) {
    if (!valid(character)) return;
    Character& c = characters[character.index];
    c.machine = std::move(m);
    enterMachine(c);
}

const scene::AnimationStateMachine& SceneAnimator::stateMachine(scene::AnimatorId character) const {
    return valid(character) ? characters[character.index].machine : kNoMachine;
}

uint32_t SceneAnimator::currentState(scene::AnimatorId character) const {
    return valid(character) ? characters[character.index].state : scene::kAnyState;
}

void SceneAnimator::setParameter(scene::AnimatorId character, uint32_t param, float value) {
    if (!valid(character) || param >= characters[character.index].parameters.size()) return;
    characters[character.index].parameters[param] = value;
}

float SceneAnimator::parameter(scene::AnimatorId character, uint32_t param) const {
    if (!valid(character) || param >= characters[character.index].parameters.size()) return 0.0f;
    return characters[character.index].parameters[param];
}

void SceneAnimator::fire(scene::AnimatorId character, uint32_t param) { setParameter(character, param, 1.0f); }

void SceneAnimator::beginFade(Character& c, const core::ClipPlayback& to, float duration) {
    if (duration > 0.0f && c.fade >= 1.0f) {
        // Only a settled character fades from what it was playing: a fade interrupted
        // mid-way drops the clip it was fading out of, because blending from it would
        // need a third playback.
        c.previous = c.current;
        c.fade = 0.0f;
        c.fadeRate = 1.0f / duration;
    } else if (duration <= 0.0f) {
        c.fade = 1.0f;
        c.fadeRate = 0.0f;
    }
    c.current = to;
}

void SceneAnimator::stepStateMachine(Character& c) {
    const scene::AnimationStateMachine& machine = c.machine;
    if (machine.states.empty() || c.state >= machine.states.size()) return;

    const scene::AnimationState& from = machine.states[c.state];
    const bool finished = from.loop == core::LoopMode::ClampToEnd && c.current.clip < rigData.clips.size() &&
                          rigData.clips[c.current.clip].duration > 0.0f &&
                          c.current.time >= rigData.clips[c.current.clip].duration;

    for (const scene::AnimationTransition& t : machine.transitions) {
        if (t.from != scene::kAnyState && t.from != c.state) continue;
        if (t.to >= machine.states.size() || t.to == c.state) continue;
        if (t.waitForExit && !finished) continue;

        bool holds = true;
        for (const scene::AnimationCondition& cond : t.conditions) {
            if (cond.parameter >= c.parameters.size()) {
                holds = false;
                break;
            }
            const float v = c.parameters[cond.parameter];
            switch (cond.test) {
            case scene::ConditionTest::Greater: holds = v > cond.value; break;
            case scene::ConditionTest::Less: holds = v < cond.value; break;
            case scene::ConditionTest::Equal: holds = v == cond.value; break;
            case scene::ConditionTest::NotEqual: holds = v != cond.value; break;
            }
            if (!holds) break;
        }
        if (!holds) continue;

        // Consume every trigger this transition tested, whichever way it tested it.
        // Leaving one set fires the next transition that reads it in the same frame.
        for (const scene::AnimationCondition& cond : t.conditions) {
            if (cond.parameter < machine.parameters.size() && machine.parameters[cond.parameter].trigger) {
                c.parameters[cond.parameter] = 0.0f;
            }
        }

        const scene::AnimationState& to = machine.states[t.to];
        c.state = t.to;
        beginFade(c, core::ClipPlayback{to.clip, 0.0f, to.speed, to.loop, true}, t.duration);
        return;
    }
}

void SceneAnimator::resolve(Character& c) {
    // Parents before children, in repeated passes: glTF does not require the node array to
    // be topologically ordered, so a child may precede its parent. Every pass resolves at
    // least the shallowest unresolved node, which is what terminates it.
    const std::vector<scene::SceneNode>& nodes = c.pose.nodes;
    // `assign`, not `clear()` + `resize()`: it writes every element of [0, nodes.size()),
    // which is exactly the range the loop below reads, so no character can read a mark the
    // character before it left in this shared buffer.
    std::vector<bool>& done = resolvedNodes;
    done.assign(nodes.size(), false);
    size_t resolved = 0;
    while (resolved < nodes.size()) {
        const size_t before = resolved;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (done[i]) continue;
            const int32_t parent = nodes[i].parent;
            if (parent >= 0 && !done[static_cast<size_t>(parent)]) continue;

            c.world[i] = parent >= 0 ? c.world[static_cast<size_t>(parent)] * scene::localTransform(nodes[i])
                                     : scene::localTransform(nodes[i]);
            done[i] = true;
            ++resolved;
        }
        // A cycle in the parent links. Leaving the rest at identity is a visible wrong
        // answer; without the break it is an infinite loop.
        if (resolved == before) break;
    }

    if (c.skin >= rigData.skins.size()) return;
    const scene::Skin& skin = rigData.skins[c.skin];
    for (size_t j = 0; j < skin.joints.size(); ++j) {
        const uint32_t node = skin.joints[j];
        const glm::mat4 jointWorld = node < c.world.size() ? c.world[node] : glm::mat4(1.0f);
        c.joints[j] = jointWorld * skin.inverseBind[j];
    }
}

void SceneAnimator::update(float dt) {
    auto s = core::Profiler::scope("SceneAnimator::update");
    fired.clear();

    // Rebuilt before anything reads a pose off a node. First claim wins, which is what
    // keeps N copies of one skin agreeing on a shared joint.
    nodeOwner.assign(rigData.bind.nodes.size(), 0xFFFFFFFFu);
    for (uint32_t index = 0; index < static_cast<uint32_t>(characters.size()); ++index) {
        const Character& c = characters[index];
        if (!c.live || c.skin >= rigData.skins.size()) continue;
        for (const uint32_t joint : rigData.skins[c.skin].joints) {
            if (joint < nodeOwner.size() && nodeOwner[joint] == 0xFFFFFFFFu) nodeOwner[joint] = index;
        }
    }
    // **Skins first, then clips.** A node no skin claims is a *rigid* animated node -- a
    // drawbridge merged beside a character rig -- owned by whoever plays the clip naming
    // it. Running this pass first, or letting it overwrite a claim, takes a joint away
    // from its own skin because some clip happens to mention it.
    for (uint32_t index = 0; index < static_cast<uint32_t>(characters.size()); ++index) {
        const Character& c = characters[index];
        if (!c.live || c.current.clip >= rigData.clips.size()) continue;
        for (const scene::AnimationChannel& channel : rigData.clips[c.current.clip].channels) {
            if (channel.node < nodeOwner.size() && nodeOwner[channel.node] == 0xFFFFFFFFu) {
                nodeOwner[channel.node] = index;
            }
        }
    }

    for (uint32_t index = 0; index < static_cast<uint32_t>(characters.size()); ++index) {
        Character& c = characters[index];
        if (!c.live) continue;
        // Before the clips advance, not after: a state entered this frame then gets a full
        // step of its own clip instead of stalling for one, and a cut transition never
        // shows a frame of the pose it just left. The price is that `waitForExit` sees the
        // previous frame's time, so a clamped clip leaves one frame after it ends.
        stepStateMachine(c);

        if (c.fade < 1.0f) {
            c.fade = std::min(c.fade + c.fadeRate * dt, 1.0f);
            if (c.previous.clip < rigData.clips.size()) {
                scene::advance(c.previous, rigData.clips[c.previous.clip], dt);
            }
        }
        if (c.current.clip < rigData.clips.size()) {
            // Taken before the step: `crossedEvents` needs the interval, not the endpoint.
            const float from = c.current.time;
            scene::advance(c.current, rigData.clips[c.current.clip], dt);

            eventScratch.clear();
            scene::crossedEvents(c.current, rigData.clips[c.current.clip], from, eventScratch);
            for (const uint32_t e : eventScratch) fired.push_back({index, c.current.clip, e});
        }

        // Start from the bind pose every frame rather than accumulating: a clip that drives
        // rotation only would otherwise inherit whatever translation the previous clip left
        // behind, and the bug looks like a rig that drifts.
        c.pose = rigData.bind;
        if (!rigData.clips.empty()) {
            if (c.fade < 1.0f) {
                scene::sampleClip(rigData.clips[c.previous.clip], c.previous.time, c.pose);
                c.scratch = rigData.bind;
                scene::sampleClip(rigData.clips[c.current.clip], c.current.time, c.scratch);
                scene::blendPose(c.pose, c.scratch, c.fade);
            } else {
                scene::sampleClip(rigData.clips[c.current.clip], c.current.time, c.pose);
            }
        }

        // A character `createMorphed` made belongs to no node of the rig, so the copy of
        // `rig.bind` above has just resized its weight block to the file's -- zero, for a
        // scene whose glTF declares no morph target.
        if (!c.held.empty()) c.pose.weights = c.held;

        // After sampling and before resolve(), the only window where the pose holds this
        // step's authored root translation and nothing has consumed it yet.
        if (c.rootMotionNode < c.pose.nodes.size()) {
            const glm::vec3 sampled = c.pose.nodes[c.rootMotionNode].translation;
            c.rootDelta = c.hasPreviousRoot ? sampled - c.previousRoot : glm::vec3(0.0f);
            c.previousRoot = sampled;
            c.hasPreviousRoot = true;

            // **Horizontal only.** Holding Y as well pins the root at the height the
            // *bind* pose happened to use, and a rig binds in a T-pose while it animates
            // with bent knees -- hips bound at 1.043 against an idle between 0.946 and
            // 0.978 stands the character eight centimetres off the floor and flattens the
            // bob out of every walk cycle. The delta drops Y for the mirror reason: a
            // controller fed a vertical component fights the solver's gravity.
            glm::vec3 held = rigData.bind.nodes[c.rootMotionNode].translation;
            held.y = sampled.y;
            c.pose.nodes[c.rootMotionNode].translation = held;
            c.rootDelta.y = 0.0f;
        } else {
            c.rootDelta = glm::vec3(0.0f);
        }

        resolve(c);
    }
}

glm::vec3 SceneAnimator::rootMotion(scene::AnimatorId character) const {
    return valid(character) ? characters[character.index].rootDelta : glm::vec3(0.0f);
}

uint32_t SceneAnimator::findNode(std::string_view name) const {
    for (size_t i = 0; i < rigData.nodeNames.size(); ++i) {
        if (rigData.nodeNames[i] == name) return static_cast<uint32_t>(i);
    }
    return kNoNode;
}

void SceneAnimator::setRootNode(uint32_t node) {
    defaultRootMotionNode = node;
    // Every character restarts its measurement; a delta taken across a change of node is
    // the distance between two unrelated joints.
    for (Character& c : characters) {
        c.rootMotionNode = node;
        c.hasPreviousRoot = false;
        c.rootDelta = glm::vec3(0.0f);
    }
}

void SceneAnimator::setRootNode(scene::AnimatorId character, uint32_t node) {
    if (!valid(character)) return;
    Character& c = characters[character.index];
    // Guarded: without it a game that sets the same root node every frame restarts the
    // measurement every frame and reads a delta of zero for ever.
    if (c.rootMotionNode == node) return;
    c.rootMotionNode = node;
    c.hasPreviousRoot = false;
    c.rootDelta = glm::vec3(0.0f);
}

uint32_t SceneAnimator::rootNode(scene::AnimatorId character) const {
    return valid(character) ? characters[character.index].rootMotionNode : kNoNode;
}

const std::vector<glm::mat4>& SceneAnimator::worldTransforms(scene::AnimatorId character) const {
    return valid(character) ? characters[character.index].world : kNoMatrices;
}

scene::AnimatorId SceneAnimator::characterForNode(uint32_t node) const {
    if (node >= nodeOwner.size()) return {};
    const uint32_t slot = nodeOwner[node];
    if (slot >= characters.size() || !characters[slot].live) return {};
    return scene::AnimatorId{slot, characters[slot].generation};
}

const std::vector<float>& SceneAnimator::morphWeights(uint32_t slot) const {
    return slot < characters.size() ? characters[slot].pose.weights : kNoWeights;
}

const std::vector<glm::mat4>& SceneAnimator::jointMatrices(uint32_t slot) const {
    return slot < characters.size() ? characters[slot].joints : kNoMatrices;
}

uint32_t SceneAnimator::skinOf(scene::AnimatorId character) const {
    return valid(character) ? characters[character.index].skin : kNoSkin;
}

uint32_t SceneAnimator::jointOffset(uint32_t slot) const {
    return slot < characters.size() ? characters[slot].jointBase : 0u;
}

uint32_t SceneAnimator::weightOffset(uint32_t slot) const {
    return slot < characters.size() ? characters[slot].weightBase : 0u;
}

} // namespace anim
