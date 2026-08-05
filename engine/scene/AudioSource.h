#pragma once

#include "core/AudioBackend.h"
#include "core/Handle.h"
#include "scene/Node.h"
#include <rapidjson/fwd.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scene {

/**
 * @file AudioSource.h
 * @brief What a document, a config and a game say about sound, before any mixer sees it.
 *
 * The schema, free of miniaudio so that `GltfScene` can place a source by its node's
 * world transform without knowing what a `ma_sound` is. The mixer lives in
 * `engine/audio/Audio.h`, which is a module: everything here is what the engine and that
 * module both have to name, so moving any of it up puts `scene/` and `Modules.h` on the
 * wrong side of the layer guard. The extras key is `nodes[i].extras.substrate_audio`.
 */

/// How a source's samples reach the mixer.
enum class AudioLoad : uint32_t {
    /// Decided by the asset's duration against `audio.streamThresholdSeconds`.
    Auto,
    /// Decoded a buffer at a time, off disk, while it plays.
    Stream,
    /// Decoded once at load and held in memory as f32.
    Decode,
};

/// How loudness falls off with distance. miniaudio's models, named here so the schema
/// does not leak the library's spelling into a glTF file.
enum class AudioAttenuation : uint32_t {
    /// No distance term at all -- panning without falloff.
    None,
    /// 1/d beyond `minDistance`, and the default.
    Inverse,
    Linear,
    Exponential,
};

/**
 * @brief One sound, as `nodes[i].extras.substrate_audio` authored it.
 *
 * `file` is the one key with no default; a source without one is refused and said so.
 * The same source declared under two nodes is two sounds at two places, and only the
 * node knows where each one is, so placing them is the scene's job.
 */
struct AudioSourceDesc {
    /// See scene/Node.h -- one declaration, aliased here for callers that spell it
    /// `AudioSourceDesc::kNoNode`.
    static constexpr uint32_t kNoNode = scene::kNoNode;

    std::string name;
    /// Path to the asset, resolved relative to the scene file's own directory first and
    /// to the working directory second -- an author writing `"file": "hum.wav"` means the
    /// one next to the .gltf, not the one next to the binary.
    std::string file;

    /// World placement, written by the scene's node walk. Only the translation and the
    /// node's -Z, which aims the cone, are read.
    glm::mat4 transform{1.0f};
    uint32_t node = kNoNode;

    /// Which bus this plays into, by name, or empty for the master bus. A name no bus
    /// claims is warned about and falls back to master.
    std::string bus;

    /// Linear gain, not decibels.
    float volume = 1.0f;
    float pitch = 1.0f;

    bool loop = true;
    bool autoplay = true;

    /// False makes this a bed: no panning, no distance term, full level everywhere.
    bool spatial = true;
    /// Distance at which the source is at full volume, in metres. Inside it nothing gets
    /// louder, which is what stops a listener walking through a source from producing an
    /// infinity.
    float minDistance = 1.0f;
    /// Distance past which the source is silent, or 0 for unbounded.
    float maxDistance = 0.0f;
    /// How fast the attenuation model falls off. 1.0 is the model as stated.
    float rolloff = 1.0f;
    AudioAttenuation attenuation = AudioAttenuation::Inverse;

    /// Full-level cone half-angle and the wider one it fades to, in radians, about the
    /// node's -Z. A full sphere at both is the omnidirectional source.
    float coneInnerAngle = 6.28318531f;
    float coneOuterAngle = 6.28318531f;
    /// Gain outside the outer angle. Zero is silent behind the cone.
    float coneOuterGain = 0.0f;

    /// Pitch shift from relative motion, as a multiple of the physical effect. Zero
    /// because a Doppler term is a function of a source's velocity and nothing hands the
    /// mixer one; raising it does nothing until something does.
    float dopplerFactor = 0.0f;

    AudioLoad load = AudioLoad::Auto;

    /// Test the line to the listener against the scene's colliders and filter when it is
    /// blocked. Skipped for a bed, so this is not a second spelling of `spatial`.
    bool occlusion = true;
};

/// Tag for a playing sound. Declared, never defined -- see `core/Handle.h`.
struct SoundTag;
/// A source keeps the slot it was created in for as long as the mixer lives; nothing
/// compacts or reorders, so a slot index taken one frame is still that source the next.
using SoundId = core::Handle<SoundTag>;

/// One volume group: a named gain stage, plus the ducking that makes it more than a
/// multiply.
struct AudioBusDesc {
    std::string name;
    /// Linear gain the bus sits at when nothing is ducking it.
    float volume = 1.0f;

    /// Bus whose playing voices duck this one, by name, or empty for none. A name no bus
    /// claims is warned about and ignored.
    std::string duckedBy;
    /// Gain multiplier applied while the trigger bus has a voice playing. 1.0, the
    /// default, is no ducking at all.
    float duckAmount = 1.0f;
    /// Seconds to reach the ducked gain, and seconds to come back. Asymmetric on purpose:
    /// ducking late is audible as the first syllable of the thing you ducked for, and
    /// recovering early is audible as a pump.
    float duckAttack = 0.05f;
    float duckRelease = 0.4f;
};

/// Everything the mixer needs to exist.
struct AudioConfig {
    /// Off skips the device, the decode and every source.
    bool enabled = true;

    /// Where the mix goes. `Config` refuses a name that is not one of these, so what
    /// arrives here is already a value rather than a string to re-validate.
    core::AudioBackend backend = core::AudioBackend::Auto;

    /// The mix format. Everything the resource manager decodes is converted to this once,
    /// so a scene of 44.1 kHz and 48 kHz assets resamples at load rather than per buffer.
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    /// Pairs of ears, clamped to [1, 4] -- miniaudio's own maximum. Fixed at `init`,
    /// because miniaudio sizes the spatializer's listener array once and growing it means
    /// rebuilding the engine under every playing voice.
    uint32_t listeners = 1;

    float masterVolume = 1.0f;

    /// Assets longer than this stream; shorter ones decode. See `audioShouldStream` for
    /// why it is a threshold on duration and `docs/architecture/systems.md` for the
    /// measurement the default came from.
    float streamThresholdSeconds = 5.0f;
    /// Ceiling on what decoding may cost, in bytes, across every source in the scene.
    /// Binds after `streamThresholdSeconds` and only ever forces a source onto the
    /// streaming path -- counted by `budgetForcedStreams()`, never silent. Zero is
    /// unbounded.
    uint64_t decodeBudgetBytes = 64u * 1024u * 1024u;

    /// Master switch for the raycast, the filter and the slew.
    bool occlusion = true;
    /// Cutoff a fully occluded source is filtered to, in Hz. A wall passes low
    /// frequencies, so the filter is what keeps occlusion from sounding like a volume
    /// knob.
    float occlusionCutoffHz = 700.0f;
    /// Gain a fully occluded source is held at, on top of the filter.
    float occlusionGain = 0.45f;
    /// Seconds to reach full occlusion and to come out of it. Zero steps the cutoff in
    /// one frame, which is an audible click per frame for a listener walking past a
    /// doorway.
    float occlusionAttack = 0.08f;
    float occlusionRelease = 0.25f;
    /// How far short of each end the occlusion ray stops, in metres. The listener stands
    /// inside its own character's capsule and a source usually sits at the centre of the
    /// body it is attached to, so a ray drawn all the way to both reports every source as
    /// occluded by the thing it is bolted to.
    float occlusionRayMargin = 0.3f;

    /// Voices the engine will hold. A source past it is refused, counted and reported.
    uint32_t voiceBudget = 64;

    /// Named gain stages, in file order. Empty gets the defaults `AudioEngine::init`
    /// creates.
    std::vector<AudioBusDesc> buses;
};

/**
 * @brief Read every `nodes[i].extras.substrate_audio` out of a glTF `nodes` array.
 *
 * The transform is left at identity and `node` carries the node index, so the caller
 * still has to place each source.
 */
[[nodiscard]] bool parseSceneAudioSources(const rapidjson::Value& nodesArray, std::vector<AudioSourceDesc>& out);

/// The name in the JSON for each value. Exposed so the test that checks every spelling
/// round-trips does not have to spell them twice.
[[nodiscard]] const char* audioLoadName(AudioLoad load);
[[nodiscard]] const char* audioAttenuationName(AudioAttenuation model);

/**
 * @brief Decoded footprint of `seconds` of audio at the mixer's own format.
 *
 * f32, because that is what miniaudio's resource manager stores a decoded sound as
 * whatever the file held -- so a 16-bit WAV costs twice its file size in memory, and
 * sizing from the file would be sizing from the wrong number.
 */
[[nodiscard]] uint64_t audioDecodedBytes(float seconds, uint32_t sampleRate, uint32_t channels);

/**
 * @brief The stream-versus-decode crossover, in one place.
 *
 * A threshold on duration, because the two paths do not trade off smoothly: decoding
 * costs memory linear in duration and nothing per buffer, streaming costs a few
 * microseconds per voice per buffer and almost no memory. See
 * `docs/architecture/systems.md` for the measurement the default came from.
 *
 * @param seconds the asset's duration. Zero or negative means the length was not known
 *        before the file was opened, and that streams -- the decode path's whole value is
 *        that its cost is known in advance.
 */
[[nodiscard]] bool audioShouldStream(AudioLoad load, float seconds, float thresholdSeconds);

} // namespace scene
