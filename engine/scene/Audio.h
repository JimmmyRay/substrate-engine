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
 * @brief The device, the voices, the buses and the occlusion filters (S5).
 *
 * ## What this is not
 *
 * It is not a wrapper written so a second audio library could be dropped in -- that is
 * the RHI mistake simplicity rule 3 rules out, two subsystems along from where the
 * roadmap first names it. It is the one place that owns what a miniaudio engine needs to
 * exist: the resource manager, the engine, the sound groups, one `ma_sound` per source
 * and one low-pass node per occludable source. All of that is state, which is what puts
 * it in a class, and there is exactly one of it.
 *
 * miniaudio stays behind an `Impl` for the reason Jolt does in `Physics.h`: not
 * portability, but keeping a 100k-line header off the include path of every translation
 * unit that touches a scene.
 *
 * ## What miniaudio is doing and what this file is doing
 *
 * miniaudio owns decoding, resampling, the device, the mixing graph, the spatializer and
 * the biquads. Simplicity rule 6 permits exactly that: a mixer is a solved problem, and
 * this is the library Tethered used. What is written here is the part that is *this
 * engine's* rather than any mixer's -- what a glTF file may say about a sound, which
 * assets stream and which decode, how a bus ducks under another one, and what happens to
 * a source with a wall between it and the listener.
 *
 * ## Why there is a device-less mode, and why it is not a stub
 *
 * `backend: "null"` builds the engine with miniaudio's `noDevice`, so the caller pulls
 * PCM frames itself instead of a driver thread pulling them. **Every other thing on the
 * path is identical** -- the same decoders, the same resource manager, the same
 * spatializer, the same filters, the same mix. That is the same decision
 * `window.headless` makes for the renderer, and for the same reason it gives: an
 * unmapped window still presents, so a headless capture is comparable with a windowed
 * one. Here it buys three things a stub would not:
 *
 * - The unit suite can assert on the samples the mixer actually produced, on a machine
 *   with no sound card, under ASan and TSan.
 * - It is deterministic: no driver thread, no device clock, a fixed number of frames
 *   pulled per simulation step. Frame N holds the same samples on every run.
 * - A machine whose device fails to open falls back to it rather than to silence with a
 *   different code path behind it.
 */

/**
 * @brief One volume group (S5.4).
 *
 * A bus is a gain stage with a name, and ducking is the one thing that makes it more
 * than a multiply: `duckedBy` names another bus whose activity pulls this one down, over
 * a stated attack and release. That is where a mixer stops being a `play()` call, which
 * is what this row was pointing at.
 */
struct AudioBusDesc {
    std::string name;
    /// Linear gain the bus sits at when nothing is ducking it.
    float volume = 1.0f;

    /// Bus whose playing voices duck this one, by name, or empty for none. A name no bus
    /// claims is warned about and ignored.
    std::string duckedBy;
    /// Gain multiplier applied while the trigger bus has a voice playing. 1.0 is no
    /// ducking at all, and is the default: a mixer that quietly attenuated a bus because
    /// somebody else made a noise is the sort of help nobody asked for. `substrate.json`
    /// turns it on, which is what makes it a demonstration rather than a surprise.
    float duckAmount = 1.0f;
    /// Seconds to reach the ducked gain, and seconds to come back. Asymmetric on
    /// purpose and asymmetric in every mixing desk ever built: ducking late is audible
    /// as the first syllable of the thing you ducked for, and recovering early is
    /// audible as a pump.
    float duckAttack = 0.05f;
    float duckRelease = 0.4f;
};

/// Everything the engine needs to exist, in one struct because `Config` hands it over in
/// one piece -- the same shape `PhysicsConfig` has and for the same reason.
struct AudioConfig {
    /// Off skips the device, the decode and every source. On costs nothing for a scene
    /// that declares none, so the default is on.
    bool enabled = true;

    /// Where the mix goes. An enum rather than a string since D12: the three states were
    /// spelled out as string comparisons here *and* nowhere at all on the parsing side, so
    /// a misspelled backend reached this struct intact and was diagnosed one subsystem
    /// later. `Config` refuses a name that is not one, and what arrives here is a value.
    core::AudioBackend backend = core::AudioBackend::Auto;

    /// The mix format. Everything the resource manager decodes is converted to this
    /// once, so a scene of 44.1 kHz and 48 kHz assets resamples at load rather than per
    /// buffer. f32 throughout is miniaudio's own internal format and is not configurable
    /// here, because converting the mix bus to something else would only cost precision.
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    /// Pairs of ears (C28). One is a game with one point of view; two is split screen, where
    /// each player hears the room from where they are. Clamped to miniaudio's own maximum of
    /// four, and to at least one -- an engine with no listener spatialises nothing and would
    /// be silence with extra steps. Fixed at `init`: miniaudio sizes the spatializer's
    /// listener array once, and growing it means rebuilding the engine under every playing
    /// voice.
    uint32_t listeners = 1;

    float masterVolume = 1.0f;

    // ----------------------------------------------------- streaming (S5.2)
    /// Assets longer than this stream; shorter ones decode. See `audioShouldStream` for
    /// why it is a threshold on duration and `docs/architecture/systems.md` for the
    /// measurement the default came from.
    float streamThresholdSeconds = 5.0f;
    /// Ceiling on what decoding is allowed to cost, in bytes, across every source in the
    /// scene. A **stated budget** in 0.9's sense: what binds first is the threshold
    /// above, and this only ever forces a source to stream that would otherwise have
    /// decoded -- reported when it does, never silently. Zero is unbounded.
    uint64_t decodeBudgetBytes = 64u * 1024u * 1024u;

    // ---------------------------------------------------- occlusion (S5.5)
    /// Master switch for the raycast, the filter and the slew.
    bool occlusion = true;
    /// Cutoff a fully occluded source is filtered to. A wall passes low frequencies and
    /// stops high ones, so the filter is what makes occlusion sound like a wall rather
    /// than like a volume knob.
    float occlusionCutoffHz = 700.0f;
    /// Gain a fully occluded source is held at, on top of the filter.
    float occlusionGain = 0.45f;
    /// Seconds to reach full occlusion and to come out of it. Non-zero for the reason
    /// the duck times are: a filter cutoff stepped in one frame is an audible click, and
    /// a listener walking past a doorway would produce one per frame.
    float occlusionAttack = 0.08f;
    float occlusionRelease = 0.25f;
    /// How far short of each end the occlusion ray stops, in metres. The listener stands
    /// inside its own character's capsule and a source usually sits at the centre of the
    /// body it is attached to, so a ray drawn all the way to both would report every
    /// source as occluded by the thing it is bolted to.
    float occlusionRayMargin = 0.3f;

    /// Voices the engine will hold. A **stated budget**: a source past it is refused,
    /// counted and reported rather than dropped in silence.
    uint32_t voiceBudget = 64;

    /// Named gain stages, in file order. Empty gets the three every project invents
    /// anyway -- see `AudioEngine::init`.
    std::vector<AudioBusDesc> buses;
};

/**
 * @brief The scene's sounds.
 *
 * Sources keep the index `addSource` returned for as long as the engine lives; nothing
 * here compacts or reorders, for the same reason `InstanceTable` and `PhysicsWorld` do
 * not.
 *
 * **Nothing in this class can change the rendered frame.** That is worth stating
 * plainly, because it is what keeps every golden case in `scripts/golden.sh` valid
 * across S5: audio reads the camera and the physics world and writes to a mix buffer,
 * and no pixel depends on any of it.
 */
/// Tag for a playing sound. Declared, never defined -- see `core/Handle.h`.
struct SoundTag;
/// One source in the mix. A bus is still a bare index: buses are named in the config,
/// resolved once at init and never destroyed, so there is no lifetime for a handle to
/// carry and `kMasterBus` remains the right sentinel for "no bus".
using SoundId = core::Handle<SoundTag>;

class AudioEngine {
  public:
    static constexpr uint32_t kMasterBus = 0xFFFFFFFFu;

    /// The ceiling `voiceBudget` grows to (C40), and the only hard limit on simultaneous
    /// voices. Unlike the other three pools nothing is *allocated* per voice ahead of time,
    /// so what this bounds is mixing cost -- a property of the machine rather than of the
    /// game, which is why it is a constant here instead of a row a game states.
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

    /// True once `init` has brought something up. False also for `enabled: false`, so a
    /// caller has one thing to test.
    [[nodiscard]] bool active() const;
    /// True when a real playback device is running, as opposed to the device-less mix.
    [[nodiscard]] bool usingDevice() const;
    /// True when nothing has been added. The equivalent of `PhysicsWorld::empty()`, and
    /// what makes a scene with no sound in it cost nothing at all.
    [[nodiscard]] bool empty() const;

    /**
     * @brief Load one source and start it if it says to.
     *
     * Decides stream-versus-decode here (S5.2), resolves the bus by name (S5.4), and
     * builds the occlusion filter if the source wants one (S5.5).
     *
     * @return its index, or `kNoSource` when the file could not be opened or the voice
     *         budget refused it.
     */
    SoundId create(const AudioSourceDesc& desc);

    /**
     * @brief Fire one sound at a point, and forget about it (G7).
     *
     * The counterpart of `create`, and the difference is who owns the lifetime. A source a
     * node declared is furniture: it loops, it starts with the scene, and something knows
     * where it is for as long as the scene lives. A one-shot is an *event* -- a crate
     * landing, a door latching -- and the thing that fired it has no business remembering
     * it afterwards. So this forces `loop` off and `autoplay` on, and `update()` retires
     * the voice the step after it reaches its end.
     *
     * **It is `create` with two fields overwritten rather than a second loading path**, and
     * that is deliberate: the bus resolution, the stream-versus-decode decision, the
     * spatialiser and the occlusion filter are all things a one-shot wants and all things
     * `create` already does. What it adds is a per-file duration cache, because the
     * duration probe opens the file and a shot fired on every impact would otherwise be a
     * file open per impact.
     *
     * **It is not a `ma_sound` pool, and here is the number that says it need not be.**
     * miniaudio's resource manager caches the *decoded buffer* by path, so the second shot
     * on a file costs a reference to the first one's samples rather than a decode -- which
     * is the expensive part. What is left is a node-graph attach per shot. A pool would
     * save that and cost a free list per file; the trigger to build one is a profile that
     * shows the attach, and there is not one.
     *
     * @param desc what to play. `loop` and `autoplay` are ignored; everything else --
     *        volume, pitch, bus, attenuation, the cone -- is read exactly as `create` reads
     *        it, so a caller varying the loudness of an impact edits one field of a desc it
     *        built once.
     * @param position where it happens. Overwrites `desc.transform`, since a one-shot has a
     *        place and no orientation worth carrying.
     * @return the voice, which goes stale on its own when the sound ends. A caller that
     *         wants to cut it short can still `stop` or `destroy` it; a caller that does
     *         not can drop the handle, which is the ordinary case and the point.
     */
    SoundId playAt(const AudioSourceDesc& desc, const glm::vec3& position);

    /**
     * @brief Stop a source and retire its slot (C1).
     *
     * Deferred the way `PhysicsWorld::destroy` is, and for the same shape of reason: a
     * `ma_sound` is a node in a graph the device thread is walking, so the handle goes
     * stale on this call while the `ma_sound_uninit` happens at the next `update()`. The
     * slot's storage is never freed -- `voices` holds `unique_ptr`s precisely because the
     * mixer holds pointers into them -- so what is reused is the slot, not the address.
     */
    void destroy(SoundId id);

    /// Does this handle still name a live source?
    [[nodiscard]] bool valid(SoundId id) const;

    /// The handle occupying a slot, for the walkers -- the counterpart of
    /// `InstanceTable::idAt` and `PhysicsWorld::bodyAt`. Invalid for an empty slot.
    [[nodiscard]] SoundId soundAt(uint32_t slot) const;

    /// Slots, not live sources: what a walker pairs with `soundAt`.
    [[nodiscard]] uint32_t sourceCount() const;
    [[nodiscard]] const AudioSourceDesc& source(SoundId id) const;
    /// Which path `addSource` chose for this source, and why it is worth asking: the
    /// overlay and the load-time summary both report it, because a decision taken from a
    /// threshold is one somebody will want to check against the asset it was taken for.
    [[nodiscard]] bool sourceStreamed(SoundId id) const;
    [[nodiscard]] float sourceSeconds(SoundId id) const;

    /// Move a source. Only the translation is read, plus -Z for a cone.
    void setSourceTransform(SoundId id, const glm::mat4& transform);
    [[nodiscard]] glm::vec3 sourcePosition(SoundId id) const;
    [[nodiscard]] bool sourcePlaying(SoundId id) const;
    void start(SoundId id);
    void stop(SoundId id);

    /**
     * @brief Where a pair of ears is (S5.3, C28). `forward` and `up` are that listener's
     *        view, so the panning matches what they are looking down.
     *
     * **A spatial source is attenuated against the *closest* listener, not the sum of
     * them.** That is miniaudio's own rule and it is kept deliberately rather than
     * inherited: summing would make a room get louder as players are added and would double
     * a sound both of them can hear, so a source's loudness would stop being a property of
     * the scene. Closest-listener means a sound near player two is heard at player two's
     * volume, which is what split screen wants and what a top-down game moving the ears to
     * the cursor wants.
     *
     * An index past `listenerCount()` is ignored rather than clamped -- writing listener 3
     * of two onto listener 0 would put both players' ears in one place and look like a
     * panning bug.
     */
    void setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up,
                     uint32_t listener = 0);
    [[nodiscard]] glm::vec3 listenerPosition(uint32_t listener = 0) const;
    /// How many pairs of ears this engine came up with, from `AudioConfig::listeners`.
    [[nodiscard]] uint32_t listenerCount() const;

    /**
     * @brief Tell the engine whether the line to source `i` is blocked (S5.5).
     *
     * **The raycast is the caller's**, and that is the same division `renderer.debugLines`
     * draws: the application owns the scene's geometry and this class owns what a filter
     * does about it. It keeps Jolt off this file's include path and it means a game whose
     * occlusion comes from a navmesh, a portal graph or nothing at all calls the same
     * function.
     *
     * Idempotent per frame -- what is slewed is the *state*, not the number of calls.
     */
    void setOccluded(SoundId id, bool occluded);
    /// How far into occlusion source `i` currently is, 0 to 1, after the slew. What the
    /// debug draw colours its line by.
    [[nodiscard]] float occlusion(SoundId id) const;
    /// True for a source that both wants occlusion and is spatial, which is the set the
    /// caller has to raycast for.
    [[nodiscard]] bool occludable(SoundId id) const;

    /**
     * @brief Advance the ducking and the occlusion slews by one step, and mix.
     *
     * Called once per simulation step rather than once per frame, so what the mixer is
     * fed does not depend on the frame rate -- the same argument the animation, particle
     * and physics steps make in `Engine`. Under the device-less backend this is also
     * where the frames are pulled and where they would otherwise never be consumed.
     */
    void update(float dt);

    /// Peak absolute sample of the last block the device-less backend mixed, 0 when a
    /// real device is running (the driver thread owns those buffers and reading them
    /// from here would be a race for a number nobody acts on).
    [[nodiscard]] float lastPeak() const;
    /// Root-mean-square of the same block. What a test asserts on, because peak is one
    /// sample and RMS is a level.
    [[nodiscard]] float lastRms() const;

    // ------------------------------------------------------- capturing the mix (S7)
    /**
     * @brief Start copying the mixed output into a ring the recorder can drain.
     *
     * Fed from miniaudio's `onProcess`, which fires at the end of every mix -- so it is
     * the same samples that reach the speakers, taken from the same call, and it works
     * identically with a device and without one. There is no second mixing path and the
     * game stays audible while it records.
     *
     * @param seconds capacity of the ring. It only has to cover the gap between the audio
     *        thread producing and the recorder consuming; a second is generous.
     */
    void startCapture(float seconds);
    void stopCapture();
    [[nodiscard]] bool capturing() const;

    /// The ring itself, for a `Recorder` to drain directly rather than through a wrapper
    /// that would forward every call unchanged. Null until `startCapture`, and valid for
    /// as long as this engine is; `shutdown` stops the tap before releasing it.
    [[nodiscard]] core::AudioTap* captureTap();

    /// Recorder thread. Interleaved frames at `AudioConfig::sampleRate` and `channels`.
    [[nodiscard]] uint64_t readCaptured(float* dst, uint64_t maxFrames);
    /// Frames the audio thread could not fit because the recorder fell behind. A gap of
    /// exactly this length, which the recorder replaces with silence so the sound stays
    /// aligned with the picture rather than drifting earlier.
    [[nodiscard]] uint64_t capturedDropped() const;

    [[nodiscard]] uint32_t sampleRate() const;
    [[nodiscard]] uint32_t channelCount() const;

    /// Silence everything without stopping it, so a muted run still advances every
    /// stream, every duck and every slew. Muting by pausing would make the mute audible
    /// on release as a jump to wherever the sound would have been.
    void setMuted(bool muted);
    [[nodiscard]] bool muted() const;

    // ------------------------------------------------------------ buses (S5.4)
    [[nodiscard]] uint32_t busCount() const;
    [[nodiscard]] const std::string& busName(uint32_t bus) const;
    /// The bus's gain right now, after ducking. Reported on the overlay, because a gain
    /// that moves on its own is one somebody will accuse of being broken.
    [[nodiscard]] float busGain(uint32_t bus) const;
    /// Index of the bus with this name, or `kMasterBus`.
    [[nodiscard]] uint32_t findBus(const std::string& name) const;

    // ------------------------------------------------------------ statistics
    /// Bytes the decoded sources cost, as f32 at the mix rate. The number the budget is
    /// spent against, and it is counted **per distinct file rather than per source**:
    /// miniaudio's resource manager caches a decoded buffer by path, so forty sources on
    /// one sound decode it once. Charging each of them would refuse a scene memory it
    /// never spent.
    [[nodiscard]] uint64_t decodedBytes() const;
    [[nodiscard]] uint32_t streamedCount() const;
    [[nodiscard]] uint32_t decodedCount() const;
    /// Sources refused since init(): a voice past the budget, or a file that would not
    /// open. Non-zero means the scene asked for something it did not get.
    [[nodiscard]] uint32_t refusedSources() const;
    /// Sources the decode budget forced onto the streaming path. Separate from the count
    /// above because this one is not a refusal -- the sound plays, it just plays the
    /// other way, and 0.9's rule is that a budget which binds has to say so.
    [[nodiscard]] uint32_t budgetForcedStreams() const;

  private:
    /**
     * @brief Retire the least valuable live voice to make room for a new one. False when
     *        every voice is looping, which is the one case nothing should be taken from.
     *
     * Reached only at `kMaxVoices`, after the budget has finished growing. A one-shot is
     * preferred over a placed source and the quietest of either is taken first: losing a
     * footstep is momentary, and losing a loop is a hole in the mix that never fills back in.
     */
    bool stealVoice();

    /// Everything miniaudio owns, in one holder defined in the .cpp -- the same
    /// arrangement, and the same justification, as `PhysicsWorld::Impl`.
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace scene
