#pragma once

#include "core/Handle.h"
#include "core/AudioBackend.h"
#include "scene/AudioSource.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace core {
class AudioTap;
} // namespace core

namespace scene {

/**
 * @file Audio.h
 * @brief The device, the voices, the buses and the occlusion filters.
 *
 * miniaudio stays behind an `Impl` to keep a 100k-line header off the include path of
 * every translation unit that touches a scene.
 *
 * `backend: "null"` builds the engine with miniaudio's `noDevice`, so the caller pulls
 * PCM frames itself; every other thing on the path is identical -- the same decoders,
 * resource manager, spatializer, filters and mix. Replacing it with a stub costs the unit
 * suite its assertions on real mixed samples, the determinism a fixed pull per step
 * gives, and the fallback a machine whose device will not open takes.
 */

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

/// Everything the engine needs to exist.
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
 * @brief The scene's sounds.
 *
 * A source keeps the slot `create` gave it for as long as the engine lives; nothing here
 * compacts or reorders, so a slot index taken one frame is still that source the next.
 */
/// Tag for a playing sound. Declared, never defined -- see `core/Handle.h`.
struct SoundTag;
using SoundId = core::Handle<SoundTag>;

class AudioEngine {
  public:
    static constexpr uint32_t kMasterBus = 0xFFFFFFFFu;

    /// The ceiling `voiceBudget` grows to, and the only hard limit on simultaneous
    /// voices. What it bounds is mixing cost, which is a property of the machine, so it
    /// is a constant here rather than a row a game states.
    static constexpr uint32_t kMaxVoices = 1024;

    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// Open the device (or the device-less mix) and create the buses.
    /// @return false when neither could be brought up, which leaves every call below a
    ///         no-op rather than a crash -- a game with no sound card still runs.
    bool init(const AudioConfig& cfg);
    void shutdown();

    /// True once `init` has brought something up, and false for `enabled: false`, so a
    /// caller has one thing to test.
    [[nodiscard]] bool active() const;
    /// True when a real playback device is running, as opposed to the device-less mix.
    [[nodiscard]] bool usingDevice() const;
    [[nodiscard]] bool empty() const;

    /**
     * @brief Load one source and start it if it says to.
     *
     * @return its handle, invalid when the file could not be opened or the voice budget
     *         refused it.
     */
    SoundId create(const AudioSourceDesc& desc);

    /**
     * @brief Fire one sound at a point, and forget about it.
     *
     * @param desc what to play. `loop` is forced off and `autoplay` on; everything else --
     *        volume, pitch, bus, attenuation, the cone -- is read exactly as `create`
     *        reads it.
     * @param position where it happens. Overwrites `desc.transform`.
     * @return the voice, which goes stale on its own the step after the sound ends. A
     *         caller may `stop` or `destroy` it early, or drop the handle.
     */
    SoundId playAt(const AudioSourceDesc& desc, const glm::vec3& position);

    /**
     * @brief Stop a source and retire its slot.
     *
     * Deferred: a `ma_sound` is a node in a graph the device thread is walking, so the
     * handle goes stale on this call while the `ma_sound_uninit` happens at the next
     * `update()`. The slot's storage is never freed -- `voices` holds `unique_ptr`s
     * precisely because the mixer holds pointers into them -- so what is reused is the
     * slot, not the address.
     */
    void destroy(SoundId id);

    /// Does this handle still name a live source?
    [[nodiscard]] bool valid(SoundId id) const;

    /// The handle occupying a slot, invalid for an empty one.
    [[nodiscard]] SoundId soundAt(uint32_t slot) const;

    /// Slots, not live sources: what a walker pairs with `soundAt`.
    [[nodiscard]] uint32_t sourceCount() const;
    [[nodiscard]] const AudioSourceDesc& source(SoundId id) const;
    /// Which path `create` chose for this source. Reported by the overlay and the
    /// load-time summary, since a decision taken from a threshold is one somebody will
    /// want to check against the asset it was taken for.
    [[nodiscard]] bool sourceStreamed(SoundId id) const;
    [[nodiscard]] float sourceSeconds(SoundId id) const;

    /// Move a source. Only the translation is read, plus -Z for a cone.
    void setSourceTransform(SoundId id, const glm::mat4& transform);
    [[nodiscard]] glm::vec3 sourcePosition(SoundId id) const;
    [[nodiscard]] bool sourcePlaying(SoundId id) const;
    void start(SoundId id);
    void stop(SoundId id);

    /**
     * @brief Where a pair of ears is. `forward` and `up` are that listener's view, so the
     *        panning matches what they are looking down.
     *
     * A spatial source is attenuated against the **closest** listener, not the sum of
     * them -- miniaudio's own rule, and kept: summing makes a room louder as players are
     * added and doubles a sound both can hear, so loudness stops being a property of the
     * scene.
     *
     * An index past `listenerCount()` is ignored rather than clamped, because writing
     * listener 3 of two onto listener 0 puts both players' ears in one place and reads as
     * a panning bug.
     */
    void setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up,
                     uint32_t listener = 0);
    [[nodiscard]] glm::vec3 listenerPosition(uint32_t listener = 0) const;
    /// How many pairs of ears this engine came up with, from `AudioConfig::listeners`.
    [[nodiscard]] uint32_t listenerCount() const;

    /**
     * @brief Tell the engine whether the line to a source is blocked.
     *
     * **The raycast is the caller's**: this keeps Jolt off the file's include path, and a
     * game whose occlusion comes from a navmesh or a portal graph calls the same function.
     *
     * Idempotent per frame -- what is slewed is the state, not the number of calls.
     */
    void setOccluded(SoundId id, bool occluded);
    /// How far into occlusion a source currently is, 0 to 1, after the slew.
    [[nodiscard]] float occlusion(SoundId id) const;
    /// True for a source that both wants occlusion and is spatial, which is the set the
    /// caller has to raycast for.
    [[nodiscard]] bool occludable(SoundId id) const;

    /**
     * @brief Advance the ducking and the occlusion slews by one step, and mix.
     *
     * `dt` must be the fixed simulation step, or what the mixer is fed depends on the
     * frame rate. Under the device-less backend this is also where the frames are pulled,
     * and without a call they are never consumed at all.
     */
    void update(float dt);

    /// Peak absolute sample of the last block the device-less backend mixed, and 0 while
    /// a real device is running -- the driver thread owns those buffers, and reading them
    /// from here would be a race for a number nobody acts on.
    [[nodiscard]] float lastPeak() const;
    /// Root-mean-square of the same block. What a test asserts on, because peak is one
    /// sample and RMS is a level.
    [[nodiscard]] float lastRms() const;

    /**
     * @brief Start copying the mixed output into a ring the recorder can drain.
     *
     * Fed from miniaudio's `onProcess` at the end of every mix, so these are the samples
     * that reach the speakers and the path is the same with a device and without one.
     *
     * @param seconds capacity of the ring. It only has to cover the gap between the audio
     *        thread producing and the recorder consuming; a second is generous.
     */
    void startCapture(float seconds);
    void stopCapture();
    [[nodiscard]] bool capturing() const;

    /// The capture ring, for a `Recorder` to drain. Null until `startCapture`, and valid
    /// for as long as this engine is; `shutdown` stops the tap before releasing it.
    [[nodiscard]] core::AudioTap* captureTap();

    /// Called from the recorder thread. Interleaved frames at `AudioConfig::sampleRate`
    /// and `channels`.
    [[nodiscard]] uint64_t readCaptured(float* dst, uint64_t maxFrames);
    /// Frames the audio thread could not fit because the recorder fell behind. The
    /// recorder must replace exactly this many with silence, or the sound drifts earlier
    /// than the picture.
    [[nodiscard]] uint64_t capturedDropped() const;

    [[nodiscard]] uint32_t sampleRate() const;
    [[nodiscard]] uint32_t channelCount() const;

    /// Silence everything without stopping it, so a muted run still advances every
    /// stream, every duck and every slew. Muting by pausing makes the mute audible on
    /// release, as a jump to wherever the sound would have been.
    void setMuted(bool muted);
    [[nodiscard]] bool muted() const;

    [[nodiscard]] uint32_t busCount() const;
    [[nodiscard]] const std::string& busName(uint32_t bus) const;
    /// The bus's gain right now, after ducking.
    [[nodiscard]] float busGain(uint32_t bus) const;
    /// Index of the bus with this name, or `kMasterBus`.
    [[nodiscard]] uint32_t findBus(const std::string& name) const;

    /// Bytes the decoded sources cost, as f32 at the mix rate, counted **per distinct
    /// file rather than per source**: miniaudio's resource manager caches a decoded buffer
    /// by path, so charging each of forty sources on one sound would refuse a scene memory
    /// it never spent.
    [[nodiscard]] uint64_t decodedBytes() const;
    [[nodiscard]] uint32_t streamedCount() const;
    [[nodiscard]] uint32_t decodedCount() const;
    /// Sources refused since init(): a voice past the budget, or a file that would not
    /// open.
    [[nodiscard]] uint32_t refusedSources() const;
    /// Sources the decode budget forced onto the streaming path. Not counted as refusals,
    /// since they still play.
    [[nodiscard]] uint32_t budgetForcedStreams() const;

  private:
    /**
     * @brief Retire the least valuable live voice to make room for a new one. False when
     *        every voice is looping, which is the one case nothing is taken from.
     *
     * Reached only at `kMaxVoices`, after the budget has finished growing. A one-shot is
     * preferred over a placed source and the quietest of either goes first: losing a
     * footstep is momentary, losing a loop is a hole in the mix that never fills back in.
     */
    bool stealVoice();

    /// Everything miniaudio owns, defined in the .cpp.
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace scene
