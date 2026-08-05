#include "scene/AnimationRig.h"

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

bool advance(core::ClipPlayback& p, const AnimationClip& clip, float dt) {
    return core::advance(p, clip.duration, dt);
}

void crossedEvents(const core::ClipPlayback& p, const AnimationClip& clip, float from, std::vector<uint32_t>& out) {
    core::crossedEvents(p, clip.events, clip.duration, from, out);
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

} // namespace scene
