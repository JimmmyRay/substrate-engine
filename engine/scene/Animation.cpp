#include "scene/Animation.h"

#include "core/Profiler.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace scene {

namespace {

/// Index of the last key at or before `t`, plus the fraction to the next one.
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
    // Hold the endpoint rather than extrapolating. A clip whose channels start at
    // different times is legal glTF, and extrapolating the earlier ones turns a rig
    // inside out for a frame.
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
        // quaternion components lands off the unit sphere, and a non-unit quaternion in
        // a joint matrix is a scale nobody authored.
        if (!isRotation) return v;
        const float len = glm::length(v);
        return len > 0.0f ? v / len : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const glm::vec4 a = s.values[key.index];
    const glm::vec4 b = s.values[key.index + 1];
    if (isRotation) {
        // Spherical, not linear: a lerp between two quaternions takes the chord rather
        // than the arc, which shows up as a joint speeding up through the middle of
        // every rotation and slowing at the ends.
        const glm::quat qa(a.w, a.x, a.y, a.z);
        const glm::quat qb(b.w, b.x, b.y, b.z);
        const glm::quat q = glm::slerp(qa, qb, key.alpha);
        return glm::vec4(q.x, q.y, q.z, q.w);
    }
    return glm::mix(a, b, key.alpha);
}

/// @brief Sample a morph-weight channel into `out`, which is `count` floats long.
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
    // **Strictly greater than.** `fmod(d, d)` is 0, so `>=` wraps a time of exactly the
    // duration to the first keyframe -- and a `ClampToEnd` playback sits on exactly the
    // duration for as long as it holds its last pose, which is a one-frame pop at the end
    // of every clip that ends. A looping clip cannot tell the difference: `advance` has
    // already wrapped it below the duration before it arrives here.
    const float raw = std::max(time, 0.0f);
    const float t = clip.duration <= 0.0f ? 0.0f : (raw > clip.duration ? std::fmod(raw, clip.duration) : raw);

    for (const AnimationChannel& ch : clip.channels) {
        if (ch.node >= pose.nodes.size() || ch.sampler >= clip.samplers.size()) continue;

        const AnimationSampler& s = clip.samplers[ch.sampler];
        SceneNode& n = pose.nodes[ch.node];

        if (ch.path == AnimationPath::Weights) {
            // Bounds are the node's, not the sampler's: a channel claiming more targets
            // than the mesh has would otherwise write past the pose's weight array.
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
        // Spherical, and renormalised. glm::slerp negates the target when the two are in
        // opposite hemispheres, so the blend takes the short arc; a plain mix sends a
        // joint the long way round on every second transition.
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
        // fmod of a negative time is negative, so a clip played backwards walks off the
        // front. One add is enough: |fmod| is strictly less than the duration.
        p.time = std::fmod(p.time, duration);
        if (p.time < 0.0f) p.time += duration;
        return false;
    }

    p.time = std::clamp(p.time, 0.0f, duration);
    // Reverse playback finishes at the *start*; testing against the duration leaves a
    // clamped clip run backwards never finishing.
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
        // as the new time being *behind* the old one. The two intervals must not overlap,
        // or an event fires twice for one step.
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

void SceneAnimator::init(AnimationRig r) {
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

uint32_t SceneAnimator::merge(const AnimationRig& extra) {
    const auto nodeBase = static_cast<uint32_t>(rigData.bind.nodes.size());
    const auto weightBase = static_cast<uint32_t>(rigData.bind.weights.size());
    const auto skinBase = static_cast<uint32_t>(rigData.skins.size());

    // The parent shift is signed because -1 is a root, and shifting one makes it a child
    // of an unrelated node of the base scene.
    rigData.bind.nodes.reserve(rigData.bind.nodes.size() + extra.bind.nodes.size());
    for (SceneNode n : extra.bind.nodes) {
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

    for (Skin s : extra.skins) {
        for (uint32_t& joint : s.joints) joint += nodeBase;
        rigData.skins.push_back(std::move(s));
    }

    for (AnimationClip c : extra.clips) {
        for (AnimationChannel& ch : c.channels) ch.node += nodeBase;
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

AnimatorId SceneAnimator::create(uint32_t skin) {
    return createSlot(skin, static_cast<uint32_t>(rigData.bind.weights.size()));
}

AnimatorId SceneAnimator::createMorphed(uint32_t targets) {
    // A valid handle to an empty block is a handle whose every `setMorphWeight` silently
    // does nothing.
    if (targets == 0) return {};

    const AnimatorId id = createSlot(kNoSkin, targets);
    if (!id.valid()) return id;

    Character& c = characters[id.index];
    c.held.assign(targets, 0.0f);
    // Replaces whatever `rig.bind` sized the block to -- nothing at all, for a scene whose
    // glTF declares no morph target. `update` restores it from `held` after every sample
    // for the same reason.
    c.pose.weights = c.held;
    return id;
}

void SceneAnimator::setMorphWeight(AnimatorId character, uint32_t target, float weight) {
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

float SceneAnimator::morphWeight(AnimatorId character, uint32_t target) const {
    if (!valid(character)) return 0.0f;
    const Character& c = characters[character.index];
    return target < c.held.size() ? c.held[target] : 0.0f;
}

AnimatorId SceneAnimator::createSlot(uint32_t skin, uint32_t weights) {
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

void SceneAnimator::play(AnimatorId character, uint32_t clip, float fade, LoopMode loop, float speed) {
    if (!valid(character) || clip >= rigData.clips.size()) return;
    Character& c = characters[character.index];
    // Already playing it, and not still fading out of something else: restarting here
    // would reset the clip every frame for anything driving this from held input.
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

        // Consume every trigger this transition tested, whichever way it tested it.
        // Leaving one set fires the next transition that reads it in the same frame.
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
    // Parents before children, in repeated passes: glTF does not require the node array to
    // be topologically ordered, so a child may precede its parent. Every pass resolves at
    // least the shallowest unresolved node, which is what terminates it.
    const std::vector<SceneNode>& nodes = c.pose.nodes;
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

            c.world[i] = parent >= 0 ? c.world[static_cast<size_t>(parent)] * localTransform(nodes[i])
                                     : localTransform(nodes[i]);
            done[i] = true;
            ++resolved;
        }
        // A cycle in the parent links. Leaving the rest at identity is a visible wrong
        // answer; without the break it is an infinite loop.
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
        for (const AnimationChannel& channel : rigData.clips[c.current.clip].channels) {
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
            if (c.previous.clip < rigData.clips.size()) advance(c.previous, rigData.clips[c.previous.clip], dt);
        }
        if (c.current.clip < rigData.clips.size()) {
            // Taken before the step: `crossedEvents` needs the interval, not the endpoint.
            const float from = c.current.time;
            advance(c.current, rigData.clips[c.current.clip], dt);

            eventScratch.clear();
            crossedEvents(c.current, rigData.clips[c.current.clip], from, eventScratch);
            for (const uint32_t e : eventScratch) fired.push_back({index, c.current.clip, e});
        }

        // Start from the bind pose every frame rather than accumulating: a clip that drives
        // rotation only would otherwise inherit whatever translation the previous clip left
        // behind, and the bug looks like a rig that drifts.
        c.pose = rigData.bind;
        if (!rigData.clips.empty()) {
            if (c.fade < 1.0f) {
                sampleClip(rigData.clips[c.previous.clip], c.previous.time, c.pose);
                c.scratch = rigData.bind;
                sampleClip(rigData.clips[c.current.clip], c.current.time, c.scratch);
                blendPose(c.pose, c.scratch, c.fade);
            } else {
                sampleClip(rigData.clips[c.current.clip], c.current.time, c.pose);
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
    // Every character restarts its measurement; a delta taken across a change of node is
    // the distance between two unrelated joints.
    for (Character& c : characters) {
        c.rootMotionNode = node;
        c.hasPreviousRoot = false;
        c.rootDelta = glm::vec3(0.0f);
    }
}

void SceneAnimator::setRootNode(AnimatorId character, uint32_t node) {
    if (!valid(character)) return;
    Character& c = characters[character.index];
    // Guarded: without it a game that sets the same root node every frame restarts the
    // measurement every frame and reads a delta of zero for ever.
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
