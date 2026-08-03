#pragma once

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
 * @brief What a glTF file says about sound, before any mixer sees it (S5.2, S5.3).
 *
 * Deliberately free of miniaudio, for the reason `Collider.h` is free of Jolt:
 * `GltfScene` places a source by its node's world transform exactly as it places a
 * light, an emitter or a collider, and it has no business knowing what a `ma_sound` is.
 * The schema lives here and the device lives in `Audio.h`.
 *
 * The extras key is `nodes[i].extras.substrate_audio`, read by the same targeted
 * rapidjson pass `substrate_emitter` and `substrate_collider` are, and for the same
 * reason -- see `core::json::gltfJsonSpan`, which this file is the third caller of and
 * therefore the reason it exists at all.
 */

/// How a source's samples reach the mixer (S5.2).
enum class AudioLoad : uint32_t {
    /// Decided by the asset's duration against `audio.streamThresholdSeconds`. The
    /// default, because duration is the thing the decision actually turns on and an
    /// author should not have to restate a rule the engine already holds.
    Auto,
    /// Decoded a buffer at a time, off disk, while it plays.
    Stream,
    /// Decoded once at load and held in memory as f32.
    Decode,
};

/// How loudness falls off with distance. miniaudio's models, named here so the schema
/// does not leak the library's spelling into a glTF file.
enum class AudioAttenuation : uint32_t {
    /// No distance term at all. What a non-spatial bed uses, and what a spatial source
    /// uses when the author wants the panning without the falloff.
    None,
    /// 1/d beyond `minDistance`. Physically what a point source does, and the default.
    Inverse,
    Linear,
    Exponential,
};

/**
 * @brief One sound, as `nodes[i].extras.substrate_audio` authored it.
 *
 * Every key is optional and every field has a working default, for the reason `Config`,
 * `ParticleEmitter` and `ColliderDesc` all give: a source that names two properties
 * should get two properties and the engine's defaults for the rest. The one key with no
 * default is `file`, and a source without one is refused and said so -- a sound with no
 * samples is not a sound with a default.
 *
 * Placed by its glTF node, like everything else in this engine that has a position: the
 * same source declared under two nodes is two sounds at two places, and only the node
 * knows where each one is. `node` is retained so an animated hierarchy or a physics body
 * can push a new transform in every frame -- a rattle on a falling crate is a source
 * whose transform is a body's.
 */
struct AudioSourceDesc {
    /// See scene/Node.h -- one declaration, aliased here for callers that spell it
    /// `AudioSourceDesc::kNoNode` (D3).
    static constexpr uint32_t kNoNode = scene::kNoNode;

    std::string name;
    /// Path to the asset, resolved relative to the scene file's own directory first and
    /// to the working directory second. Resolved that way round because a scene and its
    /// sounds travel together, and an author writing `"file": "hum.wav"` means the one
    /// next to the .gltf rather than the one next to the binary.
    std::string file;

    /// World placement, written by the scene's node walk. Only the translation is used:
    /// a cone is aimed by the node's -Z, which is glTF's forward, and nothing here has
    /// a size.
    glm::mat4 transform{1.0f};
    uint32_t node = kNoNode;

    /// Which bus this plays into, by name, or empty for the master bus. A name no bus
    /// claims is warned about and falls back to master -- the same policy the input map
    /// applies to a binding for an action nobody declared.
    std::string bus;

    /// Linear gain, not decibels. Decibels are the right unit for a mixing desk and the
    /// wrong one for a file an artist edits by hand next to `"intensity": 3.0`.
    float volume = 1.0f;
    float pitch = 1.0f;

    /// A source authored into a scene is furniture -- a generator hum, a fountain, a
    /// wind bed -- so both of these default to on. A one-shot that fires on an event is
    /// something the *application* plays, not something a node declares.
    bool loop = true;
    bool autoplay = true;

    // ------------------------------------------------------- spatial (S5.3)
    /// False makes this a bed: no panning, no distance term, full level everywhere.
    /// What music and a room tone are, and the reason it is per-source rather than
    /// per-bus is that a bus is a gain stage and a bed is a property of the sound.
    bool spatial = true;
    /// Distance at which the source is at full volume. Inside it nothing gets louder,
    /// which is what stops a listener walking through a source from producing an
    /// infinity.
    float minDistance = 1.0f;
    /// Distance past which the source is silent, or 0 for unbounded. Bounded is the
    /// cheaper answer and unbounded is the correct one, so the default is correct and
    /// a scene that wants the cheaper one says so.
    float maxDistance = 0.0f;
    /// How fast the attenuation model falls off. 1.0 is the model as stated.
    float rolloff = 1.0f;
    AudioAttenuation attenuation = AudioAttenuation::Inverse;

    /// Full-level cone half-angle and the wider one it fades to, in radians, about the
    /// node's -Z. The default is a full sphere at both, which is the omnidirectional
    /// source every other field describes; a cone only starts costing anything once an
    /// author narrows it.
    float coneInnerAngle = 6.28318531f;
    float coneOuterAngle = 6.28318531f;
    /// Gain outside the outer angle. Zero is silent behind the cone.
    float coneOuterGain = 0.0f;

    /// Pitch shift from relative motion, as a multiple of the physical effect. **Zero by
    /// default, and that is a statement rather than a timidity**: a Doppler term is a
    /// function of a source's *velocity*, this engine never hands the mixer one, and a
    /// factor of 1.0 over velocities that are always zero is a feature that is on and
    /// does nothing. A scene that wants it has to start reporting velocities first.
    float dopplerFactor = 0.0f;

    // --------------------------------------------------- streaming (S5.2)
    AudioLoad load = AudioLoad::Auto;

    // --------------------------------------------------- occlusion (S5.5)
    /// Test the line to the listener against the scene's colliders and filter when it is
    /// blocked. Meaningless for a bed and skipped for one, so this and `spatial` are not
    /// two ways of saying the same thing: a spatial source with occlusion off is one the
    /// author knows is never behind anything.
    bool occlusion = true;
};

/**
 * @brief Read every `nodes[i].extras.substrate_audio` out of a glTF document.
 *
 * Takes the document's bytes rather than a path or a parsed asset, which is what makes
 * it testable without a device, a file or fastgltf. `.glb` is handled by unwrapping its
 * JSON chunk, so a caller does not have to know which it has.
 *
 * The transform is left at identity and `node` carries the node index: placing a source
 * is the scene's job, because the same source under two nodes is two sounds.
 *
 * @return false when the bytes are not a glTF document at all. A document with no source
 *         in it is not a failure, it is Sponza.
 */
[[nodiscard]] bool parseSceneAudioSources(const rapidjson::Value& nodesArray, std::vector<AudioSourceDesc>& out);

/// The name in the JSON for each value, and the parse of it. Exposed because the test
/// that checks every spelling round-trips should not have to spell them twice.
[[nodiscard]] const char* audioLoadName(AudioLoad load);
[[nodiscard]] const char* audioAttenuationName(AudioAttenuation model);

/**
 * @brief Decoded footprint of `seconds` of audio at the mixer's own format.
 *
 * f32, because that is what miniaudio's resource manager stores a decoded sound as
 * regardless of what the file held -- a 16-bit WAV costs twice its file size in memory
 * and a 24-bit one costs less than it. Sizing from the *file* would therefore be sizing
 * from the wrong number, which is the mistake this function exists to make impossible.
 */
[[nodiscard]] uint64_t audioDecodedBytes(float seconds, uint32_t sampleRate, uint32_t channels);

/**
 * @brief The stream-versus-decode crossover, in one place (S5.2).
 *
 * **A tiered disposition whose crossover is decided rather than discovered.** The two
 * paths do not trade off against each other smoothly: decoding costs memory that grows
 * linearly with duration and nothing per buffer, streaming costs a fixed few microseconds
 * per voice per buffer and almost no memory. So the decision is a threshold on duration,
 * and the threshold is where the decoded footprint stops being worth its saving -- see
 * `docs/architecture/systems.md` for the measurement the default came from.
 *
 * An explicit `stream` or `decode` wins over the threshold, because an author who has
 * said which they want has more information than a rule about seconds does.
 *
 * @param seconds the asset's duration. A negative or zero duration means the length is
 *        not known before the file is opened, and that is streamed: the whole point of
 *        the decode path is that its cost is known in advance.
 */
[[nodiscard]] bool audioShouldStream(AudioLoad load, float seconds, float thresholdSeconds);

} // namespace scene
