#include "GltfExtras.h"
#include "scene/AudioSource.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace core;

using namespace scene;

/**
 * @file tests/AudioSourceTests.cpp
 * @brief The audio schema and the stream-versus-decode crossover (S5.2).
 *
 * This half of S5 needs no device, no file and no mixer -- it is a JSON reader and a
 * threshold -- which is what makes it the cheapest place to pin the properties that would
 * otherwise only be discovered by listening:
 *
 * 1. **Every spelling round-trips.** A schema whose enum names are only exercised by the
 *    one scene that happens to use them is a schema where the fifth value has never been
 *    parsed. This walks all of them.
 * 2. **A typo keeps the default and says so.** The alternative -- silently taking value
 *    zero -- is a source that is quietly the wrong loudness in a file that looks correct.
 * 3. **The crossover is a stated rule, not an emergent one.** An explicit `stream` or
 *    `decode` beats the threshold, and a duration nobody could determine streams.
 * 4. **Nonsense is corrected rather than propagated.** A cone whose outer angle is inside
 *    its inner one, and a max distance inside the min, both reach miniaudio as something
 *    it does not defend against.
 */

namespace {

/// A one-node glTF carrying whatever `substrate_audio` body is handed in. Written as a
/// string rather than built with rapidjson so the test reads like the file an author
/// would type.
std::string sceneWith(const std::string& body) {
    return R"({"asset":{"version":"2.0"},"nodes":[{"name":"speaker","extras":{"substrate_audio":)" + body +
           R"(}}]})";
}

std::vector<AudioSourceDesc> parse(const std::string& json) {
    std::vector<AudioSourceDesc> out;
    EXPECT_TRUE(testing_extras::parseNodes(json.data(), json.size(), out, parseSceneAudioSources));
    return out;
}

} // namespace

TEST(AudioSource, EmptyDocumentIsNotAFailure) {
    // Sponza. A scene with no sound in it is the ordinary case, not an error.
    const std::string json = R"({"asset":{"version":"2.0"},"nodes":[{"name":"floor"}]})";
    std::vector<AudioSourceDesc> out;
    EXPECT_TRUE(testing_extras::parseNodes(json.data(), json.size(), out, parseSceneAudioSources));
    EXPECT_TRUE(out.empty());
}

TEST(AudioSource, GarbageIsAFailure) {
    const char* junk = "not json at all";
    std::vector<AudioSourceDesc> out;
    EXPECT_FALSE(testing_extras::parseNodes(junk, std::strlen(junk), out, parseSceneAudioSources));
    EXPECT_FALSE(testing_extras::parseNodes(nullptr, 0, out, parseSceneAudioSources));
}

TEST(AudioSource, DefaultsSurviveAMinimalDeclaration) {
    const auto sources = parse(sceneWith(R"({"file":"hum.wav"})"));
    ASSERT_EQ(sources.size(), 1u);
    const AudioSourceDesc& a = sources[0];

    EXPECT_EQ(a.file, "hum.wav");
    EXPECT_EQ(a.name, "speaker"); ///< falls back to the node's name
    EXPECT_EQ(a.node, 0u);
    EXPECT_FLOAT_EQ(a.volume, 1.0f);
    EXPECT_FLOAT_EQ(a.pitch, 1.0f);
    EXPECT_TRUE(a.loop);
    EXPECT_TRUE(a.autoplay);
    EXPECT_TRUE(a.spatial);
    EXPECT_TRUE(a.occlusion);
    EXPECT_EQ(a.load, AudioLoad::Auto);
    EXPECT_EQ(a.attenuation, AudioAttenuation::Inverse);
    // The one default that is a statement rather than a value: Doppler is off because
    // nothing in this engine reports a velocity for it to act on.
    EXPECT_FLOAT_EQ(a.dopplerFactor, 0.0f);
    // Identity, because placing a source is the scene's job and not the parser's.
    EXPECT_FLOAT_EQ(a.transform[3][0], 0.0f);
    EXPECT_FLOAT_EQ(a.transform[3][1], 0.0f);
    EXPECT_FLOAT_EQ(a.transform[3][2], 0.0f);
}

TEST(AudioSource, NoFileIsSkipped) {
    // The one key with no working default. A source with no samples is not a source.
    EXPECT_TRUE(parse(sceneWith(R"({"volume":0.5})")).empty());
}

TEST(AudioSource, EverySpellingRoundTrips) {
    for (uint32_t i = 0; i < 3; ++i) {
        const auto load = static_cast<AudioLoad>(i);
        const auto sources =
            parse(sceneWith(R"({"file":"a.wav","load":")" + std::string(audioLoadName(load)) + R"("})"));
        ASSERT_EQ(sources.size(), 1u);
        EXPECT_EQ(sources[0].load, load) << audioLoadName(load);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        const auto model = static_cast<AudioAttenuation>(i);
        const auto sources = parse(
            sceneWith(R"({"file":"a.wav","attenuation":")" + std::string(audioAttenuationName(model)) + R"("})"));
        ASSERT_EQ(sources.size(), 1u);
        EXPECT_EQ(sources[0].attenuation, model) << audioAttenuationName(model);
    }
}

TEST(AudioSource, UnknownSpellingKeepsTheDefault) {
    const auto sources = parse(sceneWith(R"({"file":"a.wav","load":"strem","attenuation":"invrese"})"));
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_EQ(sources[0].load, AudioLoad::Auto);
    EXPECT_EQ(sources[0].attenuation, AudioAttenuation::Inverse);
}

TEST(AudioSource, EveryFieldIsRead) {
    const auto sources = parse(sceneWith(R"({
        "name":"generator","file":"gen.wav","bus":"ambience",
        "volume":0.4,"pitch":0.9,"loop":false,"autoplay":false,
        "spatial":true,"minDistance":2.5,"maxDistance":40.0,"rolloff":1.5,
        "attenuation":"linear","coneInnerAngle":30.0,"coneOuterAngle":90.0,
        "coneOuterGain":0.2,"dopplerFactor":1.0,"load":"decode","occlusion":false
    })"));
    ASSERT_EQ(sources.size(), 1u);
    const AudioSourceDesc& a = sources[0];

    EXPECT_EQ(a.name, "generator");
    EXPECT_EQ(a.bus, "ambience");
    EXPECT_FLOAT_EQ(a.volume, 0.4f);
    EXPECT_FLOAT_EQ(a.pitch, 0.9f);
    EXPECT_FALSE(a.loop);
    EXPECT_FALSE(a.autoplay);
    EXPECT_FLOAT_EQ(a.minDistance, 2.5f);
    EXPECT_FLOAT_EQ(a.maxDistance, 40.0f);
    EXPECT_FLOAT_EQ(a.rolloff, 1.5f);
    EXPECT_EQ(a.attenuation, AudioAttenuation::Linear);
    // Degrees in the file, radians in the struct -- the one conversion this schema does,
    // because nobody writes 0.5235988.
    EXPECT_NEAR(a.coneInnerAngle, 0.5235988f, 1e-5f);
    EXPECT_NEAR(a.coneOuterAngle, 1.5707963f, 1e-5f);
    EXPECT_FLOAT_EQ(a.coneOuterGain, 0.2f);
    EXPECT_FLOAT_EQ(a.dopplerFactor, 1.0f);
    EXPECT_EQ(a.load, AudioLoad::Decode);
    EXPECT_FALSE(a.occlusion);
}

TEST(AudioSource, InvertedConeIsCorrected) {
    const auto sources = parse(sceneWith(R"({"file":"a.wav","coneInnerAngle":90.0,"coneOuterAngle":30.0})"));
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_GE(sources[0].coneOuterAngle, sources[0].coneInnerAngle);
}

TEST(AudioSource, MaxDistanceInsideMinIsIgnored) {
    const auto sources = parse(sceneWith(R"({"file":"a.wav","minDistance":10.0,"maxDistance":2.0})"));
    ASSERT_EQ(sources.size(), 1u);
    // 0 is "unbounded", which is audible everywhere -- as opposed to a cap inside the
    // full-volume radius, which is silent everywhere including at the source itself.
    EXPECT_FLOAT_EQ(sources[0].maxDistance, 0.0f);
}

TEST(AudioSource, TwoNodesAreTwoSources) {
    // The property every schema in this engine shares: the same declaration under two
    // nodes is two of the thing, and only the node knows where each one is.
    const std::string json = R"({"asset":{"version":"2.0"},"nodes":[
        {"name":"left","extras":{"substrate_audio":{"file":"a.wav"}}},
        {"name":"middle"},
        {"name":"right","extras":{"substrate_audio":{"file":"a.wav"}}}]})";
    const auto sources = parse(json);
    ASSERT_EQ(sources.size(), 2u);
    EXPECT_EQ(sources[0].node, 0u);
    EXPECT_EQ(sources[1].node, 2u);
}

TEST(AudioSource, GlbIsUnwrapped) {
    // The same GLB container path the emitter and collider parsers take, exercised here
    // because it now lives in one place (json::gltfJsonSpan) and a break would be silent
    // in two subsystems at once.
    const std::string json = sceneWith(R"({"file":"a.wav"})");
    std::vector<unsigned char> glb(20);
    std::memcpy(glb.data(), "glTF", 4);
    const uint32_t version = 2;
    const auto total = static_cast<uint32_t>(20 + json.size());
    const auto chunkLength = static_cast<uint32_t>(json.size());
    const uint32_t chunkType = 0x4E4F534Au;
    std::memcpy(glb.data() + 4, &version, 4);
    std::memcpy(glb.data() + 8, &total, 4);
    std::memcpy(glb.data() + 12, &chunkLength, 4);
    std::memcpy(glb.data() + 16, &chunkType, 4);
    glb.insert(glb.end(), json.begin(), json.end());

    std::vector<AudioSourceDesc> out;
    ASSERT_TRUE(testing_extras::parseNodes(glb.data(), glb.size(), out, parseSceneAudioSources));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].file, "a.wav");
}

// --------------------------------------------------------------- the crossover (S5.2)

TEST(AudioCrossover, ThresholdDecidesWhenTheAuthorDoesNot) {
    EXPECT_FALSE(audioShouldStream(AudioLoad::Auto, 1.0f, 5.0f));
    EXPECT_FALSE(audioShouldStream(AudioLoad::Auto, 5.0f, 5.0f)); ///< exactly at it decodes
    EXPECT_TRUE(audioShouldStream(AudioLoad::Auto, 5.001f, 5.0f));
    EXPECT_TRUE(audioShouldStream(AudioLoad::Auto, 536.0f, 5.0f)); ///< the crickets bed
}

TEST(AudioCrossover, AnExplicitChoiceBeatsTheThreshold) {
    // An author who has said which they want has more information than a rule about
    // seconds does -- a one-second effect fired two hundred times a frame may still be
    // the one thing worth streaming, and a thirty-second loop may still be worth decoding.
    EXPECT_TRUE(audioShouldStream(AudioLoad::Stream, 0.1f, 5.0f));
    EXPECT_FALSE(audioShouldStream(AudioLoad::Decode, 3600.0f, 5.0f));
}

TEST(AudioCrossover, AnUnknownDurationStreams) {
    // The whole value of the decode path is that its cost is known in advance. A length
    // the container would not state cheaply is a footprint nobody can bound.
    EXPECT_TRUE(audioShouldStream(AudioLoad::Auto, 0.0f, 5.0f));
    EXPECT_TRUE(audioShouldStream(AudioLoad::Auto, -1.0f, 5.0f));
}

TEST(AudioCrossover, DecodedBytesAreTheMixFormatNotTheFile) {
    // 375 KiB per second of stereo f32 at 48 kHz, which is the number the threshold and
    // the budget are both stated against. A 16-bit file costs twice its own size here and
    // a 24-bit one costs less -- which is why sizing from the file would be wrong.
    EXPECT_EQ(audioDecodedBytes(1.0f, 48000, 2), 48000u * 2u * 4u);
    EXPECT_EQ(audioDecodedBytes(0.0f, 48000, 2), 0u);
    // The two ambience assets this stage was written against, at the mix format. The
    // 8.9-minute bed costs 196 MiB decoded -- three times the whole default budget -- and
    // that figure is the argument for the streaming path existing at all.
    EXPECT_NEAR(static_cast<double>(audioDecodedBytes(535.77f, 48000, 2)) / (1024.0 * 1024.0), 196.2, 0.5);
    EXPECT_NEAR(static_cast<double>(audioDecodedBytes(58.0f, 48000, 2)) / (1024.0 * 1024.0), 21.24, 0.5);
}
