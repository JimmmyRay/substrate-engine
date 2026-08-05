#include "scene/Audio.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace core;

using namespace scene;

namespace fs = std::filesystem;

/**
 * @file tests/AudioTests.cpp
 * @brief The mixer, the buses, the spatializer and the occlusion filter.
 *
 * Every test here runs the **whole** audio path -- the same decoders, resource manager,
 * spatializer, biquads and node graph a run with speakers uses -- with `backend: "null"`,
 * which builds the engine device-less and lets the caller pull the samples out. That is
 * the same decision `window.headless` makes for the renderer: an unmapped window still
 * presents, so what is being checked is the real thing rather than a stub of it. Three
 * consequences, and all three are the reason this file can exist at all:
 *
 * - It needs no sound card, so it runs wherever the rest of the suite runs.
 * - It needs no thread: the resource manager is configured with none and the jobs are
 *   pumped from `update()`, which is what keeps it clean under ThreadSanitizer.
 * - It is deterministic. A fixed step in, a fixed number of frames out.
 *
 * The assets are written by the test rather than read from an asset tree, since neither is in the
 * repository -- a suite that skips itself when content is missing is a suite that has
 * never run on a fresh clone.
 *
 * What is being pinned:
 *
 * 1. **The mixer produces samples**, and muting produces none while everything keeps
 *    running underneath.
 * 2. **Distance attenuates and a bed does not**, which is the whole of `spatial` being a
 *    property of the sound rather than of the bus.
 * 3. **A duck arrives and releases**, over the stated times, in the stated direction.
 * 4. **Occlusion is a slew, not a step.** The failure this catches is a filter cutoff
 *    that jumps -- audible as a click, and invisible to a test that only compares the
 *    settled level.
 * 5. **Budgets refuse and report rather than overrun**, for voices, and push to the other
 *    path rather than refusing, for decoded bytes.
 */

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr float kStep = 1.0f / 60.0f;

/// A 16-bit mono PCM WAV of a sine at `hz`. Hand-rolled because miniaudio's encoder is
/// compiled out (`MA_NO_ENCODING`) and because 44 bytes of header is less machinery than
/// turning it back on would be.
void writeWav(const fs::path& path, float seconds, float hz, float amplitude = 0.5f) {
    const auto rate = static_cast<uint32_t>(kRate);
    const auto frames = static_cast<uint32_t>(seconds * static_cast<float>(rate));
    const uint32_t dataBytes = frames * 2u;

    std::ofstream out(path, std::ios::binary);
    const auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

    out.write("RIFF", 4);
    u32(36u + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16u);
    u16(1u); ///< PCM
    u16(1u); ///< mono
    u32(rate);
    u32(rate * 2u); ///< byte rate
    u16(2u);        ///< block align
    u16(16u);       ///< bits
    out.write("data", 4);
    u32(dataBytes);

    for (uint32_t i = 0; i < frames; ++i) {
        const auto phase = static_cast<float>(2.0 * 3.14159265358979 * hz * i / rate);
        const auto sample = static_cast<int16_t>(std::sin(phase) * amplitude * 32000.0f);
        u16(static_cast<uint16_t>(sample));
    }
}

class AudioTest : public ::testing::Test {
  protected:
    fs::path dir;
    fs::path tone;  ///< 1 s, short enough to decode under any sane threshold
    fs::path longer; ///< 12 s, long enough to stream under the default threshold
    fs::path shot;  ///< 0.1 s, short enough that a one-shot ends inside a test

    void SetUp() override {
        dir = fs::temp_directory_path() / "substrate_audio_tests";
        fs::remove_all(dir);
        fs::create_directories(dir);
        tone = dir / "tone.wav";
        longer = dir / "long.wav";
        shot = dir / "shot.wav";
        writeWav(tone, 1.0f, 440.0f);
        writeWav(longer, 12.0f, 220.0f);
        writeWav(shot, 0.1f, 660.0f);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    static AudioConfig config() {
        AudioConfig cfg;
        cfg.backend = AudioBackend::Null;
        cfg.sampleRate = kRate;
        cfg.channels = kChannels;
        return cfg;
    }

    AudioSourceDesc source(const fs::path& file) const {
        AudioSourceDesc desc;
        desc.name = file.stem().string();
        desc.file = file.string();
        desc.spatial = false; ///< a bed unless a test says otherwise
        desc.loop = true;
        return desc;
    }

    /// Mix `seconds` of audio a step at a time and report the loudest RMS seen after the
    /// first quarter of it. Skipping the opening is deliberate: miniaudio ramps a
    /// spatialized gain across a few milliseconds rather than stepping it, so a level
    /// read from the first block is a level measured mid-fade.
    static float levelOver(AudioEngine& audio, float seconds) {
        const auto steps = static_cast<uint32_t>(seconds / kStep);
        float loudest = 0.0f;
        for (uint32_t i = 0; i < steps; ++i) {
            audio.update(kStep);
            if (i > steps / 4) loudest = std::max(loudest, audio.lastRms());
        }
        return loudest;
    }
};

} // namespace

TEST_F(AudioTest, DeviceLessBackendActuallyMixes) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    EXPECT_TRUE(audio.active());
    EXPECT_FALSE(audio.usingDevice());
    EXPECT_TRUE(audio.empty());

    ASSERT_TRUE(audio.create(source(tone)).valid());
    EXPECT_FALSE(audio.empty());
    EXPECT_TRUE(audio.sourcePlaying(audio.soundAt(0))); ///< autoplay defaults on

    EXPECT_GT(levelOver(audio, 0.2f), 0.01f);
}

TEST_F(AudioTest, MutingIsSilentAndReversible) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    ASSERT_TRUE(audio.create(source(tone)).valid());
    const float open = levelOver(audio, 0.2f);
    ASSERT_GT(open, 0.01f);

    audio.setMuted(true);
    EXPECT_TRUE(audio.muted());
    EXPECT_LT(levelOver(audio, 0.2f), 1e-4f);
    // Still playing underneath, which is the difference between muting and stopping: a
    // mute released mid-track resumes where the track is, not where it was silenced.
    EXPECT_TRUE(audio.sourcePlaying(audio.soundAt(0)));

    audio.setMuted(false);
    EXPECT_GT(levelOver(audio, 0.2f), 0.01f);
}

TEST_F(AudioTest, DistanceAttenuatesASpatialSource) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    AudioSourceDesc desc = source(tone);
    desc.spatial = true;
    desc.minDistance = 1.0f;
    desc.occlusion = false;
    ASSERT_TRUE(audio.create(desc).valid());

    audio.setListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    audio.setSourceTransform(audio.soundAt(0), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.0f)));
    const float near = levelOver(audio, 0.3f);

    audio.setSourceTransform(audio.soundAt(0), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -30.0f)));
    const float far = levelOver(audio, 0.3f);

    EXPECT_GT(near, 0.01f);
    EXPECT_LT(far, near * 0.25f);
}

TEST_F(AudioTest, ABedIgnoresDistanceEntirely) {
    // `spatial: false` is what music and a room tone are, and the reason it belongs to
    // the sound rather than the bus: two sources on one bus can disagree about it.
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    ASSERT_TRUE(audio.create(source(tone)).valid());

    audio.setListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    const float here = levelOver(audio, 0.2f);
    audio.setSourceTransform(audio.soundAt(0), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -500.0f)));
    const float away = levelOver(audio, 0.2f);

    EXPECT_GT(here, 0.01f);
    EXPECT_NEAR(here, away, here * 0.05f);
}

TEST_F(AudioTest, ADuckArrivesAndReleases) {
    AudioConfig cfg = config();
    cfg.buses = {{"music", 1.0f, "sfx", 0.25f, 0.05f, 0.2f}, {"sfx", 1.0f, "", 1.0f, 0.05f, 0.2f}};
    AudioEngine audio;
    ASSERT_TRUE(audio.init(cfg));
    ASSERT_EQ(audio.busCount(), 2u);

    AudioSourceDesc music = source(tone);
    music.bus = "music";
    AudioSourceDesc effect = source(tone);
    effect.name = "effect";
    effect.bus = "sfx";
    effect.autoplay = false;
    ASSERT_TRUE(audio.create(music).valid());
    ASSERT_TRUE(audio.create(effect).valid());

    // Nothing on sfx yet, so music sits at its own volume.
    levelOver(audio, 0.3f);
    EXPECT_NEAR(audio.busGain(0), 1.0f, 1e-3f);

    audio.start(audio.soundAt(1));
    // One step is not a duck: the attack is 50 ms, which is three steps.
    audio.update(kStep);
    EXPECT_LT(audio.busGain(0), 1.0f);
    EXPECT_GT(audio.busGain(0), 0.25f);

    levelOver(audio, 0.3f);
    EXPECT_NEAR(audio.busGain(0), 0.25f, 1e-3f);

    audio.stop(audio.soundAt(1));
    levelOver(audio, 0.5f);
    EXPECT_NEAR(audio.busGain(0), 1.0f, 1e-3f);
}

TEST_F(AudioTest, DuckingIsOffUnlessAskedFor) {
    // The default `duckAmount` of 1.0 is no ducking at all, and that is the point: a
    // mixer that quietly attenuated a bus because something else made a noise is help
    // nobody asked for.
    AudioConfig cfg = config();
    cfg.buses = {{"music", 1.0f, "sfx", 1.0f, 0.05f, 0.2f}, {"sfx", 1.0f, "", 1.0f, 0.05f, 0.2f}};
    AudioEngine audio;
    ASSERT_TRUE(audio.init(cfg));

    AudioSourceDesc music = source(tone);
    music.bus = "music";
    AudioSourceDesc effect = source(tone);
    effect.bus = "sfx";
    ASSERT_TRUE(audio.create(music).valid());
    ASSERT_TRUE(audio.create(effect).valid());

    levelOver(audio, 0.4f);
    EXPECT_NEAR(audio.busGain(0), 1.0f, 1e-3f);
}

TEST_F(AudioTest, AnUnknownBusFallsBackToMaster) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    AudioSourceDesc desc = source(tone);
    desc.bus = "nonexistent";
    // Refusing the source would lose the sound over a typo in a name. It plays on master
    // and the log says so -- the same policy the input map applies to a binding for an
    // action nobody declared.
    ASSERT_TRUE(audio.create(desc).valid());
    EXPECT_GT(levelOver(audio, 0.2f), 0.01f);
}

TEST_F(AudioTest, OcclusionSlewsRatherThanSteps) {
    AudioConfig cfg = config();
    cfg.occlusionAttack = 0.2f;
    cfg.occlusionRelease = 0.2f;
    AudioEngine audio;
    ASSERT_TRUE(audio.init(cfg));

    AudioSourceDesc desc = source(tone);
    desc.spatial = true;
    desc.minDistance = 100.0f; ///< no distance term in the way of the occlusion term
    ASSERT_TRUE(audio.create(desc).valid());
    ASSERT_TRUE(audio.occludable(audio.soundAt(0)));

    audio.setListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    audio.setSourceTransform(audio.soundAt(0), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -2.0f)));
    const float clear = levelOver(audio, 0.3f);
    ASSERT_GT(clear, 0.01f);
    EXPECT_FLOAT_EQ(audio.occlusion(audio.soundAt(0)), 0.0f);

    // A step is what a click sounds like. What this asserts is the absence of one: after
    // a single 16 ms update the state has moved off zero and is nowhere near one.
    audio.setOccluded(audio.soundAt(0), true);
    audio.update(kStep);
    EXPECT_GT(audio.occlusion(audio.soundAt(0)), 0.0f);
    EXPECT_LT(audio.occlusion(audio.soundAt(0)), 0.5f);

    const float blocked = levelOver(audio, 0.5f);
    EXPECT_FLOAT_EQ(audio.occlusion(audio.soundAt(0)), 1.0f);
    // Quieter and duller. The level is the half of it a test can read; the filter is the
    // half that makes it sound like a wall rather than a volume knob.
    EXPECT_LT(blocked, clear);

    audio.setOccluded(audio.soundAt(0), false);
    const float recovered = levelOver(audio, 0.5f);
    EXPECT_FLOAT_EQ(audio.occlusion(audio.soundAt(0)), 0.0f);
    EXPECT_NEAR(recovered, clear, clear * 0.1f);
}

TEST_F(AudioTest, ABedIsNotOccludable) {
    // Occlusion on a non-spatial source is meaningless, and the engine says so rather
    // than raycasting for it -- which is what stops the caller from paying for a ray per
    // frame for the music.
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    ASSERT_TRUE(audio.create(source(tone)).valid());
    EXPECT_FALSE(audio.occludable(audio.soundAt(0)));
}

TEST_F(AudioTest, TheThresholdChoosesThePath) {
    AudioConfig cfg = config();
    cfg.streamThresholdSeconds = 5.0f;
    AudioEngine audio;
    ASSERT_TRUE(audio.init(cfg));

    ASSERT_TRUE(audio.create(source(tone)).valid());   ///< 1 s
    ASSERT_TRUE(audio.create(source(longer)).valid()); ///< 12 s

    EXPECT_FALSE(audio.sourceStreamed(audio.soundAt(0)));
    EXPECT_TRUE(audio.sourceStreamed(audio.soundAt(1)));
    EXPECT_EQ(audio.decodedCount(), 1u);
    EXPECT_EQ(audio.streamedCount(), 1u);
    EXPECT_NEAR(static_cast<double>(audio.sourceSeconds(audio.soundAt(0))), 1.0, 0.01);
    EXPECT_NEAR(static_cast<double>(audio.sourceSeconds(audio.soundAt(1))), 12.0, 0.01);
    // One second of stereo f32 at 48 kHz, and nothing counted for the stream.
    EXPECT_EQ(audio.decodedBytes(), audioDecodedBytes(1.0f, kRate, kChannels));
}

TEST_F(AudioTest, BothPathsPlayTheSameSound) {
    // The property that makes the crossover a cost decision rather than a quality one: a
    // listener cannot tell which path an asset took.
    AudioConfig cfg = config();
    AudioEngine decoded;
    ASSERT_TRUE(decoded.init(cfg));
    AudioSourceDesc a = source(longer);
    a.load = AudioLoad::Decode;
    ASSERT_TRUE(decoded.create(a).valid());
    ASSERT_FALSE(decoded.sourceStreamed(decoded.soundAt(0)));

    AudioEngine streamed;
    ASSERT_TRUE(streamed.init(cfg));
    AudioSourceDesc b = source(longer);
    b.load = AudioLoad::Stream;
    ASSERT_TRUE(streamed.create(b).valid());
    ASSERT_TRUE(streamed.sourceStreamed(streamed.soundAt(0)));

    const float one = levelOver(decoded, 0.3f);
    const float two = levelOver(streamed, 0.3f);
    EXPECT_GT(one, 0.01f);
    EXPECT_NEAR(one, two, one * 0.1f);
}

TEST_F(AudioTest, TheDecodeBudgetPushesRatherThanRefuses) {
    AudioConfig cfg = config();
    cfg.streamThresholdSeconds = 3600.0f; ///< everything would decode
    cfg.decodeBudgetBytes = audioDecodedBytes(2.0f, kRate, kChannels);
    AudioEngine audio;
    ASSERT_TRUE(audio.init(cfg));

    ASSERT_TRUE(audio.create(source(tone)).valid());   ///< 1 s, fits
    ASSERT_TRUE(audio.create(source(longer)).valid()); ///< 12 s, does not

    EXPECT_FALSE(audio.sourceStreamed(audio.soundAt(0)));
    EXPECT_TRUE(audio.sourceStreamed(audio.soundAt(1)));
    // Pushed, not refused: the sound still plays, it just plays the other way. And the
    // fact that a stated budget bound is counted rather than swallowed.
    EXPECT_EQ(audio.refusedSources(), 0u);
    EXPECT_EQ(audio.budgetForcedStreams(), 1u);
    EXPECT_LE(audio.decodedBytes(), cfg.decodeBudgetBytes);
    EXPECT_GT(levelOver(audio, 0.3f), 0.01f);
}

TEST_F(AudioTest, OneFileIsChargedToTheBudgetOnce) {
    // miniaudio's resource manager caches a decoded buffer by path, so eight sources on
    // one sound decode it once and share it. This was found by measuring rather than by
    // reading: eight voices on a 58-second asset reported 169.9 MiB against a budget the
    // process had never spent. Counting per source would refuse a scene memory that was
    // never allocated -- the exact inversion of what a stated budget is for.
    AudioConfig cfg = config();
    cfg.streamThresholdSeconds = 3600.0f;
    AudioEngine audio;
    ASSERT_TRUE(audio.init(cfg));

    for (uint32_t i = 0; i < 8; ++i) ASSERT_TRUE(audio.create(source(tone)).valid());

    EXPECT_EQ(audio.decodedCount(), 8u);
    EXPECT_EQ(audio.decodedBytes(), audioDecodedBytes(1.0f, kRate, kChannels));
    EXPECT_EQ(audio.budgetForcedStreams(), 0u);

    // And a second, different file is charged.
    ASSERT_TRUE(audio.create(source(longer)).valid());
    EXPECT_EQ(audio.decodedBytes(),
              audioDecodedBytes(1.0f, kRate, kChannels) + audioDecodedBytes(12.0f, kRate, kChannels));
}

TEST_F(AudioTest, TheVoiceBudgetGrowsRatherThanRefusing) {
    // **A floor since C40.** Nothing is allocated per voice ahead of time -- the voice list
    // is a vector that already grows -- so what the budget bounds is mixing cost, which is a
    // property of the machine rather than of the game. A third sound past a budget of two
    // therefore plays, and the number doubles behind it.
    AudioConfig cfg = config();
    cfg.voiceBudget = 2;
    AudioEngine audio;
    ASSERT_TRUE(audio.init(cfg));

    EXPECT_TRUE(audio.create(source(tone)).valid());
    EXPECT_TRUE(audio.create(source(tone)).valid());
    EXPECT_TRUE(audio.create(source(tone)).valid()) << "the third was refused, so the budget is still a ceiling";

    EXPECT_EQ(audio.sourceCount(), 3u);
    EXPECT_EQ(audio.refusedSources(), 0u);
}

TEST_F(AudioTest, AMissingFileIsRefusedRatherThanFatal) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    AudioSourceDesc desc = source(dir / "nothing_here.wav");
    EXPECT_FALSE(audio.create(desc).valid());
    EXPECT_EQ(audio.refusedSources(), 1u);
    EXPECT_TRUE(audio.empty());
    // And the engine is still usable afterwards, which is the part that matters: one bad
    // path in a scene should cost that one sound.
    EXPECT_TRUE(audio.create(source(tone)).valid());
    EXPECT_GT(levelOver(audio, 0.2f), 0.01f);
}

TEST_F(AudioTest, DisabledIsNotAFailureAndEveryCallIsSafe) {
    AudioConfig cfg = config();
    cfg.enabled = false;
    AudioEngine audio;
    EXPECT_FALSE(audio.init(cfg));
    EXPECT_FALSE(audio.active());
    EXPECT_TRUE(audio.empty());
    // The shape every subsystem in this engine has: off costs nothing and needs no second
    // test at the call site.
    EXPECT_FALSE(audio.create(source(tone)).valid());
    audio.setListener({}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    audio.update(kStep);
    EXPECT_FLOAT_EQ(audio.lastRms(), 0.0f);
}

TEST_F(AudioTest, TheMixIsAFunctionOfTheStepAndNotOfTheWallClock) {
    // What makes the device-less backend worth having: the same steps in, the same
    // samples out. Two engines fed identically must agree.
    AudioEngine a;
    AudioEngine b;
    ASSERT_TRUE(a.init(config()));
    ASSERT_TRUE(b.init(config()));
    ASSERT_TRUE(a.create(source(tone)).valid());
    ASSERT_TRUE(b.create(source(tone)).valid());

    for (uint32_t i = 0; i < 30; ++i) {
        a.update(kStep);
        b.update(kStep);
        ASSERT_FLOAT_EQ(a.lastRms(), b.lastRms()) << "step " << i;
        ASSERT_FLOAT_EQ(a.lastPeak(), b.lastPeak()) << "step " << i;
    }
}

// ============================================================ capturing the mix

TEST_F(AudioTest, CapturingHandsBackTheSameSamplesTheMixProduced) {
    // The tap is not a second mixing path -- it is a copy taken from `onProcess`, which
    // miniaudio fires at the end of every mix. So what the recorder receives has to be the
    // audio that was played, and the level is how that is checked without asserting on
    // individual samples the resampler is entitled to change.
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    ASSERT_TRUE(audio.create(source(tone)).valid());

    EXPECT_FALSE(audio.capturing());
    audio.startCapture(1.0f);
    ASSERT_TRUE(audio.capturing());

    std::vector<float> captured;
    std::vector<float> block(4096 * kChannels);
    float loudestMixed = 0.0f;
    for (uint32_t i = 0; i < 40; ++i) {
        audio.update(kStep);
        loudestMixed = std::max(loudestMixed, audio.lastPeak());
        while (const uint64_t got = audio.readCaptured(block.data(), 4096)) {
            captured.insert(captured.end(), block.begin(), block.begin() + static_cast<long>(got * kChannels));
        }
    }

    ASSERT_FALSE(captured.empty()) << "onProcess never fired, so nothing was tapped at all";
    EXPECT_EQ(audio.capturedDropped(), 0u) << "a second of ring drained every step should never overflow";
    EXPECT_EQ(audio.sampleRate(), kRate);
    EXPECT_EQ(audio.channelCount(), kChannels);

    // Roughly a step's worth per step, at the mix rate. Loose because the engine carries
    // a fractional frame between steps rather than rounding each one.
    const size_t expectedFrames = static_cast<size_t>(kRate * kStep * 40.0f);
    const size_t gotFrames = captured.size() / kChannels;
    EXPECT_GT(gotFrames, expectedFrames * 9 / 10);
    EXPECT_LT(gotFrames, expectedFrames * 11 / 10);

    float loudestCaptured = 0.0f;
    for (const float v : captured) loudestCaptured = std::max(loudestCaptured, std::abs(v));
    EXPECT_GT(loudestCaptured, 0.0f) << "a playing tone was captured as silence";
    EXPECT_FLOAT_EQ(loudestCaptured, loudestMixed) << "the tap is a copy, not a re-mix";
}

TEST_F(AudioTest, NotCapturingCostsNothingAndStoppingIsIdempotent) {
    // `onProcess` fires on every mix whether or not anything is recording, so the inert
    // path is the common one.
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    ASSERT_TRUE(audio.create(source(tone)).valid());

    std::vector<float> block(1024 * kChannels);
    for (uint32_t i = 0; i < 10; ++i) audio.update(kStep);
    EXPECT_EQ(audio.readCaptured(block.data(), 1024), 0u);

    audio.startCapture(0.5f);
    audio.update(kStep);
    EXPECT_GT(audio.readCaptured(block.data(), 1024), 0u);

    audio.stopCapture();
    audio.stopCapture();
    EXPECT_FALSE(audio.capturing());
    audio.update(kStep);
    EXPECT_EQ(audio.readCaptured(block.data(), 1024), 0u);
}

// `create` returns an invalid handle for a file it could not open or a voice the budget
// refused. It is 0xFFFFFFFF, not an unrepresentable value, so a caller that stored it can
// reach every accessor below with it. Each has to answer the way it would for a source
// that exists and is silent.
TEST_F(AudioTest, TheValueCreateReturnsOnFailureIsSafeToAskAbout) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));

    AudioSourceDesc missing = source(tone);
    missing.file = "res:/definitely_not_a_real_sound_file.wav";
    const SoundId bad = audio.create(missing);
    ASSERT_FALSE(bad.valid()) << "the file opened, so this test proves nothing";

    EXPECT_EQ(audio.source(bad).file, std::string{});
    EXPECT_FALSE(audio.sourceStreamed(bad));
    EXPECT_FLOAT_EQ(audio.sourceSeconds(bad), 0.0f);
    EXPECT_EQ(audio.sourcePosition(bad), glm::vec3(0.0f));
    EXPECT_FALSE(audio.sourcePlaying(bad));
    EXPECT_FLOAT_EQ(audio.occlusion(bad), 0.0f);
    EXPECT_FALSE(audio.occludable(bad));

    // The mutators have to be no-ops rather than crashes for the same index.
    audio.start(bad);
    audio.stop(bad);
    audio.setOccluded(bad, true);
    EXPECT_FLOAT_EQ(audio.occlusion(bad), 0.0f);
    audio.update(kStep);
}

// ========================================== lifetimes: create and destroy

TEST_F(AudioTest, DestroyingASourceMakesTheHandleStale) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    const SoundId id = audio.create(source(tone));
    ASSERT_TRUE(id.valid());
    ASSERT_TRUE(audio.valid(id));

    audio.destroy(id);

    EXPECT_TRUE(id.valid()) << "it was issued";
    EXPECT_FALSE(audio.valid(id)) << "but it no longer names a live source";
    EXPECT_EQ(audio.source(id).file, std::string{});
    EXPECT_FALSE(audio.sourcePlaying(id));
    EXPECT_FALSE(audio.occludable(id));

    audio.update(kStep); // the reclaim boundary; must not crash inside the mixer
    SUCCEED();
}

TEST_F(AudioTest, ASlotIsReusedOnlyAfterUpdateAndDoesNotAlias) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    const SoundId first = audio.create(source(tone));
    ASSERT_TRUE(first.valid());

    audio.destroy(first);
    // Before update() the ma_sound still exists, so the slot must not be handed out.
    const SoundId early = audio.create(source(tone));
    ASSERT_TRUE(early.valid());
    EXPECT_NE(early.index, first.index);

    audio.destroy(early);
    audio.update(kStep);

    const SoundId reused = audio.create(source(tone));
    ASSERT_TRUE(reused.valid());
    EXPECT_TRUE(audio.valid(reused));
    EXPECT_FALSE(audio.valid(first));
    EXPECT_FALSE(audio.valid(early));
    if (reused.index == first.index) EXPECT_NE(reused.generation, first.generation);
}

TEST_F(AudioTest, DestroyIsIdempotentAndToleratesAnInvalidHandle) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    const SoundId id = audio.create(source(tone));
    ASSERT_TRUE(id.valid());

    audio.destroy(id);
    audio.destroy(id);
    audio.destroy(SoundId{});
    audio.update(kStep);
    SUCCEED();
}

TEST_F(AudioTest, TheVoiceBudgetNowBoundsLiveSourcesRatherThanLifetimeTotals) {
    // What C1 changes about the budget: before it, nothing could be retired, so "sources
    // created" and "sources playing" were the same number and the budget could not tell
    // them apart. A game that spawns and despawns a footstep is the case that cares.
    AudioConfig cfg = config();
    cfg.voiceBudget = 2;
    AudioEngine audio;
    ASSERT_TRUE(audio.init(cfg));

    for (int i = 0; i < 20; ++i) {
        const SoundId id = audio.create(source(tone));
        ASSERT_TRUE(id.valid()) << "refused on cycle " << i << ", so the budget is still counting totals";
        audio.destroy(id);
        audio.update(kStep);
    }
}

// ============================================================ one-shots
//
// What a contact event unblocks. Everything a source declared by a node does, a one-shot
// does too -- the bus, the spatialiser, the stream-versus-decode decision -- so what these
// pin is only what is different about it: nobody owns its lifetime, and the same file is
// fired over and over.

TEST_F(AudioTest, AOneShotPlaysAndThenRetiresItself) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));

    AudioSourceDesc desc = source(shot);
    // Deliberately wrong, both of them. `playAt` means "once, here", so a desc that says
    // otherwise is overwritten rather than refused -- a caller reusing one desc for both
    // verbs should not have to zero two fields it never set.
    desc.loop = true;
    desc.autoplay = false;

    const SoundId id = audio.playAt(desc, {1.0f, 2.0f, 3.0f});
    ASSERT_TRUE(id.valid());
    EXPECT_TRUE(audio.valid(id));
    EXPECT_TRUE(audio.sourcePlaying(id)) << "autoplay was forced on";
    EXPECT_EQ(audio.sourcePosition(id), glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_FALSE(audio.sourceStreamed(id)) << "a tenth of a second is the decode side of the crossover";
    EXPECT_GT(levelOver(audio, 0.05f), 0.001f) << "it made no sound at all";

    // 0.1 s of audio, so it is over well inside this; one further update reclaims the
    // slot, and the loop covers both.
    for (int i = 0; i < 30; ++i) audio.update(kStep);
    EXPECT_FALSE(audio.valid(id)) << "nothing called destroy, so the engine had to";
}

TEST_F(AudioTest, OneShotSlotsAreRecycledRatherThanAccumulating) {
    // The property that decides whether this is usable from a contact stream at all. A
    // game firing an impact per collision fires thousands over a session, and a voice per
    // firing would hit the budget in seconds and stay there.
    AudioConfig cfg = config();
    cfg.voiceBudget = 4;
    AudioEngine audio;
    ASSERT_TRUE(audio.init(cfg));

    for (int i = 0; i < 40; ++i) {
        const SoundId id = audio.playAt(source(shot), {0.0f, 0.0f, 0.0f});
        ASSERT_TRUE(id.valid()) << "refused on shot " << i << ", so nothing is being reclaimed";
        for (int s = 0; s < 12; ++s) audio.update(kStep);
    }
    EXPECT_EQ(audio.refusedSources(), 0u);
    // Four slots at most, reused forty times. The exact number is miniaudio's business --
    // what matters is that it is bounded by the budget rather than by the shot count.
    EXPECT_LE(audio.sourceCount(), cfg.voiceBudget);
}

TEST_F(AudioTest, SeveralOneShotsOnOneFileOverlapWithoutRefusal) {
    // Two impacts a step apart is the ordinary case, and they have to be two voices on one
    // decoded buffer rather than one voice restarted -- which would cut the first short.
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));

    std::vector<SoundId> live;
    for (int i = 0; i < 6; ++i) {
        const SoundId id = audio.playAt(source(shot), {static_cast<float>(i), 0.0f, 0.0f});
        ASSERT_TRUE(id.valid());
        live.push_back(id);
        audio.update(kStep);
    }
    for (const SoundId id : live) EXPECT_TRUE(audio.valid(id)) << "an earlier shot was cut short by a later one";
    EXPECT_EQ(audio.refusedSources(), 0u);
    EXPECT_EQ(audio.decodedCount(), 6u);
    // One buffer, six voices -- the same rule the decode budget is charged by.
    EXPECT_EQ(audio.decodedBytes(), audioDecodedBytes(0.1f, kRate, kChannels));
}

TEST_F(AudioTest, AOneShotOnAMissingFileIsRefusedRatherThanCrashing) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));

    AudioSourceDesc desc = source(dir / "nothing_here.wav");
    for (int i = 0; i < 50; ++i) EXPECT_FALSE(audio.playAt(desc, {0.0f, 0.0f, 0.0f}).valid());
    // Counted every time -- the source really was refused each time -- and the file is not
    // reopened after the first, which is what stops a bad path costing a syscall per
    // contact and a log line per frame.
    EXPECT_EQ(audio.refusedSources(), 50u);
    EXPECT_TRUE(audio.empty());
}

TEST_F(AudioTest, AOneShotIsSpatialisedLikeAnyOtherSource) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init(config()));
    audio.setListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});

    AudioSourceDesc desc = source(shot);
    desc.spatial = true;
    desc.minDistance = 1.0f;
    desc.occlusion = false;

    ASSERT_TRUE(audio.playAt(desc, {0.0f, 0.0f, -1.0f}).valid());
    const float near = levelOver(audio, 0.09f);
    for (int i = 0; i < 30; ++i) audio.update(kStep);

    ASSERT_TRUE(audio.playAt(desc, {0.0f, 0.0f, -30.0f}).valid());
    const float far = levelOver(audio, 0.09f);

    EXPECT_GT(near, 0.001f);
    EXPECT_LT(far, near * 0.5f) << "distance did not attenuate it, so it is not on the spatial path";
}

TEST_F(AudioTest, PlayAtOnAnEngineThatNeverCameUpIsANoOp) {
    AudioConfig cfg = config();
    cfg.enabled = false;
    AudioEngine audio;
    EXPECT_FALSE(audio.init(cfg));
    EXPECT_FALSE(audio.playAt(source(shot), {0.0f, 0.0f, 0.0f}).valid());
}

// ------------------------------------------------------ more than one pair of ears

TEST_F(AudioTest, TheListenerCountIsWhatWasAskedForAndIsClampedToWhatMiniaudioHolds) {
    AudioConfig cfg = config();
    cfg.listeners = 2;
    AudioEngine two;
    ASSERT_TRUE(two.init(cfg));
    EXPECT_EQ(two.listenerCount(), 2u);

    // Zero is not silence with extra steps, and nine is more than miniaudio's four. Both
    // land on something the engine can spatialise against rather than being refused.
    cfg.listeners = 0;
    AudioEngine none;
    ASSERT_TRUE(none.init(cfg));
    EXPECT_EQ(none.listenerCount(), 1u);

    cfg.listeners = 9;
    AudioEngine many;
    ASSERT_TRUE(many.init(cfg));
    EXPECT_EQ(many.listenerCount(), 4u);
}

TEST_F(AudioTest, AListenerPastTheCountIsIgnoredRatherThanFoldedOntoZero) {
    AudioConfig cfg = config();
    cfg.listeners = 2;
    AudioEngine audio;
    ASSERT_TRUE(audio.init(cfg));

    audio.setListener({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, 0);
    audio.setListener({7.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, 5);

    // Clamping would have put listener 5 on top of listener 0, which is both players' ears
    // in one place and reads as a panning bug rather than as a bad index.
    EXPECT_FLOAT_EQ(audio.listenerPosition(0).x, 1.0f);
    EXPECT_FLOAT_EQ(audio.listenerPosition(5).x, 0.0f);
}

/**
 * **The row's decision, as a measurement.** miniaudio attenuates a spatial source against
 * the *closest* listener, and this row keeps that rather than summing: summing would make a
 * room louder as players are added and would double a sound both of them can hear, so a
 * source's loudness would stop being a property of the scene.
 *
 * The two arms are the same source at the same place, thirty metres from the ears the first
 * arm has. The second arm adds a second pair one metre away, and nothing else.
 */
TEST_F(AudioTest, ASourceIsHeardFromTheNearestPairOfEars) {
    AudioSourceDesc desc = source(tone);
    desc.spatial = true;
    desc.minDistance = 1.0f;
    desc.occlusion = false;

    float lone = 0.0f;
    {
        AudioEngine audio;
        ASSERT_TRUE(audio.init(config()));
        ASSERT_EQ(audio.listenerCount(), 1u);
        audio.setListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
        ASSERT_TRUE(audio.create(desc).valid());
        audio.setSourceTransform(audio.soundAt(0), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -30.0f)));
        lone = levelOver(audio, 0.25f);
    }

    float pair = 0.0f;
    {
        AudioConfig cfg = config();
        cfg.listeners = 2;
        AudioEngine audio;
        ASSERT_TRUE(audio.init(cfg));
        ASSERT_EQ(audio.listenerCount(), 2u);
        audio.setListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, 0);
        audio.setListener({0.0f, 0.0f, -29.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, 1);
        ASSERT_TRUE(audio.create(desc).valid());
        audio.setSourceTransform(audio.soundAt(0), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -30.0f)));
        pair = levelOver(audio, 0.25f);
    }

    EXPECT_GT(lone, 0.0f);
    EXPECT_GT(pair, lone * 2.0f) << "the second player's ears did not hear the source next to them";
}
