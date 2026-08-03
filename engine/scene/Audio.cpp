#include "scene/Audio.h"

#include "core/AudioTap.h"

#include "core/Logger.h"
#include "core/Profiler.h"

#include <miniaudio.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_map>

namespace scene {

namespace {

/**
 * @brief Move a 0-to-1 state toward `target` at a rate of one full range per `seconds`.
 *
 * Linear rather than exponential, and that is a choice rather than a shortcut: an
 * exponential approach never actually arrives, so a bus that "finished" ducking is still
 * a fraction of a decibel down forever, and a filter that "finished" opening is still
 * filtering. Linear arrives, and at these times -- tens to hundreds of milliseconds --
 * nothing about the two is distinguishable by ear.
 *
 * Both callers slew a *state* in [0, 1] and then interpolate the real quantity from it,
 * rather than slewing gains and cutoffs directly. That is what lets one helper serve a
 * bus gain and a filter cutoff without a `mode` flag: the ducking depth and the occlusion
 * depth are the same kind of number, and the gain and the hertz are not.
 */
float approach(float current, float target, float dt, float seconds) {
    if (seconds <= 0.0f) return target;
    const float stepSize = dt / seconds;
    if (target > current) return std::min(target, current + stepSize);
    return std::max(target, current - stepSize);
}

/// The cutoff an *unoccluded* source sits at. Not "no filter": the filter node is in the
/// graph either way once a source is occludable, so what open means is a cutoff above
/// everything the mix can carry. Nyquist would ring, so this stops short of it.
float openCutoffHz(uint32_t sampleRate) { return std::min(20000.0f, static_cast<float>(sampleRate) * 0.45f); }

ma_attenuation_model toMa(AudioAttenuation model) {
    switch (model) {
    case AudioAttenuation::None: return ma_attenuation_model_none;
    case AudioAttenuation::Linear: return ma_attenuation_model_linear;
    case AudioAttenuation::Exponential: return ma_attenuation_model_exponential;
    case AudioAttenuation::Inverse: break;
    }
    return ma_attenuation_model_inverse;
}

/// The one list of backend names (D12), canonical first. `auto` is first and is also the
/// row's default, which is what a refusal leaves standing.
constexpr core::Named<AudioBackend> kAudioBackends[] = {
    {"auto", AudioBackend::Auto},
    {"device", AudioBackend::Device},
    {"null", AudioBackend::Null},
};
static_assert(core::namesEveryValue(kAudioBackends), "a backend reachable from the enum and from no name");

} // namespace

core::Names<AudioBackend> audioBackendNames() {
    return kAudioBackends;
}

/**
 * @brief Everything miniaudio owns.
 *
 * Voices and buses are held by pointer rather than by value, and that is load-bearing
 * rather than tidy: a `ma_sound` registers itself into the engine's node graph by
 * address, so a `std::vector<Voice>` that reallocated would leave the graph pointing at
 * freed memory and the symptom would be a crash inside the mixer on the frame a scene
 * happened to declare one more sound than the last one. `InstanceTable` can grow because
 * nothing holds a pointer into it; this cannot.
 */
struct AudioEngine::Impl {
    AudioConfig cfg;
    bool running = false;
    bool device = false;
    bool isMuted = false;

    ma_resource_manager resources{};
    ma_engine engine{};
    bool resourcesReady = false;
    bool engineReady = false;

    struct Bus {
        AudioBusDesc desc;
        ma_sound_group group{};
        bool ready = false;
        /// Index of the bus that ducks this one, or kMasterBus for none. Resolved once at
        /// init, because a name resolved per frame is a string compare per frame.
        uint32_t duckedBy = AudioEngine::kMasterBus;
        /// How far into the duck this bus currently is, 0 to 1.
        float duck = 0.0f;
        float gain = 1.0f;
        /// What the last log said, so the transition is reported and the slew between is
        /// not -- the same policy the occlusion line and the dropped-step warning follow.
        bool duckingReported = false;
    };
    std::vector<std::unique_ptr<Bus>> buses;

    struct Voice {
        /// The lifetime pair (C1). Generation starts at 1 because `Handle::valid()`
        /// reserves 0 for "never issued".
        uint32_t generation = 1;
        bool live = true;
        AudioSourceDesc desc;
        ma_sound sound{};
        bool ready = false;
        /// Present only for a source that is both spatial and occludable, so a bed and a
        /// source the author excluded pay for no filter at all.
        ma_lpf_node lpf{};
        bool hasFilter = false;
        float filterCutoff = 0.0f;
        uint32_t bus = AudioEngine::kMasterBus;
        bool streamed = false;
        float seconds = 0.0f;
        /// Fired by `playAt` rather than declared by a node (G7), which is the whole of
        /// what makes `update()` retire it when it reaches its end. A flag rather than a
        /// second container because everything else about the two is identical -- the same
        /// slot, the same generation, the same graph, the same `destroy`.
        bool oneShot = false;
        bool occludedNow = false;
        float occlusionState = 0.0f;
        /// What the last log said, so the transition is reported and the steady state is
        /// not. A message at 60 Hz drowns the log it is trying to appear in, which is the
        /// same policy the dropped-step warning and the particle budget both follow.
        bool occludedReported = false;
    };
    std::vector<std::unique_ptr<Voice>> voices;
    /// Retired slots, and slots whose `ma_sound` is not torn down yet. A slot only moves
    /// from the second list to the first inside `update()`, which is the audio equivalent
    /// of the physics step boundary.
    std::vector<uint32_t> freeVoiceSlots;
    std::vector<uint32_t> pendingVoiceRemoval;

    /// One per listener, sized at `init` and never resized. Kept beside miniaudio's own
    /// copy because `listenerPosition` is read by the occlusion sweep once per source per
    /// frame and `ma_engine_listener_get_position` returns by value through two indirections.
    std::vector<glm::vec3> listeners{glm::vec3(0.0f)};

    uint64_t decodedBytes = 0;
    uint32_t refused = 0;
    uint32_t forcedStreams = 0;

    /// Files already counted against the decode budget.
    ///
    /// **miniaudio's resource manager caches a decoded buffer by path**, so forty sources
    /// on one file decode it once and share it. Counting per *source* would therefore
    /// have charged the budget forty times for one allocation -- a scene refused memory
    /// it never spent, which is the exact opposite of what 0.9's stated budgets are for.
    /// Found by measuring: eight sources on one 58-second asset reported 169.9 MiB and
    /// the process had grown by a twenty-first of that.
    ///
    /// A vector rather than a set because the list is one entry per distinct sound in the
    /// scene -- single digits in every scene that exists -- and a linear scan over eight
    /// strings at load time is not worth a hash table.
    std::vector<std::string> decodedFiles;

    [[nodiscard]] bool alreadyDecoded(const std::string& file) const {
        return std::find(decodedFiles.begin(), decodedFiles.end(), file) != decodedFiles.end();
    }

    /// Seconds per asset, or **negative for a file that would not open** (G7).
    ///
    /// A map rather than the vector `decodedFiles` is, and the difference is the access
    /// pattern rather than taste: that one is walked when a scene loads and this one is
    /// walked every time a one-shot fires, which for a game playing impacts is several
    /// times a step. The negative entry earns its place for the same reason -- a bad path
    /// fired on every contact is a warning sixty times a second, and remembering the
    /// refusal makes it a warning once.
    std::unordered_map<std::string, float> durationByFile;

    /// Whether the voice budget's refusal has already been said. The transition is
    /// reported and the steady state is not, exactly as the duck and the occlusion states
    /// are: a full mixer stays full for a run of calls, and one line each would be the
    /// whole of what the log contained.
    bool voiceBudgetReported = false;

    /// Where the device-less backend's frames go. Sized once from a step's worth of
    /// audio; the mix is discarded, because what is being exercised is the *mixer*.
    std::vector<float> mixBuffer;
    /// Fractional frames carried between steps, so a step of 1/60 at 48 kHz mixes 800
    /// frames every step rather than 800 frames most steps and 799 sometimes.
    double frameCarry = 0.0;
    float peak = 0.0f;
    float rms = 0.0f;

    /// A copy of every mix, for the recorder (S7). Written from the audio thread inside
    /// `onProcess`; read by the recorder's worker. Inert until `startCapture`.
    core::AudioTap tap;

    [[nodiscard]] ma_node* busNode(uint32_t bus) {
        if (bus >= buses.size()) return ma_engine_get_endpoint(&engine);
        return reinterpret_cast<ma_node*>(&buses[bus]->group);
    }

    [[nodiscard]] ma_sound_group* busGroup(uint32_t bus) {
        return bus >= buses.size() ? nullptr : &buses[bus]->group;
    }
};

AudioEngine::AudioEngine() : impl(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init(const AudioConfig& cfg) {
    shutdown();
    impl->cfg = cfg;
    if (!cfg.enabled) {
        core::Logger::status(core::LogCategory::Audio, "Audio: disabled by config");
        return false;
    }

    const bool wantDevice = cfg.backend != AudioBackend::Null;

    // The resource manager is ours rather than the engine's own, for one reason: the
    // decoded format. Naming it here converts every asset to the mix format *once, at
    // load*, so a scene mixing a 44.1 kHz effect with a 48 kHz bed resamples twice at
    // startup instead of twice per buffer forever.
    ma_resource_manager_config rmConfig = ma_resource_manager_config_init();
    rmConfig.decodedFormat = ma_format_f32;
    rmConfig.decodedChannels = cfg.channels;
    rmConfig.decodedSampleRate = cfg.sampleRate;
    if (!wantDevice) {
        // No job thread at all under the device-less backend, and the streams are paged
        // in from `update()` instead. That is what makes this mode deterministic and what
        // keeps it clean under ThreadSanitizer -- the same reason `physics.workerThreads`
        // defaults to zero.
        rmConfig.jobThreadCount = 0;
        rmConfig.flags |= MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
    }
    if (ma_resource_manager_init(&rmConfig, &impl->resources) != MA_SUCCESS) {
        core::Logger::error(core::LogCategory::Audio, "Audio: could not create the resource manager -- audio is off");
        return false;
    }
    impl->resourcesReady = true;

    ma_engine_config engineConfig = ma_engine_config_init();
    // The tap, and it is one line because miniaudio already has the hook: `onProcess`
    // fires at the end of every `ma_engine_read_pcm_frames` with the frames that were
    // just mixed. With a device that call is the device's own data callback, so this runs
    // on the audio thread; without one it is the step below. Same samples, same place,
    // one code path -- and nothing reads the driver's buffers behind its back.
    engineConfig.onProcess = [](void* user, float* frames, ma_uint64 frameCount) {
        // Named from inside the callback because that is the only code of ours that runs
        // on miniaudio's device thread -- it is created by the driver, not by us, so there
        // is no spawn site to name it at. `nameThread` compares pointers and returns
        // immediately once the name is set, which is what makes it safe to call at the
        // mix rate.
        core::Profiler::nameThread("audio device");
        static_cast<Impl*>(user)->tap.write(frames, frameCount);
    };
    engineConfig.pProcessUserData = impl.get();
    engineConfig.pResourceManager = &impl->resources;
    engineConfig.channels = cfg.channels;
    engineConfig.sampleRate = cfg.sampleRate;
    // Clamped rather than refused: a game asking for five ears in a build of miniaudio that
    // holds four is a game that wants as many as it can have, and `listenerCount()` reports
    // what it got.
    const uint32_t wanted = std::clamp(cfg.listeners, 1u, static_cast<uint32_t>(MA_ENGINE_MAX_LISTENERS));
    engineConfig.listenerCount = wanted;
    impl->listeners.assign(wanted, glm::vec3(0.0f));

    bool started = false;
    if (wantDevice) {
        started = ma_engine_init(&engineConfig, &impl->engine) == MA_SUCCESS;
        if (started) impl->device = true;
    }
    if (!started) {
        // The fallback is not silence and it is not a second code path. Everything past
        // this point -- decoders, spatializer, filters, buses -- is the same objects
        // doing the same work; only the thing pulling frames out of the endpoint differs.
        if (wantDevice && cfg.backend != AudioBackend::Null) {
            core::Logger::warn(core::LogCategory::Audio,
                         "Audio: no playback device could be opened -- mixing without one, so every stream, "
                         "duck and filter still runs and nothing reaches a speaker");
        }
        engineConfig.noDevice = MA_TRUE;
        if (ma_engine_init(&engineConfig, &impl->engine) != MA_SUCCESS) {
            core::Logger::error(core::LogCategory::Audio, "Audio: could not create the mixer -- audio is off");
            ma_resource_manager_uninit(&impl->resources);
            impl->resourcesReady = false;
            return false;
        }
        impl->device = false;
        // One step's worth at the slowest step the engine ships, with headroom. Sized
        // once because the alternative is an allocation inside the simulation loop.
        impl->mixBuffer.assign(static_cast<size_t>(cfg.sampleRate) * cfg.channels / 4, 0.0f);
    }
    impl->engineReady = true;
    impl->running = true;
    ma_engine_set_volume(&impl->engine, cfg.masterVolume);

    // The three every project invents anyway. Named here rather than left to the config
    // so that a source authored with `"bus": "sfx"` works against a file nobody edited,
    // which is the same reason every action in the input map ships with a default key.
    std::vector<AudioBusDesc> descs = cfg.buses;
    if (descs.empty()) {
        descs.push_back({"music", 1.0f, "", 1.0f, 0.05f, 0.4f});
        descs.push_back({"sfx", 1.0f, "", 1.0f, 0.05f, 0.4f});
        descs.push_back({"ambience", 1.0f, "", 1.0f, 0.05f, 0.4f});
    }
    for (const AudioBusDesc& desc : descs) {
        auto bus = std::make_unique<Impl::Bus>();
        bus->desc = desc;
        bus->gain = desc.volume;
        if (ma_sound_group_init(&impl->engine, 0, nullptr, &bus->group) != MA_SUCCESS) {
            core::Logger::warn(core::LogCategory::Audio, "Audio: bus '%s' could not be created -- its sources go to master",
                         desc.name.c_str());
            continue;
        }
        bus->ready = true;
        ma_sound_group_set_volume(&bus->group, desc.volume);
        impl->buses.push_back(std::move(bus));
    }

    // Resolved after every bus exists, so a bus may be ducked by one declared below it.
    for (std::unique_ptr<Impl::Bus>& bus : impl->buses) {
        if (bus->desc.duckedBy.empty()) continue;
        bus->duckedBy = findBus(bus->desc.duckedBy);
        if (bus->duckedBy == kMasterBus) {
            core::Logger::warn(core::LogCategory::Audio, "Audio: bus '%s' is ducked by '%s', which no bus claims -- ignored",
                         bus->desc.name.c_str(), bus->desc.duckedBy.c_str());
        }
    }

    core::Logger::status(core::LogCategory::Audio, "Audio: miniaudio %s, %u Hz %u ch, %zu buses, %s", MA_VERSION_STRING,
                   cfg.sampleRate, cfg.channels, impl->buses.size(),
                   impl->device ? "playback device" : "no device (mixing only)");
    return true;
}

void AudioEngine::shutdown() {
    if (impl == nullptr) return;

    // Sounds before their filters before their buses before the engine. Every one of
    // these is a node in the graph holding a pointer to the next, and tearing down in
    // any other order is a read of freed memory rather than an error code.
    for (std::unique_ptr<Impl::Voice>& voice : impl->voices) {
        if (voice->ready) ma_sound_uninit(&voice->sound);
        if (voice->hasFilter) ma_lpf_node_uninit(&voice->lpf, nullptr);
    }
    impl->voices.clear();
    impl->freeVoiceSlots.clear();
    impl->pendingVoiceRemoval.clear();

    for (std::unique_ptr<Impl::Bus>& bus : impl->buses) {
        if (bus->ready) ma_sound_group_uninit(&bus->group);
    }
    impl->buses.clear();

    if (impl->engineReady) {
        // Before the tap is released: uninit stops the device, and the device's thread is
        // what writes into it. Freeing the storage first would be a use-after-free with a
        // window of exactly one audio period, which is the kind that reproduces on
        // somebody else's machine.
        ma_engine_uninit(&impl->engine);
        impl->engineReady = false;
    }
    impl->tap.stop();
    if (impl->resourcesReady) {
        ma_resource_manager_uninit(&impl->resources);
        impl->resourcesReady = false;
    }

    impl->running = false;
    impl->device = false;
    impl->decodedBytes = 0;
    impl->decodedFiles.clear();
    impl->durationByFile.clear();
    impl->voiceBudgetReported = false;
    impl->refused = 0;
    impl->forcedStreams = 0;
}

bool AudioEngine::active() const { return impl->running; }
bool AudioEngine::usingDevice() const { return impl->device; }

void AudioEngine::startCapture(float seconds) {
    const auto frames = static_cast<uint64_t>(static_cast<double>(impl->cfg.sampleRate) * seconds);
    impl->tap.start(impl->cfg.channels, frames);
}

void AudioEngine::stopCapture() { impl->tap.stop(); }
core::AudioTap* AudioEngine::captureTap() { return &impl->tap; }
bool AudioEngine::capturing() const { return impl->tap.active(); }
uint64_t AudioEngine::readCaptured(float* dst, uint64_t maxFrames) { return impl->tap.read(dst, maxFrames); }
uint64_t AudioEngine::capturedDropped() const { return impl->tap.dropped(); }
uint32_t AudioEngine::sampleRate() const { return impl->cfg.sampleRate; }
uint32_t AudioEngine::channelCount() const { return impl->cfg.channels; }
bool AudioEngine::empty() const { return impl->voices.empty(); }
uint32_t AudioEngine::sourceCount() const { return static_cast<uint32_t>(impl->voices.size()); }
// The nine accessors that take a source index are bounds-checked, the same way busName()
// and busGain() already were. The reason is not defensive habit: `addSource` returns
// `kNoSource` on a file it could not open or a voice the budget refused, `kNoSource` is
// 0xFFFFFFFF rather than an unrepresentable value, and a caller that stores the result
// and asks a question about it later is the ordinary way to use this class. An index that
// came from a failure is therefore reachable at every one of these, and each returns the
// same do-nothing answer it would give for a source that exists and is silent.
bool AudioEngine::valid(SoundId id) const {
    return id.valid() && id.index < impl->voices.size() && impl->voices[id.index]->generation == id.generation &&
           impl->voices[id.index]->live;
}

SoundId AudioEngine::soundAt(uint32_t slot) const {
    if (slot >= impl->voices.size() || !impl->voices[slot]->live) return {};
    return SoundId{slot, impl->voices[slot]->generation};
}

const AudioSourceDesc& AudioEngine::source(SoundId id) const {
    static const AudioSourceDesc none;
    return valid(id) ? impl->voices[id.index]->desc : none;
}
bool AudioEngine::sourceStreamed(SoundId id) const { return valid(id) && impl->voices[id.index]->streamed; }
float AudioEngine::sourceSeconds(SoundId id) const { return valid(id) ? impl->voices[id.index]->seconds : 0.0f; }
uint64_t AudioEngine::decodedBytes() const { return impl->decodedBytes; }
uint32_t AudioEngine::refusedSources() const { return impl->refused; }
uint32_t AudioEngine::budgetForcedStreams() const { return impl->forcedStreams; }
float AudioEngine::lastPeak() const { return impl->peak; }
float AudioEngine::lastRms() const { return impl->rms; }
bool AudioEngine::muted() const { return impl->isMuted; }
uint32_t AudioEngine::busCount() const { return static_cast<uint32_t>(impl->buses.size()); }
glm::vec3 AudioEngine::listenerPosition(uint32_t listener) const {
    return listener < impl->listeners.size() ? impl->listeners[listener] : glm::vec3(0.0f);
}

uint32_t AudioEngine::listenerCount() const { return static_cast<uint32_t>(impl->listeners.size()); }

uint32_t AudioEngine::streamedCount() const {
    uint32_t n = 0;
    for (const std::unique_ptr<Impl::Voice>& voice : impl->voices) n += (voice->live && voice->streamed) ? 1u : 0u;
    return n;
}

uint32_t AudioEngine::decodedCount() const { return sourceCount() - streamedCount(); }

const std::string& AudioEngine::busName(uint32_t bus) const {
    static const std::string master = "master";
    return bus < impl->buses.size() ? impl->buses[bus]->desc.name : master;
}

float AudioEngine::busGain(uint32_t bus) const { return bus < impl->buses.size() ? impl->buses[bus]->gain : 1.0f; }

uint32_t AudioEngine::findBus(const std::string& name) const {
    for (size_t i = 0; i < impl->buses.size(); ++i) {
        if (impl->buses[i]->desc.name == name) return static_cast<uint32_t>(i);
    }
    return kMasterBus;
}

bool AudioEngine::stealVoice() {
    constexpr uint32_t kNone = 0xFFFFFFFFu;

    // Two passes rather than one scoring function: "a one-shot before a placed source" and
    // "the quietest of those" are different questions, and folding them into one comparison
    // is where a loud footstep starts outranking a quiet ambience.
    const auto quietest = [&](bool oneShotOnly) {
        uint32_t best = kNone;
        float lowest = 0.0f;
        for (uint32_t i = 0; i < impl->voices.size(); ++i) {
            const Impl::Voice& v = *impl->voices[i];
            if (!v.live || v.desc.loop) continue;
            if (oneShotOnly && !v.oneShot) continue;
            if (best == kNone || v.desc.volume < lowest) {
                best = i;
                lowest = v.desc.volume;
            }
        }
        return best;
    };

    uint32_t victim = quietest(true);
    if (victim == kNone) victim = quietest(false);
    if (victim == kNone) return false;

    destroy(SoundId{victim, impl->voices[victim]->generation});
    return true;
}

SoundId AudioEngine::create(const AudioSourceDesc& desc) {
    if (!impl->running) return {};

    // Live voices, not slots. The budget bounds what is playing, which is what it always
    // meant -- before C1 nothing could be retired, so the two were the same number.
    //
    // **A floor now, and it grows** (C40). Unlike the other three pools there is no
    // allocation behind this number -- `voices` is a vector of `unique_ptr`s that already
    // grows -- so what the budget actually bounds is *mixing cost*, which is a property of
    // the machine rather than of the game. So it doubles rather than refusing, and the hard
    // stop is `kMaxVoices`: a number about what a mixer can carry, not a guess a game made
    // in `configure()`.
    const uint32_t livingVoices = static_cast<uint32_t>(impl->voices.size() - impl->freeVoiceSlots.size());
    if (livingVoices >= impl->cfg.voiceBudget) {
        if (impl->cfg.voiceBudget < kMaxVoices) {
            impl->cfg.voiceBudget = std::min(std::max(impl->cfg.voiceBudget * 2u, 1u), kMaxVoices);
            core::Logger::status(core::LogCategory::Audio, "Audio: voices grown to %u", impl->cfg.voiceBudget);
        } else if (!stealVoice()) {
            // Every voice is looping, so there is nothing whose loss would be momentary.
            // This is the one refusal left, and it is a real one rather than a budget
            // somebody guessed low.
            ++impl->refused;
            if (!impl->voiceBudgetReported) {
                impl->voiceBudgetReported = true;
                core::Logger::warn(core::LogCategory::Audio,
                                   "Audio source '%s': refused, all %u voices are looping and none can be stolen",
                                   desc.name.c_str(), kMaxVoices);
            }
            return {};
        }
    }
    impl->voiceBudgetReported = false;

    // Opened as a decoder first, and for two answers at once: whether the file exists and
    // can be decoded at all, and how long it is. The second is what the stream-versus-
    // decode decision turns on, and it cannot be taken after the sound is created --
    // creating it is the thing being decided about.
    //
    // Both answers are cached by path, because neither can change while the engine is up
    // and `playAt` asks them again on every impact. Without it a game firing a one-shot per
    // contact opens and parses a file per contact, which is the one cost in this function
    // that is a syscall rather than arithmetic.
    float seconds = 0.0f;
    const auto known = impl->durationByFile.find(desc.file);
    if (known != impl->durationByFile.end()) {
        // Negative is the remembered refusal. Counted again -- the source really was
        // refused -- and not said again.
        if (known->second < 0.0f) {
            ++impl->refused;
            return {};
        }
        seconds = known->second;
    } else {
        ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, impl->cfg.channels, impl->cfg.sampleRate);
        ma_decoder decoder;
        if (ma_decoder_init_file(desc.file.c_str(), &decoderConfig, &decoder) != MA_SUCCESS) {
            ++impl->refused;
            impl->durationByFile[desc.file] = -1.0f;
            core::Logger::warn(core::LogCategory::Audio, "Audio source '%s': could not open '%s' -- skipped", desc.name.c_str(),
                         desc.file.c_str());
            return {};
        }
        ma_uint64 frames = 0;
        // A length the container does not state cheaply -- an MP3 without a Xing header
        // is the usual one -- leaves this zero, and zero streams. See audioShouldStream.
        if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS && frames > 0) {
            seconds = static_cast<float>(static_cast<double>(frames) / impl->cfg.sampleRate);
        }
        ma_decoder_uninit(&decoder);
        impl->durationByFile[desc.file] = seconds;
    }

    bool stream = audioShouldStream(desc.load, seconds, impl->cfg.streamThresholdSeconds);
    // Zero for a file some other source already decoded, because the resource manager
    // will hand this one the same buffer -- see Impl::decodedFiles.
    const uint64_t bytes = impl->alreadyDecoded(desc.file)
                               ? 0
                               : audioDecodedBytes(seconds, impl->cfg.sampleRate, impl->cfg.channels);

    // The budget only ever pushes a source the *other* way, and it says so when it does.
    // That is 0.9's rule applied to memory: a stated budget that binds is reported, and a
    // source is never silently refused for being large -- it plays, it just plays the
    // other way.
    if (!stream && impl->cfg.decodeBudgetBytes > 0 && impl->decodedBytes + bytes > impl->cfg.decodeBudgetBytes) {
        stream = true;
        ++impl->forcedStreams;
        core::Logger::warn(core::LogCategory::Audio,
                     "Audio source '%s': %.1f MiB decoded would pass the %.1f MiB budget -- streaming it instead",
                     desc.name.c_str(), static_cast<double>(bytes) / (1024.0 * 1024.0),
                     static_cast<double>(impl->cfg.decodeBudgetBytes) / (1024.0 * 1024.0));
    }

    auto voice = std::make_unique<Impl::Voice>();
    voice->desc = desc;
    voice->streamed = stream;
    voice->seconds = seconds;
    voice->bus = desc.bus.empty() ? kMasterBus : findBus(desc.bus);
    if (!desc.bus.empty() && voice->bus == kMasterBus) {
        core::Logger::warn(core::LogCategory::Audio, "Audio source '%s': bus '%s' does not exist -- playing on master",
                     desc.name.c_str(), desc.bus.c_str());
    }

    ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
    if (!desc.spatial) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

    if (ma_sound_init_from_file(&impl->engine, desc.file.c_str(), flags, impl->busGroup(voice->bus), nullptr,
                                &voice->sound) != MA_SUCCESS) {
        ++impl->refused;
        core::Logger::warn(core::LogCategory::Audio, "Audio source '%s': could not load '%s' -- skipped", desc.name.c_str(),
                     desc.file.c_str());
        return {};
    }
    voice->ready = true;
    if (!stream && bytes > 0) {
        impl->decodedBytes += bytes;
        impl->decodedFiles.push_back(desc.file);
    }

    // A stream's first pages are filled by a job, and under the device-less backend there
    // is no thread to run it. Drained here rather than left to the first `update()` so
    // that a source which reports itself loaded is one that can actually be read from --
    // otherwise the opening tenth of a second of every stream is silence, which is
    // exactly the kind of defect that only shows up as "it sounds slightly wrong".
    if (stream && !impl->device) {
        while (ma_resource_manager_process_next_job(&impl->resources) == MA_SUCCESS) {
        }
    }

    ma_sound_set_volume(&voice->sound, desc.volume);
    ma_sound_set_pitch(&voice->sound, desc.pitch);
    ma_sound_set_looping(&voice->sound, desc.loop ? MA_TRUE : MA_FALSE);

    if (desc.spatial) {
        ma_sound_set_attenuation_model(&voice->sound, toMa(desc.attenuation));
        ma_sound_set_min_distance(&voice->sound, desc.minDistance);
        // Left at miniaudio's own unbounded default when the schema says 0, rather than
        // written as some large number that would then be a second place the meaning of
        // "no cap" was decided.
        if (desc.maxDistance > 0.0f) ma_sound_set_max_distance(&voice->sound, desc.maxDistance);
        ma_sound_set_rolloff(&voice->sound, desc.rolloff);
        ma_sound_set_cone(&voice->sound, desc.coneInnerAngle, desc.coneOuterAngle, desc.coneOuterGain);
        ma_sound_set_doppler_factor(&voice->sound, desc.dopplerFactor);

        // The filter node goes in at load or never. Inserting one later would mean
        // re-plumbing a running graph on the frame a wall first came between a source and
        // the listener, which is exactly the frame that must not glitch.
        if (impl->cfg.occlusion && desc.occlusion) {
            ma_lpf_node_config lpfConfig = ma_lpf_node_config_init(impl->cfg.channels, impl->cfg.sampleRate,
                                                                   openCutoffHz(impl->cfg.sampleRate), 2);
            if (ma_lpf_node_init(ma_engine_get_node_graph(&impl->engine), &lpfConfig, nullptr, &voice->lpf) ==
                MA_SUCCESS) {
                voice->hasFilter = true;
                voice->filterCutoff = openCutoffHz(impl->cfg.sampleRate);
                // sound -> filter -> bus, replacing sound -> bus. Both attachments are
                // needed and in this order: the filter has to have somewhere to go before
                // the sound is pointed at it.
                ma_node_attach_output_bus(&voice->lpf, 0, impl->busNode(voice->bus), 0);
                ma_node_attach_output_bus(&voice->sound, 0, &voice->lpf, 0);
            } else {
                core::Logger::warn(core::LogCategory::Audio,
                             "Audio source '%s': no filter could be created, so it will duck rather than muffle "
                             "when occluded",
                             desc.name.c_str());
            }
        }
    }

    // The slot is taken now, and the voice is placed into it before anything asks about
    // it. The old version called setSourceTransform() with `voices.size()` -- one past the
    // end -- and that accessor carried a special case to tolerate it; placing first is
    // what lets the special case go.
    uint32_t slot;
    if (!impl->freeVoiceSlots.empty()) {
        slot = impl->freeVoiceSlots.back();
        impl->freeVoiceSlots.pop_back();
        // The generation was moved by destroy(); carry it across the reused storage.
        voice->generation = impl->voices[slot]->generation;
        impl->voices[slot] = std::move(voice);
    } else {
        slot = static_cast<uint32_t>(impl->voices.size());
        impl->voices.push_back(std::move(voice));
    }

    const SoundId id{slot, impl->voices[slot]->generation};
    setSourceTransform(id, desc.transform);
    if (desc.autoplay) ma_sound_start(&impl->voices[slot]->sound);
    return id;
}

SoundId AudioEngine::playAt(const AudioSourceDesc& desc, const glm::vec3& position) {
    if (!impl->running) return {};

    AudioSourceDesc one = desc;
    // Overwritten rather than checked, and the caller is not told off for setting them.
    // What `playAt` means is "play this once, here"; a desc that said `loop` would be a
    // desc that meant something the verb does not, and refusing it would only make every
    // call site zero two fields it never wanted.
    one.loop = false;
    one.autoplay = true;
    one.transform = glm::translate(glm::mat4(1.0f), position);

    const SoundId id = create(one);
    // Marked after the fact rather than threaded through `create` as a parameter: the flag
    // changes nothing about how the voice is built, only who retires it, and a `bool
    // oneShot` argument on the create path would be a mode flag on a function that has no
    // modes.
    if (id.valid()) impl->voices[id.index]->oneShot = true;
    return id;
}

void AudioEngine::setSourceTransform(SoundId id, const glm::mat4& transform) {
    if (!valid(id)) return;
    const glm::vec3 position(transform[3]);

    Impl::Voice& voice = *impl->voices[id.index];
    voice.desc.transform = transform;
    if (!voice.ready || !voice.desc.spatial) return;
    ma_sound_set_position(&voice.sound, position.x, position.y, position.z);
    // glTF's forward is -Z, so a node rotated to face something aims its cone at it.
    const glm::vec3 forward = -glm::vec3(transform[2]);
    ma_sound_set_direction(&voice.sound, forward.x, forward.y, forward.z);
}

glm::vec3 AudioEngine::sourcePosition(SoundId id) const {
    return valid(id) ? glm::vec3(impl->voices[id.index]->desc.transform[3]) : glm::vec3(0.0f);
}

bool AudioEngine::sourcePlaying(SoundId id) const {
    if (!valid(id)) return false;
    const Impl::Voice& voice = *impl->voices[id.index];
    return voice.ready && ma_sound_is_playing(const_cast<ma_sound*>(&voice.sound)) == MA_TRUE;
}

void AudioEngine::start(SoundId id) {
    if (valid(id) && impl->voices[id.index]->ready) ma_sound_start(&impl->voices[id.index]->sound);
}

void AudioEngine::stop(SoundId id) {
    if (valid(id) && impl->voices[id.index]->ready) ma_sound_stop(&impl->voices[id.index]->sound);
}

void AudioEngine::destroy(SoundId id) {
    if (!valid(id)) return;

    Impl::Voice& voice = *impl->voices[id.index];
    // Silenced now, torn down later. Stopping is safe from here; uninit walks the node
    // graph the device thread is also walking, so it waits for update().
    if (voice.ready) ma_sound_stop(&voice.sound);
    voice.live = false;
    ++voice.generation;
    impl->pendingVoiceRemoval.push_back(id.index);
}

void AudioEngine::setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up,
                              uint32_t listener) {
    if (!impl->running || listener >= impl->listeners.size()) return;
    impl->listeners[listener] = position;
    ma_engine_listener_set_position(&impl->engine, listener, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&impl->engine, listener, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&impl->engine, listener, up.x, up.y, up.z);
}

void AudioEngine::setOccluded(SoundId id, bool occluded) {
    if (valid(id)) impl->voices[id.index]->occludedNow = occluded;
}

float AudioEngine::occlusion(SoundId id) const {
    return valid(id) ? impl->voices[id.index]->occlusionState : 0.0f;
}

bool AudioEngine::occludable(SoundId id) const {
    if (!valid(id)) return false;
    const Impl::Voice& voice = *impl->voices[id.index];
    return impl->cfg.occlusion && voice.desc.occlusion && voice.desc.spatial && voice.ready;
}

void AudioEngine::setMuted(bool muted) {
    impl->isMuted = muted;
    if (impl->running) ma_engine_set_volume(&impl->engine, muted ? 0.0f : impl->cfg.masterVolume);
}

void AudioEngine::update(float dt) {
    // Above the early-out and named for the function, which is the rule every other zone
    // in the frame follows: a mixer that is not running has to cost a named zero, or the
    // gap between `simulate` and the sum of its children cannot be read as work no zone
    // names. It was `audio`, below the return, and it was the only zone in `simulate` that
    // disappeared instead of reporting nothing.
    auto scope = core::Profiler::scope("AudioEngine::update");
    if (!impl->running) return;

    // The reclaim boundary (C1), and it goes first for the same reason PhysicsWorld's
    // does: `destroy` cannot call `ma_sound_uninit` itself, because that walks a node
    // graph the device thread is also walking. Here, between mixes, it can.
    for (const uint32_t slot : impl->pendingVoiceRemoval) {
        Impl::Voice& voice = *impl->voices[slot];
        // Sound before filter, the same order shutdown() tears the whole graph down in:
        // each is a node holding a pointer to the next.
        if (voice.ready) ma_sound_uninit(&voice.sound);
        if (voice.hasFilter) ma_lpf_node_uninit(&voice.lpf, nullptr);
        const uint32_t generation = voice.generation;
        // The storage stays; only the contents are reset. `voices` holds unique_ptrs
        // because the mixer knows a voice by address, so a slot's address must outlive
        // every sound that ever occupies it.
        *impl->voices[slot] = Impl::Voice{};
        impl->voices[slot]->generation = generation;
        impl->voices[slot]->live = false;
        impl->freeVoiceSlots.push_back(slot);
    }
    impl->pendingVoiceRemoval.clear();

    // ----------------------------------------------------------- one-shots (G7)
    // A voice fired at an event has no owner to call `destroy` for it, and a game that had
    // to keep a list of the sounds it fired would be keeping exactly the bookkeeping
    // `playAt` exists to remove. So it retires itself, through the same deferred path
    // everything else here uses -- which is one step of latency on a voice that is already
    // silent.
    //
    // `at_end` rather than `!is_playing`, and the difference is the frame a shot is fired
    // on: a sound that has not started yet is not playing either, and this loop would
    // reclaim it before it made a sound.
    for (uint32_t slot = 0; slot < impl->voices.size(); ++slot) {
        Impl::Voice& voice = *impl->voices[slot];
        if (!voice.live || !voice.oneShot || !voice.ready) continue;
        if (ma_sound_at_end(&voice.sound) == MA_TRUE) destroy(SoundId{slot, voice.generation});
    }

    // ------------------------------------------------- which ears hear it (C28)
    /*
     * **The nearest listener, chosen here, because miniaudio cannot be asked to.**
     * `ma_engine_node_config_init` zeroes its config, and zero is a *valid* listener index
     * rather than the `MA_LISTENER_INDEX_CLOSEST` sentinel -- so every sound is pinned to
     * listener 0 for its whole life. `ma_sound_set_pinned_listener_index` cannot undo that:
     * it refuses any index at or past the listener count, and the sentinel is 255.
     * `ma_sound_config` has no field for it either. Measured before this loop existed: a
     * source one metre from listener 1 and thirty from listener 0 mixed at exactly the
     * thirty-metre level, so a second pair of ears was created, positioned, drawn and never
     * heard from.
     *
     * Nearest rather than summed is this row's decision and not miniaudio's leftover.
     * Summing would make a room louder as players are added and would double a sound both
     * can hear, so a source's loudness would stop being a property of the scene. Nearest
     * means a sound beside player two is heard at player two's volume, which is what split
     * screen wants and what a top-down game with the ears on its cursor wants.
     *
     * Skipped entirely at one listener, which is every scene in this tree: the pin is
     * already 0 and there is nothing to choose between.
     */
    if (impl->listeners.size() > 1) {
        for (const std::unique_ptr<Impl::Voice>& voice : impl->voices) {
            if (!voice->live || !voice->ready || !voice->desc.spatial) continue;
            const ma_vec3f at = ma_sound_get_position(&voice->sound);
            uint32_t nearest = 0;
            float best = std::numeric_limits<float>::max();
            for (uint32_t ears = 0; ears < impl->listeners.size(); ++ears) {
                // Squared, since only the order matters and a square root per source per
                // listener per frame buys nothing.
                const glm::vec3 delta = impl->listeners[ears] - glm::vec3(at.x, at.y, at.z);
                if (const float reach = glm::dot(delta, delta); reach < best) {
                    best = reach;
                    nearest = ears;
                }
            }
            // Switching mid-playback does not click: the engine's gain smoothing ramps a
            // spatialized voice rather than stepping it, which is the same behaviour the
            // level tests already skip the first quarter of a run to avoid measuring.
            ma_sound_set_pinned_listener_index(&voice->sound, nearest);
        }
    }

    // ------------------------------------------------------------ ducking (S5.4)
    // Which buses have a voice playing, computed once for the whole set rather than per
    // ducked bus: with three buses and forty voices the difference is 120 `is_playing`
    // calls against 40.
    std::vector<bool> busActive(impl->buses.size(), false);
    for (const std::unique_ptr<Impl::Voice>& voice : impl->voices) {
        if (!voice->live || !voice->ready || voice->bus >= busActive.size()) continue;
        if (ma_sound_is_playing(&voice->sound) == MA_TRUE) busActive[voice->bus] = true;
    }

    for (std::unique_ptr<Impl::Bus>& bus : impl->buses) {
        if (!bus->ready) continue;
        const bool ducking = bus->duckedBy < busActive.size() && busActive[bus->duckedBy];
        bus->duck = approach(bus->duck, ducking ? 1.0f : 0.0f, dt,
                             ducking ? bus->desc.duckAttack : bus->desc.duckRelease);
        bus->gain = bus->desc.volume * (1.0f + bus->duck * (bus->desc.duckAmount - 1.0f));
        ma_sound_group_set_volume(&bus->group, bus->gain);

        if (ducking != bus->duckingReported) {
            bus->duckingReported = ducking;
            core::Logger::debug(core::LogCategory::Audio, "Audio: bus '%s' %s '%s' (%.2f)", bus->desc.name.c_str(),
                          ducking ? "ducking under" : "released from",
                          impl->buses[bus->duckedBy]->desc.name.c_str(),
                          static_cast<double>(ducking ? bus->desc.duckAmount : 1.0f));
        }
    }

    // ---------------------------------------------------------- occlusion (S5.5)
    const float open = openCutoffHz(impl->cfg.sampleRate);
    for (std::unique_ptr<Impl::Voice>& voice : impl->voices) {
        if (!voice->live || !voice->ready || !voice->desc.spatial) continue;
        voice->occlusionState = approach(voice->occlusionState, voice->occludedNow ? 1.0f : 0.0f, dt,
                                         voice->occludedNow ? impl->cfg.occlusionAttack : impl->cfg.occlusionRelease);

        if (voice->occludedNow != voice->occludedReported) {
            voice->occludedReported = voice->occludedNow;
            core::Logger::debug(core::LogCategory::Audio, "Audio: '%s' %s", voice->desc.name.c_str(),
                          voice->occludedNow ? "occluded" : "clear");
        }

        const float gain = 1.0f + voice->occlusionState * (impl->cfg.occlusionGain - 1.0f);
        ma_sound_set_volume(&voice->sound, voice->desc.volume * gain);

        if (!voice->hasFilter) continue;
        const float cutoff = open + voice->occlusionState * (impl->cfg.occlusionCutoffHz - open);
        // Reinitialised only when the cutoff has actually moved. A biquad recomputed
        // every frame at an unchanged cutoff is arithmetic nobody asked for, and the
        // state a reinit preserves is what keeps the sweep clickless.
        if (std::abs(cutoff - voice->filterCutoff) > 1.0f) {
            ma_lpf_config lpfConfig =
                ma_lpf_config_init(ma_format_f32, impl->cfg.channels, impl->cfg.sampleRate, cutoff, 2);
            if (ma_lpf_node_reinit(&lpfConfig, &voice->lpf) == MA_SUCCESS) voice->filterCutoff = cutoff;
        }
    }

    if (impl->device) return;

    // ----------------------------------------------- the device-less mix (S5.1)
    // With no job thread, the resource manager's work has to happen somewhere, and this
    // is it. Drained rather than one-per-step: a stream that fell a page behind would
    // otherwise stay a page behind forever.
    while (ma_resource_manager_process_next_job(&impl->resources) == MA_SUCCESS) {
    }

    impl->frameCarry += static_cast<double>(dt) * impl->cfg.sampleRate;
    auto frames = static_cast<ma_uint64>(impl->frameCarry);
    impl->frameCarry -= static_cast<double>(frames);
    const ma_uint64 capacity = impl->mixBuffer.size() / impl->cfg.channels;
    if (frames > capacity) frames = capacity;
    if (frames == 0) return;

    ma_uint64 read = 0;
    if (ma_engine_read_pcm_frames(&impl->engine, impl->mixBuffer.data(), frames, &read) != MA_SUCCESS) return;

    // Peak and RMS of what was just mixed. The samples themselves are discarded -- there
    // is nowhere for them to go -- but the two numbers are what a test asserts on and
    // what proves the graph is doing something rather than running.
    float peak = 0.0f;
    double sum = 0.0;
    const size_t samples = static_cast<size_t>(read) * impl->cfg.channels;
    for (size_t s = 0; s < samples; ++s) {
        const float v = impl->mixBuffer[s];
        peak = std::max(peak, std::abs(v));
        sum += static_cast<double>(v) * v;
    }
    impl->peak = peak;
    impl->rms = samples > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(samples))) : 0.0f;
}

} // namespace scene
