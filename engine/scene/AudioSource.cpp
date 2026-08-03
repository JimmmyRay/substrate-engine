#include "scene/AudioSource.h"

#include "core/Json.h"
#include "core/Logger.h"

#include <rapidjson/document.h>

#include <string>

namespace scene {

namespace {

using rapidjson::Value;
using core::json::gltfJsonSpan;
using core::json::member;
using core::json::readAngleDegrees;
using core::json::readBool;
using core::json::readFloat;
using core::json::readString;
using core::json::readVec;

/// An unrecognised spelling keeps the default *and says so*, exactly as `Collider.cpp`
/// does and for the same reason: a typo in an attenuation model is a source that is
/// quietly the wrong loudness, and the file it came from looks correct.
template <typename Enum, size_t N>
void readEnum(const Value& parent, const char* key, const char* const (&names)[N], Enum& out, const char* what,
              const std::string& owner) {
    std::string text;
    core::json::readString(parent, key, text);
    if (text.empty()) return;
    for (size_t i = 0; i < N; ++i) {
        if (text == names[i]) {
            out = static_cast<Enum>(i);
            return;
        }
    }
    core::Logger::warn(core::LogCategory::Audio, "Audio source '%s': unknown %s '%s' -- keeping %s", owner.c_str(), what,
                 text.c_str(), names[static_cast<size_t>(out)]);
}

const char* const kLoadNames[] = {"auto", "stream", "decode"};
const char* const kAttenuationNames[] = {"none", "inverse", "linear", "exponential"};

} // namespace

const char* audioLoadName(AudioLoad load) { return kLoadNames[static_cast<size_t>(load)]; }
const char* audioAttenuationName(AudioAttenuation model) { return kAttenuationNames[static_cast<size_t>(model)]; }

uint64_t audioDecodedBytes(float seconds, uint32_t sampleRate, uint32_t channels) {
    if (seconds <= 0.0f) return 0;
    return static_cast<uint64_t>(static_cast<double>(seconds) * sampleRate) * channels * sizeof(float);
}

bool audioShouldStream(AudioLoad load, float seconds, float thresholdSeconds) {
    if (load == AudioLoad::Stream) return true;
    if (load == AudioLoad::Decode) return false;
    // A length the decoder could not state before opening the file is a decoded
    // footprint nobody can bound, and the whole value of the decode path is that its
    // cost is known in advance. So it streams.
    if (seconds <= 0.0f) return true;
    return seconds > thresholdSeconds;
}

bool parseSceneAudioSources(const rapidjson::Value& nodesArray, std::vector<AudioSourceDesc>& out) {
    // The document is parsed once, by the caller, and handed to all three readers
    // (C14). It used to be parsed here, and in the other two, and that was about
    // three quarters of a large scene's load -- see core/Json.h.
    const Value* nodes = &nodesArray;

    for (rapidjson::SizeType n = 0; n < nodes->Size(); ++n) {
        const Value* extras = core::json::member((*nodes)[n], "extras");
        if (extras == nullptr) continue;
        const Value* def = core::json::member(*extras, "substrate_audio");
        if (def == nullptr || !def->IsObject()) continue;

        AudioSourceDesc a;
        a.node = n;
        core::json::readString(*def, "name", a.name);
        if (a.name.empty()) core::json::readString((*nodes)[n], "name", a.name);
        if (a.name.empty()) a.name = "node " + std::to_string(n);

        core::json::readString(*def, "file", a.file);
        // The one key with no default. Refused here rather than at load, because this is
        // where the file said it and the message can name the node.
        if (a.file.empty()) {
            core::Logger::warn(core::LogCategory::Audio, "Audio source '%s': no 'file' -- skipped", a.name.c_str());
            continue;
        }

        readString(*def, "bus", a.bus);
        core::json::readFloat(*def, "volume", a.volume);
        core::json::readFloat(*def, "pitch", a.pitch);
        core::json::readBool(*def, "loop", a.loop);
        core::json::readBool(*def, "autoplay", a.autoplay);

        readBool(*def, "spatial", a.spatial);
        readFloat(*def, "minDistance", a.minDistance);
        readFloat(*def, "maxDistance", a.maxDistance);
        readFloat(*def, "rolloff", a.rolloff);
        readEnum(*def, "attenuation", kAttenuationNames, a.attenuation, "attenuation model", a.name);

        core::json::readAngleDegrees(*def, "coneInnerAngle", a.coneInnerAngle);
        core::json::readAngleDegrees(*def, "coneOuterAngle", a.coneOuterAngle);
        readFloat(*def, "coneOuterGain", a.coneOuterGain);
        readFloat(*def, "dopplerFactor", a.dopplerFactor);

        readEnum(*def, "load", kLoadNames, a.load, "load mode", a.name);
        readBool(*def, "occlusion", a.occlusion);

        // An inner cone wider than its outer one is a fade that runs backwards, and
        // miniaudio's spatializer does not defend against it. Corrected and said so,
        // which is what S4.2 does for a capsule under non-uniform scale.
        if (a.coneOuterAngle < a.coneInnerAngle) {
            core::Logger::warn(core::LogCategory::Audio,
                         "Audio source '%s': coneOuterAngle is inside coneInnerAngle -- widening the outer to match",
                         a.name.c_str());
            a.coneOuterAngle = a.coneInnerAngle;
        }
        // Same shape of correction. A max inside the min is a source that is silent
        // everywhere including at the listener's own feet.
        if (a.maxDistance > 0.0f && a.maxDistance < a.minDistance) {
            core::Logger::warn(core::LogCategory::Audio,
                         "Audio source '%s': maxDistance %.2f is inside minDistance %.2f -- ignoring the cap",
                         a.name.c_str(), static_cast<double>(a.maxDistance), static_cast<double>(a.minDistance));
            a.maxDistance = 0.0f;
        }

        out.push_back(std::move(a));
    }
    return true;
}

} // namespace scene
