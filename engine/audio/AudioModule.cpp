#include "audio/AudioModule.h"

#include "Engine.h"
#include "Modules.h"

namespace audio {

namespace {

/// The engine's one mixer. File-scope rather than an `Engine` member, which would name
/// `audio::AudioEngine` in `Engine.h` and link miniaudio into every game.
AudioEngine g_audio;

struct Module final : modules::Audio {
    bool init(const scene::AudioConfig& cfg) override { return g_audio.init(cfg); }

    void shutdown() override { g_audio.shutdown(); }

    void create(const scene::AudioSourceDesc& desc) override { (void)g_audio.create(desc); }

    [[nodiscard]] Stats stats() const override {
        Stats s;
        s.sources = g_audio.sourceCount();
        s.streamed = g_audio.streamedCount();
        s.decoded = g_audio.decodedCount();
        s.decodedBytes = g_audio.decodedBytes();
        s.buses = g_audio.busCount();
        s.refused = g_audio.refusedSources();
        s.sampleRate = g_audio.sampleRate();
        s.channels = g_audio.channelCount();
        s.listeners = g_audio.listenerCount();
        s.active = g_audio.active();
        return s;
    }

    [[nodiscard]] bool sourceAt(uint32_t slot, Source* out) const override {
        const scene::SoundId id = g_audio.soundAt(slot);
        if (!id.valid()) return false;
        out->id = id;
        out->desc = &g_audio.source(id);
        out->position = g_audio.sourcePosition(id);
        out->seconds = g_audio.sourceSeconds(id);
        out->occlusion = g_audio.occlusion(id);
        out->streamed = g_audio.sourceStreamed(id);
        return true;
    }

    void setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) override {
        g_audio.setListener(position, forward, up);
    }

    [[nodiscard]] glm::vec3 listenerPosition(uint32_t listener) const override {
        return g_audio.listenerPosition(listener);
    }

    [[nodiscard]] core::Slot<void(scene::SoundId, const glm::mat4&)> sourceTransforms() override {
        return core::Slot<void(scene::SoundId, const glm::mat4&)>::bind<&AudioEngine::setSourceTransform>(&g_audio);
    }

    void placeSources(const core::Slot<bool(uint32_t, glm::mat4*)>& poseOf) override { g_audio.placeSources(poseOf); }

    void updateOcclusion(
        const core::Slot<bool(const glm::vec3&, const glm::vec3&, scene::BodyId)>& blocked) override {
        g_audio.updateOcclusion(blocked);
    }

    void setSourceBody(uint32_t slot, scene::BodyId body) override { g_audio.setSourceBody(slot, body); }

    void clearSourceBodies() override { g_audio.clearSourceBodies(); }

    void update(float dt) override { g_audio.update(dt); }

    [[nodiscard]] core::AudioTap* startCapture(float seconds) override {
        if (!g_audio.active()) return nullptr;
        g_audio.startCapture(seconds);
        return g_audio.captureTap();
    }

    void stopCapture() override { g_audio.stopCapture(); }
};

Module g_module;

/// Assign `modules::audio` from a header instead and any transitive include links miniaudio
/// into a game that never asked for sound.
struct Registrar {
    Registrar() { modules::audio = &g_module; }
};

const Registrar g_registrar;

} // namespace

} // namespace audio

// Defining this in Engine.cpp instead links audio into every binary -- Engine.cpp is in all
// of them and this file is not.
::audio::AudioEngine& Engine::audio() {
    return ::audio::g_audio;
}
