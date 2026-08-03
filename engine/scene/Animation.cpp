#include "scene/Animation.h"

#include "core/Profiler.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace scene {

namespace {

/// Index of the last key at or before `t`, plus the fraction to the next one. Linear
/// search from the front would be O(keys) per channel per frame; this is the standard
/// binary search, and it matters the moment a clip has a few hundred keys.
struct KeyLookup {
    size_t index = 0;
    float alpha = 0.0f;
    bool single = false;
};

KeyLookup findKey(const std::vector<float>& times, float t) {
    KeyLookup out;
    if (times.size() < 2) {
        out.single = true;
        return out;
    }
    // Before the first key or after the last: hold the endpoint rather than
    // extrapolating. A clip whose channels start at different times is legal glTF and
    // extrapolating the earlier ones is how a rig ends up inside out for one frame.
    if (t <= times.front()) {
        out.single = true;
        return out;
    }
    if (t >= times.back()) {
        out.index = times.size() - 1;
        out.single = true;
        return out;
    }

    const auto it = std::upper_bound(times.begin(), times.end(), t);
    out.index = static_cast<size_t>(it - times.begin()) - 1;
    const float t0 = times[out.index];
    const float t1 = times[out.index + 1];
    out.alpha = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f;
    return out;
}

/// Hermite basis, per the glTF spec's CUBICSPLINE definition. `values` holds
/// in-tangent, point, out-tangent for each key, so the point for key k is at 3k+1.
glm::vec4 cubicSpline(const AnimationSampler& s, size_t k, float alpha, float dt) {
    const glm::vec4 p0 = s.values[3 * k + 1];
    const glm::vec4 m0 = s.values[3 * k + 2] * dt;
    const glm::vec4 p1 = s.values[3 * (k + 1) + 1];
    const glm::vec4 m1 = s.values[3 * (k + 1)] * dt;

    const float a2 = alpha * alpha;
    const float a3 = a2 * alpha;
    return (2.0f * a3 - 3.0f * a2 + 1.0f) * p0 + (a3 - 2.0f * a2 + alpha) * m0 +
           (-2.0f * a3 + 3.0f * a2) * p1 + (a3 - a2) * m1;
}

glm::vec4 sampleValue(const AnimationSampler& s, float t, bool isRotation) {
    const bool cubic = s.interpolation == AnimationInterpolation::CubicSpline;
    const size_t stride = cubic ? 3 : 1;
    const size_t offset = cubic ? 1 : 0;

    if (s.times.empty() || s.values.size() < stride) return glm::vec4(0.0f);

    const KeyLookup key = findKey(s.times, t);
    if (key.single) return s.values[key.index * stride + offset];

    if (s.interpolation == AnimationInterpolation::Step) {
        return s.values[key.index * stride + offset];
    }
    if (cubic) {
        const float dt = s.times[key.index + 1] - s.times[key.index];
        const glm::vec4 v = cubicSpline(s, key.index, key.alpha, dt);
        // Normalising is not optional for a rotation: a Hermite interpolation of four
        // quaternion components leaves the result off the unit sphere, and a non-unit
        // quaternion in a joint matrix is a scale nobody authored.
        if (!isRotation) return v;
        const float len = glm::length(v);
        return len > 0.0f ? v / len : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const glm::vec4 a = s.values[key.index];
    const glm::vec4 b = s.values[key.index + 1];
    if (isRotation) {
        // Spherical, not linear. A lerp between two quaternions takes the chord rather
        // than the arc, which shows up as a joint speeding up through the middle of
        // every rotation and slowing at the ends.
        const glm::quat qa(a.w, a.x, a.y, a.z);
        const glm::quat qb(b.w, b.x, b.y, b.z);
        const glm::quat q = glm::slerp(qa, qb, key.alpha);
        return glm::vec4(q.x, q.y, q.z, q.w);
    }
    return glm::mix(a, b, key.alpha);
}

/**
 * @brief Sample a morph-weight channel into `out`, which is `count` floats long (S2.1).
 *
 * Separate from sampleValue() rather than parameterised on a stride, because the two
 * differ in what a keyframe *is*: a TRS key is one value and this is `count` of them,
 * so every index in here is a product where the other has a sum. Merging them means a
 * function whose every line carries a stride that one caller always sets to one.
 */
void sampleWeights(const AnimationSampler& s, float t, uint32_t count, float* out) {
    const bool cubic = s.interpolation == AnimationInterpolation::CubicSpline;
    const size_t group = cubic ? 3 * s.stride : s.stride;
    const size_t offset = cubic ? s.stride : 0;
    const uint32_t n = std::min(count, s.stride);

    if (s.times.empty() || s.weights.size() < group || n == 0) return;

    const KeyLookup key = findKey(s.times, t);
    const size_t a = key.index * group + offset;

    if (key.single || s.interpolation == AnimationInterpolation::Step) {
        for (uint32_t i = 0; i < n; ++i) out[i] = s.weights[a + i];
        return;
    }
    if (cubic) {
        const float dt = s.times[key.index + 1] - s.times[key.index];
        const float a2 = key.alpha * key.alpha;
        const float a3 = a2 * key.alpha;
        const size_t next = (key.index + 1) * group;
        for (uint32_t i = 0; i < n; ++i) {
            const float p0 = s.weights[a + i];
            const float m0 = s.weights[a + s.stride + i] * dt;
            const float p1 = s.weights[next + s.stride + i];
            const float m1 = s.weights[next + i] * dt;
            out[i] = (2.0f * a3 - 3.0f * a2 + 1.0f) * p0 + (a3 - 2.0f * a2 + key.alpha) * m0 +
                     (-2.0f * a3 + 3.0f * a2) * p1 + (a3 - a2) * m1;
        }
        return;
    }
    const size_t b = (key.index + 1) * group + offset;
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = s.weights[a + i] + (s.weights[b + i] - s.weights[a + i]) * key.alpha;
    }
}

} // namespace

const std::vector<glm::mat4> SceneAnimator::kNoMatrices;
const std::vector<float> SceneAnimator::kNoWeights;
const AnimationStateMachine SceneAnimator::kNoMachine;

glm::mat4 localTransform(const SceneNode& n) {
    return glm::translate(glm::mat4(1.0f), n.translation) * glm::mat4_cast(n.rotation) *
           glm::scale(glm::mat4(1.0f), n.scale);
}

void sampleClip(const AnimationClip& clip, float time, Pose& pose) {
    /**
     * **Strictly greater than, and that `>` is a one-frame pop in every clip that ends.**
     * `fmod(d, d)` is 0, so a time of exactly the duration used to wrap to the first
     * keyframe -- and a `ClampToEnd` playback sits on exactly the duration for as long as
     * it holds its last pose. The showcase's 0.25 s `jumping up` therefore snapped the hips
     * 2.8 cm up on the single frame the jump ended, before the fall blend took over.
     *
     * A looping clip cannot tell the difference: `advance` has already wrapped it into
     * [0, duration), so it never arrives here at the duration. Anything that does arrive
     * there means the end of the clip and gets it.
     */
    const float raw = std::max(time, 0.0f);
    const float t = clip.duration <= 0.0f ? 0.0f : (raw > clip.duration ? std::fmod(raw, clip.duration) : raw);

    for (const AnimationChannel& ch : clip.channels) {
        if (ch.node >= pose.nodes.size() || ch.sampler >= clip.samplers.size()) continue;

        const AnimationSampler& s = clip.samplers[ch.sampler];
        SceneNode& n = pose.nodes[ch.node];

        if (ch.path == AnimationPath::Weights) {
            // Bounds are the node's, not the sampler's: a channel claiming more targets
            // than the mesh has would otherwise write past the pose's weight array, and
            // a malformed file is not a reason to corrupt the next node's face.
            if (n.weightCount == 0 || n.firstWeight + n.weightCount > pose.weights.size()) continue;
            sampleWeights(s, t, n.weightCount, &pose.weights[n.firstWeight]);
            continue;
        }

        const glm::vec4 v = sampleValue(s, t, ch.path == AnimationPath::Rotation);
        switch (ch.path) {
        case AnimationPath::Translation: n.translation = glm::vec3(v); break;
        case AnimationPath::Rotation: n.rotation = glm::quat(v.w, v.x, v.y, v.z); break;
        case AnimationPath::Scale: n.scale = glm::vec3(v); break;
        case AnimationPath::Weights: break; // handled above
        }
    }
}

void blendPose(Pose& dst, const Pose& src, float t) {
    const float k = std::clamp(t, 0.0f, 1.0f);
    if (k <= 0.0f) return;

    const size_t nodes = std::min(dst.nodes.size(), src.nodes.size());
    for (size_t i = 0; i < nodes; ++i) {
        SceneNode& a = dst.nodes[i];
        const SceneNode& b = src.nodes[i];
        a.translation = glm::mix(a.translation, b.translation, k);
        a.scale = glm::mix(a.scale, b.scale, k);
        // Spherical, and renormalised. glm::slerp negates the target when the two are
        // in opposite hemispheres, so the blend takes the short arc -- without that a
        // joint goes the long way round on every second transition, which reads as an
        // arm swinging through the torso.
        a.rotation = glm::normalize(glm::slerp(a.rotation, b.rotation, k));
    }

    const size_t weights = std::min(dst.weights.size(), src.weights.size());
    for (size_t i = 0; i < weights; ++i) dst.weights[i] += (src.weights[i] - dst.weights[i]) * k;
}

bool advance(ClipPlayback& p, const AnimationClip& clip, float dt) { return advance(p, clip.duration, dt); }

bool advance(ClipPlayback& p, float duration, float dt) {
    if (p.playing) p.time += dt * p.speed;

    if (duration <= 0.0f) {
        p.time = 0.0f;
        return p.loop == LoopMode::ClampToEnd;
    }

    if (p.loop == LoopMode::Loop) {
        // fmod of a negative time is negative, so a clip played backwards would walk
        // off the front. One add brings it back into range, which is all it needs:
        // |fmod| is strictly less than the duration.
        p.time = std::fmod(p.time, duration);
        if (p.time < 0.0f) p.time += duration;
        return false;
    }

    p.time = std::clamp(p.time, 0.0f, duration);
    // Reverse playback finishes at the *start*, which is the only sense in which a
    // clamped clip run backwards can be said to have ended.
    return p.speed < 0.0f ? p.time <= 0.0f : p.time >= duration;
}

void crossedEvents(const ClipPlayback& p, const AnimationClip& clip, float from, std::vector<uint32_t>& out) {
    crossedEvents(p, clip.events, clip.duration, from, out);
}

void crossedEvents(const ClipPlayback& p, const std::vector<AnimationEvent>& events, float duration, float from,
                   std::vector<uint32_t>& out) {
    if (events.empty() || !p.playing) return;

    const float to = p.time;
    const bool reverse = p.speed < 0.0f;

    // Half-open on the side the playhead came *from*, so an event exactly under the
    // playhead fires once as it is reached and not again on the next frame.
    const auto fireBetween = [&](float lo, float hi) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(events.size()); ++i) {
            const float t = events[i].time;
            if (t > lo && t <= hi) out.push_back(i);
        }
    };

    if (duration > 0.0f && p.loop == LoopMode::Loop) {
        // `advance` has already wrapped `p.time`, so a step that crossed the end shows up
        // as the new time being *behind* the old one. Two intervals then, and each event
        // still fires at most once because the two do not overlap.
        const bool wrapped = reverse ? to > from : to < from;
        if (!wrapped) {
            reverse ? fireBetween(to, from) : fireBetween(from, to);
            return;
        }
        if (reverse) {
            fireBetween(-1.0f, from);
            fireBetween(to, duration);
        } else {
            fireBetween(from, duration);
            fireBetween(-1.0f, to);
        }
        return;
    }

    reverse ? fireBetween(to, from) : fireBetween(from, to);
}

uint32_t AnimationStateMachine::findState(const std::string& name) const {
    for (size_t i = 0; i < states.size(); ++i) {
        if (states[i].name == name) return static_cast<uint32_t>(i);
    }
    return kAnyState;
}

uint32_t AnimationStateMachine::findParameter(const std::string& name) const {
    for (size_t i = 0; i < parameters.size(); ++i) {
        if (parameters[i].name == name) return static_cast<uint32_t>(i);
    }
    return kAnyState;
}

// --------------------------------------------------------------------- SceneAnimator

void SceneAnimator::init(AnimationRig r) {
    rigData = std::move(r);
    characters.clear();
    freeCharacterSlots.clear();
    jointTotal = 0;
    weightTotal = 0;

    // One character per skin, which is what keeps a plain glTF scene behaving exactly
    // as it did before S2 -- see the header. A rig with no skin still gets one: a clip
    // that drives *node* transforms animates rigid placements, and a scene of moving
    // crates has a hierarchy to resolve and nothing to skin.
    if (rigData.skins.empty()) {
        create(kNoSkin);
    } else {
        for (uint32_t s = 0; s < static_cast<uint32_t>(rigData.skins.size()); ++s) create(s);
    }

    // A hierarchy with no clip still needs its world transforms: the bind pose is what
    // a skinned mesh draws before anything animates it.
    update(0.0f);
}

uint32_t SceneAnimator::merge(const AnimationRig& extra) {
    const auto nodeBase = static_cast<uint32_t>(rigData.bind.nodes.size());
    const auto weightBase = static_cast<uint32_t>(rigData.bind.weights.size());
    const auto skinBase = static_cast<uint32_t>(rigData.skins.size());

    // Nodes first, and the parent shift is signed because -1 is a root. A root of the
    // appended file stays a root: grafting it onto a node of the base scene would be a
    // decision about *content*, and the caller's transform has already placed it.
    rigData.bind.nodes.reserve(rigData.bind.nodes.size() + extra.bind.nodes.size());
    for (SceneNode n : extra.bind.nodes) {
        if (n.parent >= 0) n.parent += static_cast<int32_t>(nodeBase);
        if (n.weightCount > 0) n.firstWeight += weightBase;
        rigData.bind.nodes.push_back(n);
    }
    rigData.bind.weights.insert(rigData.bind.weights.end(), extra.bind.weights.begin(), extra.bind.weights.end());

    // Kept parallel to `bind.nodes` whether or not the appended file named anything. A
    // short `nodeNames` makes `findNode` search a prefix and `setRootNode` unreachable for
    // every node past it -- and the two vectors go out of step silently, which is the
    // failure this whole card is about one array along.
    rigData.nodeNames.resize(nodeBase);
    rigData.nodeNames.insert(rigData.nodeNames.end(), extra.nodeNames.begin(), extra.nodeNames.end());
    rigData.nodeNames.resize(rigData.bind.nodes.size());

    for (Skin s : extra.skins) {
        for (uint32_t& joint : s.joints) joint += nodeBase;
        rigData.skins.push_back(std::move(s));
    }

    for (AnimationClip c : extra.clips) {
        for (AnimationChannel& ch : c.channels) ch.node += nodeBase;
        rigData.clips.push_back(std::move(c));
    }

    // A rig with no skin gets no character here, unlike `init`. `init`'s lone character
    // exists so a scene of animated crates has a hierarchy to resolve; the base scene
    // already has one, and a second would be a second copy of the whole pose for a file
    // that added nodes to a hierarchy somebody is already resolving.
    for (uint32_t s = skinBase; s < static_cast<uint32_t>(rigData.skins.size()); ++s) create(s);

    // **Every existing character's `world` has to grow with the rig, and its `pose` does
    // not.** `pose` is re-copied from `rigData.bind` at the top of every update, so it
    // follows on its own; `world` is sized once by `createSlot` and written *by index* in
    // `resolve`. Left alone it is a heap overflow on the first step after an import, on
    // whichever character was created first -- which is the base scene's.
    //
    // Found by ASan on the first run of the first test in this file, which is exactly the
    // reason the card asked for hosted cases over the offset arithmetic before the merge
    // existed: nothing in the golden set or the demo would have shown it as anything but a
    // device loss somewhere else.
    for (Character& c : characters) {
        if (c.world.size() < rigData.bind.nodes.size()) c.world.resize(rigData.bind.nodes.size(), glm::mat4(1.0f));
    }

    // Resolving now rather than waiting for the next step is what makes an instance drawn
    // before that step draw the appended bind pose instead of whatever the buffer held.
    update(0.0f);

    return extra.skins.empty() ? kNoSkin : skinBase;
}

uint32_t SceneAnimator::findClip(const std::string& name) const {
    for (size_t i = 0; i < rigData.clips.size(); ++i) {
        if (rigData.clips[i].name == name) return static_cast<uint32_t>(i);
    }
    return kNoClip;
}

AnimatorId SceneAnimator::create(uint32_t skin) {
    return createSlot(skin, static_cast<uint32_t>(rigData.bind.weights.size()));
}

AnimatorId SceneAnimator::createMorphed(uint32_t targets) {
    // No targets is no character. A caller with a mesh that has none has nothing to drive,
    // and handing back a valid handle to an empty block would be a handle whose every
    // `setMorphWeight` silently did nothing.
    if (targets == 0) return {};

    const AnimatorId id = createSlot(kNoSkin, targets);
    if (!id.valid()) return id;

    Character& c = characters[id.index];
    c.held.assign(targets, 0.0f);
    // The pose's own block, replacing whatever `rig.bind` sized it to -- which for a scene
    // with no morph target in its glTF is nothing at all. `update` restores it from `held`
    // after every sample for the same reason.
    c.pose.weights = c.held;
    return id;
}

void SceneAnimator::setMorphWeight(AnimatorId character, uint32_t target, float weight) {
    if (!valid(character)) return;
    Character& c = characters[character.index];
    if (target >= c.held.size()) return;
    c.held[target] = weight;
    // Written through to the pose as well, not only to `held`. The renderer uploads
    // `morphWeights` every frame whether or not `update` ran between the two, so a weight
    // set outside the fixed step would otherwise be a frame late exactly when a game
    // stepped the animator less often than it drew.
    if (target < c.pose.weights.size()) c.pose.weights[target] = weight;
}

float SceneAnimator::morphWeight(AnimatorId character, uint32_t target) const {
    if (!valid(character)) return 0.0f;
    const Character& c = characters[character.index];
    return target < c.held.size() ? c.held[target] : 0.0f;
}

AnimatorId SceneAnimator::createSlot(uint32_t skin, uint32_t weights) {
    // kNoSkin is the deliberate case -- a hierarchy with no skin -- and any other
    // out-of-range value is a caller error that would silently index a skin later.
    if (skin != kNoSkin && skin >= rigData.skins.size()) return {};

    const auto joints = static_cast<uint32_t>(skin == kNoSkin ? 0u : rigData.skins[skin].joints.size());

    // A retired slot is reused only when its joint and weight blocks are big enough for
    // the new skin. The block cannot move -- see destroy() -- so a skin that does not fit
    // takes a fresh slot and leaves this one for a later one that does.
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
    // reaches the characters that rig produces (C23).
    c.machine = defaultMachine;
    c.rootMotionNode = defaultRootMotionNode;
    c.parameters.assign(c.machine.parameters.size(), 0.0f);
    if (!c.machine.states.empty()) {
        c.state = 0;
        c.current = {c.machine.states[0].clip, 0.0f, c.machine.states[0].speed, c.machine.states[0].loop, true};
    }

    if (slot != kNoSkin) {
        // The base and the block size are the slot's, not the character's: they outlive
        // every character that ever occupies it.
        c.generation = characters[slot].generation;
        c.jointBase = characters[slot].jointBase;
        c.weightBase = characters[slot].weightBase;
        c.jointCapacity = characters[slot].jointCapacity;
        c.weightCapacity = characters[slot].weightCapacity;
        characters[slot] = std::move(c);
        return AnimatorId{slot, characters[slot].generation};
    }

    c.jointBase = jointTotal;
    c.weightBase = weightTotal;
    c.jointCapacity = joints;
    c.weightCapacity = weights;
    jointTotal += joints;
    weightTotal += weights;
    slot = static_cast<uint32_t>(characters.size());
    characters.push_back(std::move(c));
    return AnimatorId{slot, characters[slot].generation};
}

void SceneAnimator::destroy(AnimatorId id) {
    if (!valid(id)) return;

    Character& c = characters[id.index];
    c.live = false;
    ++c.generation;
    // Identity, not freed. An instance whose meta.w still names this slot draws its bind
    // pose rather than whatever the next character to reuse the block is doing -- see the
    // header, which is where this decision is argued.
    std::fill(c.joints.begin(), c.joints.end(), glm::mat4(1.0f));
    std::fill(c.pose.weights.begin(), c.pose.weights.end(), 0.0f);
    // Zeroed and kept at its length, not cleared. `update` skips a dead slot entirely, so
    // these are the weights a stale instance goes on reading -- and zero is the undeformed
    // mesh, which is the morph half of the bind pose `joints` was just filled with.
    std::fill(c.held.begin(), c.held.end(), 0.0f);
    c.current = {};
    c.previous = {};
    c.fade = 1.0f;
    c.fadeRate = 0.0f;
    freeCharacterSlots.push_back(id.index);
}

void SceneAnimator::play(AnimatorId character, uint32_t clip, float fade, LoopMode loop, float speed) {
    if (!valid(character) || clip >= rigData.clips.size()) return;
    Character& c = characters[character.index];
    // Already playing it, and not still fading out of something else: nothing to do.
    // Anything driving this from held input calls it every frame.
    if (c.current.clip == clip && c.fade >= 1.0f) {
        c.current.loop = loop;
        c.current.speed = speed;
        return;
    }
    beginFade(c, ClipPlayback{clip, 0.0f, speed, loop, true}, fade);
}

void SceneAnimator::restart(AnimatorId character) {
    if (!valid(character)) return;
    Character& c = characters[character.index];
    c.current.time = 0.0f;
    c.fade = 1.0f;
    c.fadeRate = 0.0f;
}

void SceneAnimator::setPlaying(AnimatorId character, bool playing) {
    if (!valid(character)) return;
    characters[character.index].current.playing = playing;
    characters[character.index].previous.playing = playing;
}

void SceneAnimator::setSpeed(AnimatorId character, float speed) {
    if (!valid(character)) return;
    characters[character.index].current.speed = speed;
}

uint32_t SceneAnimator::playingClip(AnimatorId character) const {
    return valid(character) ? characters[character.index].current.clip : kNoClip;
}

float SceneAnimator::playingTime(AnimatorId character) const {
    return valid(character) ? characters[character.index].current.time : 0.0f;
}

float SceneAnimator::fadeWeight(AnimatorId character) const {
    return valid(character) ? characters[character.index].fade : 1.0f;
}

void SceneAnimator::enterMachine(Character& c) {
    c.parameters.assign(c.machine.parameters.size(), 0.0f);
    c.state = c.machine.states.empty() ? kAnyState : 0;
    if (!c.machine.states.empty()) {
        const AnimationState& s = c.machine.states[0];
        c.current = {s.clip, 0.0f, s.speed, s.loop, true};
        c.fade = 1.0f;
        c.fadeRate = 0.0f;
    }
}

void SceneAnimator::setStateMachine(AnimationStateMachine m) {
    defaultMachine = std::move(m);
    for (Character& c : characters) {
        c.machine = defaultMachine;
        enterMachine(c);
    }
}

void SceneAnimator::setStateMachine(AnimatorId character, AnimationStateMachine m) {
    if (!valid(character)) return;
    Character& c = characters[character.index];
    c.machine = std::move(m);
    enterMachine(c);
}

const AnimationStateMachine& SceneAnimator::stateMachine(AnimatorId character) const {
    return valid(character) ? characters[character.index].machine : kNoMachine;
}

uint32_t SceneAnimator::currentState(AnimatorId character) const {
    return valid(character) ? characters[character.index].state : kAnyState;
}

void SceneAnimator::setParameter(AnimatorId character, uint32_t param, float value) {
    if (!valid(character) || param >= characters[character.index].parameters.size()) return;
    characters[character.index].parameters[param] = value;
}

float SceneAnimator::parameter(AnimatorId character, uint32_t param) const {
    if (!valid(character) || param >= characters[character.index].parameters.size()) return 0.0f;
    return characters[character.index].parameters[param];
}

void SceneAnimator::fire(AnimatorId character, uint32_t param) { setParameter(character, param, 1.0f); }

void SceneAnimator::beginFade(Character& c, const ClipPlayback& to, float duration) {
    if (duration > 0.0f && c.fade >= 1.0f) {
        // Only a settled character fades from what it was playing. Interrupting a fade
        // that is already running would need a third playback to blend from, and the
        // honest cheap answer is to drop the one being faded out -- the alternative,
        // an unbounded stack of them, is a blend tree in disguise.
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
    const AnimationStateMachine& machine = c.machine;
    if (machine.states.empty() || c.state >= machine.states.size()) return;

    const AnimationState& from = machine.states[c.state];
    const bool finished = from.loop == LoopMode::ClampToEnd && c.current.clip < rigData.clips.size() &&
                          rigData.clips[c.current.clip].duration > 0.0f &&
                          c.current.time >= rigData.clips[c.current.clip].duration;

    for (const AnimationTransition& t : machine.transitions) {
        if (t.from != kAnyState && t.from != c.state) continue;
        if (t.to >= machine.states.size() || t.to == c.state) continue;
        if (t.waitForExit && !finished) continue;

        bool holds = true;
        for (const AnimationCondition& cond : t.conditions) {
            if (cond.parameter >= c.parameters.size()) {
                holds = false;
                break;
            }
            const float v = c.parameters[cond.parameter];
            switch (cond.test) {
            case ConditionTest::Greater: holds = v > cond.value; break;
            case ConditionTest::Less: holds = v < cond.value; break;
            case ConditionTest::Equal: holds = v == cond.value; break;
            case ConditionTest::NotEqual: holds = v != cond.value; break;
            }
            if (!holds) break;
        }
        if (!holds) continue;

        // Consume every trigger this transition tested, whichever way it tested it: a
        // trigger a transition looked at has been acted on, and leaving it set fires
        // the next transition that reads it in the same frame.
        for (const AnimationCondition& cond : t.conditions) {
            if (cond.parameter < machine.parameters.size() && machine.parameters[cond.parameter].trigger) {
                c.parameters[cond.parameter] = 0.0f;
            }
        }

        const AnimationState& to = machine.states[t.to];
        c.state = t.to;
        beginFade(c, ClipPlayback{to.clip, 0.0f, to.speed, to.loop, true}, t.duration);
        return;
    }
}

void SceneAnimator::resolve(Character& c) {
    // Parents before children. glTF does not require the node array to be topologically
    // ordered, so a child may precede its parent -- the loop repeats until nothing is
    // left, which terminates because every pass resolves at least the shallowest
    // unresolved node.
    const std::vector<SceneNode>& nodes = c.pose.nodes;
    // Reused across characters and across steps rather than allocated per call. `assign`
    // and not `clear()` + `resize()`: it writes every element of [0, nodes.size()), which
    // is exactly the range the loop below reads, so no character can read a mark the
    // character before it left. The declaration carries the rest of that argument.
    std::vector<bool>& done = resolvedNodes;
    done.assign(nodes.size(), false);
    size_t resolved = 0;
    while (resolved < nodes.size()) {
        const size_t before = resolved;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (done[i]) continue;
            const int32_t parent = nodes[i].parent;
            if (parent >= 0 && !done[static_cast<size_t>(parent)]) continue;

            c.world[i] = parent >= 0 ? c.world[static_cast<size_t>(parent)] * localTransform(nodes[i])
                                     : localTransform(nodes[i]);
            done[i] = true;
            ++resolved;
        }
        // A cycle in the parent links, which is malformed input rather than a state to
        // recover from. Leaving the rest at identity is a visible wrong answer, which
        // beats looping forever.
        if (resolved == before) break;
    }

    if (c.skin >= rigData.skins.size()) return;
    const Skin& skin = rigData.skins[c.skin];
    for (size_t j = 0; j < skin.joints.size(); ++j) {
        const uint32_t node = skin.joints[j];
        const glm::mat4 jointWorld = node < c.world.size() ? c.world[node] : glm::mat4(1.0f);
        c.joints[j] = jointWorld * skin.inverseBind[j];
    }
}

void SceneAnimator::update(float dt) {
    auto s = core::Profiler::scope("SceneAnimator::update");
    fired.clear();

    // Who owns which node, before anything reads a pose off one. A node index carries no
    // rig -- see `characterForNode` -- so with two rigs merged this is the only thing that
    // can tell a joint on the second character from the same index on the first. First
    // claim wins, which is what keeps N copies of one skin agreeing on a shared joint.
    nodeOwner.assign(rigData.bind.nodes.size(), 0xFFFFFFFFu);
    for (uint32_t index = 0; index < static_cast<uint32_t>(characters.size()); ++index) {
        const Character& c = characters[index];
        if (!c.live || c.skin >= rigData.skins.size()) continue;
        for (const uint32_t joint : rigData.skins[c.skin].joints) {
            if (joint < nodeOwner.size() && nodeOwner[joint] == 0xFFFFFFFFu) nodeOwner[joint] = index;
        }
    }
    // **Skins first, then clips**, and the second pass is not the same case as the first. A
    // node no skin claims is a *rigid* animated node -- a drawbridge, a clock tower, a lift
    // in a hierarchy merged beside a character rig -- and the character that moves it is the
    // one playing the clip that names it. Second so a joint is never taken from its skin by
    // a clip that happens to mention it, and unclaimed-only so the answer is stable while a
    // character keeps playing the same clip.
    for (uint32_t index = 0; index < static_cast<uint32_t>(characters.size()); ++index) {
        const Character& c = characters[index];
        if (!c.live || c.current.clip >= rigData.clips.size()) continue;
        for (const AnimationChannel& channel : rigData.clips[c.current.clip].channels) {
            if (channel.node < nodeOwner.size() && nodeOwner[channel.node] == 0xFFFFFFFFu) {
                nodeOwner[channel.node] = index;
            }
        }
    }

    for (uint32_t index = 0; index < static_cast<uint32_t>(characters.size()); ++index) {
        Character& c = characters[index];
        if (!c.live) continue;
        // Before the clips advance, not after. A state entered this frame then gets a
        // full step of its own clip instead of stalling for one, which is what keeps a
        // cut transition from showing a single frame of the pose it just left. The cost
        // is that `waitForExit` sees the previous frame's time, so the frame a clamped
        // clip reaches its end is not the frame it leaves on -- one frame, stated here
        // rather than discovered in a transition that feels a hair late.
        stepStateMachine(c);

        if (c.fade < 1.0f) {
            c.fade = std::min(c.fade + c.fadeRate * dt, 1.0f);
            if (c.previous.clip < rigData.clips.size()) advance(c.previous, rigData.clips[c.previous.clip], dt);
        }
        if (c.current.clip < rigData.clips.size()) {
            // The time before the step, so the event scan knows what interval was
            // crossed. Only the current clip's events fire -- see `firedEvents`.
            const float from = c.current.time;
            advance(c.current, rigData.clips[c.current.clip], dt);

            eventScratch.clear();
            crossedEvents(c.current, rigData.clips[c.current.clip], from, eventScratch);
            for (const uint32_t e : eventScratch) fired.push_back({index, c.current.clip, e});
        }

        // Start from the bind pose every frame rather than accumulating. A clip that
        // drives rotation only would otherwise inherit whatever translation the
        // previous clip left behind, and the bug looks like a rig that drifts.
        c.pose = rigData.bind;
        if (!rigData.clips.empty()) {
            if (c.fade < 1.0f) {
                // Fade *out* of the previous clip and *into* the current one, which is
                // one blend rather than two: sample the outgoing pose, then move it
                // `fade` of the way toward the incoming one.
                sampleClip(rigData.clips[c.previous.clip], c.previous.time, c.pose);
                c.scratch = rigData.bind;
                sampleClip(rigData.clips[c.current.clip], c.current.time, c.scratch);
                blendPose(c.pose, c.scratch, c.fade);
            } else {
                sampleClip(rigData.clips[c.current.clip], c.current.time, c.pose);
            }
        }

        // ------------------------------------------------------- held weights (G11)
        // A character `createMorphed` made belongs to no node of the rig, so the copy of
        // `rig.bind` above resized its weight block to the file's -- zero, for a scene
        // whose glTF declares no morph target. Restored rather than protected from the
        // copy, because the copy is what keeps the *nodes* from drifting and that argument
        // does not stop applying just because these weights are somebody else's.
        if (!c.held.empty()) c.pose.weights = c.held;

        // ------------------------------------------------------------ root motion (C7)
        // After sampling and before resolve(), which is the only window where the pose
        // holds this step's authored root translation and nothing has consumed it yet.
        if (c.rootMotionNode < c.pose.nodes.size()) {
            const glm::vec3 sampled = c.pose.nodes[c.rootMotionNode].translation;
            c.rootDelta = c.hasPreviousRoot ? sampled - c.previousRoot : glm::vec3(0.0f);
            c.previousRoot = sampled;
            c.hasPreviousRoot = true;

            /**
             * **Horizontal only, and the vertical axis is the whole of why.** X and Z are
             * held at the bind translation, so the pose animates in place and the travel
             * is the controller's to apply -- reporting the delta *and* leaving the node
             * moving would move the character twice, which is what `setRootNode` argues.
             *
             * Y is the clip's and is left alone. Holding it too pins the root at whatever
             * height the *bind* pose happened to use, and a rig binds in a T-pose while it
             * animates with bent knees: the showcase rig binds its hips at 1.043 and idles
             * them between 0.946 and 0.978, so holding Y stood the character eight
             * centimetres off the floor and flattened the bob out of every walk cycle.
             *
             * The delta loses Y for the same reason it keeps X and Z: a controller fed a
             * vertical component would drive the character into the floor and the ceiling
             * on alternate steps, fighting the gravity that is already the solver's.
             */
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

glm::vec3 SceneAnimator::rootMotion(AnimatorId character) const {
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
    // Every character restarts its measurement: the node it was measuring against is not
    // the node it will be measuring now, and a delta across that change is meaningless.
    for (Character& c : characters) {
        c.rootMotionNode = node;
        c.hasPreviousRoot = false;
        c.rootDelta = glm::vec3(0.0f);
    }
}

void SceneAnimator::setRootNode(AnimatorId character, uint32_t node) {
    if (!valid(character)) return;
    Character& c = characters[character.index];
    // Guarded, because restarting the measurement is the expensive half of this call and a
    // game that resolves its root joint once per frame would otherwise report a delta of
    // zero for ever -- which is exactly the trap the animator-wide version documents.
    if (c.rootMotionNode == node) return;
    c.rootMotionNode = node;
    c.hasPreviousRoot = false;
    c.rootDelta = glm::vec3(0.0f);
}

uint32_t SceneAnimator::rootNode(AnimatorId character) const {
    return valid(character) ? characters[character.index].rootMotionNode : kNoNode;
}

const std::vector<glm::mat4>& SceneAnimator::worldTransforms(AnimatorId character) const {
    return valid(character) ? characters[character.index].world : kNoMatrices;
}

AnimatorId SceneAnimator::characterForNode(uint32_t node) const {
    if (node >= nodeOwner.size()) return {};
    const uint32_t slot = nodeOwner[node];
    if (slot >= characters.size() || !characters[slot].live) return {};
    return AnimatorId{slot, characters[slot].generation};
}

const std::vector<float>& SceneAnimator::morphWeights(uint32_t slot) const {
    static const std::vector<float> none; return slot < characters.size() ? characters[slot].pose.weights : none;
}

const std::vector<glm::mat4>& SceneAnimator::jointMatrices(uint32_t slot) const {
    static const std::vector<glm::mat4> none; return slot < characters.size() ? characters[slot].joints : none;
}

uint32_t SceneAnimator::skinOf(AnimatorId character) const {
    return valid(character) ? characters[character.index].skin : kNoSkin;
}

uint32_t SceneAnimator::jointOffset(uint32_t slot) const {
    return slot < characters.size() ? characters[slot].jointBase : 0u;
}

uint32_t SceneAnimator::weightOffset(uint32_t slot) const {
    return slot < characters.size() ? characters[slot].weightBase : 0u;
}

} // namespace scene
