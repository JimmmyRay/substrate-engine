#include "scene/SceneData.h"

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <string>

using namespace core;
using namespace scene;

/**
 * @file tests/SceneDataTests.cpp
 * @brief The scene sidecar's format: what it round-trips, and what it refuses.
 *
 * A cache is the one kind of code where being *wrong* is worse than being absent, because
 * a wrong answer looks like a fast one. So the assertions here are in two groups and the
 * second is the important one:
 *
 * 1. **Round trip.** Everything written comes back, field for field. The failure this
 *    catches is the classic serializer defect -- a field added to the writer and not the
 *    reader, or the reverse -- which produces a file that loads and is subtly wrong.
 * 2. **Refusal.** Every way the cache can fail to apply must produce "no cache" rather
 *    than a bad load: a stale source, a truncated file, a foreign file, a different
 *    layout. These are the cases a corrupted or half-written cache actually takes.
 *
 * No device and no glTF. The whole point of C15's split is that this half needs neither,
 * which is why this file is in the hosted suite and runs under every sanitizer.
 */
namespace {

/// A temporary source file, its sidecar, and the promise that both are gone afterwards.
/// The source has to exist: the cache is stamped with its size and mtime, and a stamp
/// against a file that is not there is what `readSceneCache` refuses first.
class Fixture {
  public:
    Fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("substrate-scene-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
               std::to_string(counter++));
        std::filesystem::create_directories(dir);
        source = dir / "scene.gltf";
        write(source, "{\"asset\":{\"version\":\"2.0\"}}");
    }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    static void write(const std::filesystem::path& p, const std::string& text) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
    }

    std::filesystem::path dir;
    std::filesystem::path source;

  private:
    static inline int counter = 0;
};

/// A scene with something in every container, so a field the writer forgets shows up as a
/// mismatch rather than as two empty vectors agreeing.
SceneData populated() {
    SceneData d;
    d.vertices.push_back({{1.0f, 2.0f, 3.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.25f, 0.75f}});
    d.vertices.push_back({{4.0f, 5.0f, 6.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, -1.0f}, {0.5f, 0.5f}});
    d.indices = {0, 1, 0};

    GpuMaterial m{};
    m.baseColorFactor = {0.1f, 0.2f, 0.3f, 1.0f};
    m.emissiveFactor = {2.0f, 0.0f, 0.0f, 0.0f};
    m.baseColorTexture = 1;
    d.materials.push_back(m);
    d.materialEmissive = {1};

    d.images.push_back({"albedo.png", true});
    d.images.push_back({"normal.png", false});

    Primitive p{};
    p.firstIndex = 0;
    p.indexCount = 3;
    p.materialIndex = 0;
    p.vertexCount = 2;
    p.blended = true;
    // Not the UINT32_MAX default, so the round trip asserts a value rather than a sentinel
    // that a zeroed struct would also produce.
    p.clothOffset = 1;
    p.localMin = {-1.0f, -1.0f, -1.0f};
    p.localMax = {1.0f, 1.0f, 1.0f};
    d.primitives.push_back(p);

    Placement pl{};
    pl.primitive = 0;
    pl.transform = glm::mat4(2.0f);
    pl.node = 7;
    d.placements.push_back(pl);

    gfx::GpuLight light{};
    light.position = {1.0f, 2.0f, 3.0f, 10.0f};
    light.color = {1.0f, 0.5f, 0.25f, 3.0f};
    d.lights.push_back(light);

    ParticleEmitter e;
    e.name = "sparks";
    e.node = 3;
    e.rate = 42.0f;
    e.emissive = true;
    d.emitters.push_back(e);

    ColliderDesc c;
    c.name = "floor";
    c.node = 4;
    c.shape = ColliderShape::Mesh;
    c.motion = ColliderMotion::Static;
    c.points = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    c.indices = {0, 1, 2};
    d.colliders.push_back(c);

    AudioSourceDesc a;
    a.name = "hum";
    a.file = "audio/hum.wav";
    a.bus = "ambience";
    a.node = 5;
    a.volume = 0.5f;
    a.loop = false;
    a.attenuation = AudioAttenuation::Linear;
    d.audioSources.push_back(a);

    d.rig.bind.nodes.push_back({-1, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 0, 2});
    // Two entries against one node, and the empty one is the point as much as the named one:
    // glTF does not require a node to have a name, so the run has to carry a length per string
    // rather than stop at the first empty. See the round-trip assertion below.
    d.rig.nodeNames = {"Hips", ""};
    d.rig.bind.weights = {0.25f, 0.75f};
    Skin skin;
    skin.joints = {0, 1};
    skin.inverseBind = {glm::mat4(1.0f), glm::mat4(3.0f)};
    d.rig.skins.push_back(skin);

    AnimationClip clip;
    clip.name = "walk";
    clip.duration = 1.5f;
    clip.events.push_back({0.25f, "footstep_left"});
    clip.events.push_back({0.75f, "footstep_right"});
    AnimationSampler sampler;
    sampler.times = {0.0f, 1.5f};
    sampler.values = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}};
    sampler.interpolation = AnimationInterpolation::Step;
    sampler.stride = 1;
    clip.samplers.push_back(sampler);
    clip.channels.push_back({0, AnimationPath::Translation, 0});
    d.rig.clips.push_back(clip);

    d.skinVertices.push_back({{0, 1, 2, 3}, {0.5f, 0.5f, 0.0f, 0.0f}});
    d.morphDeltas.push_back({{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}});
    // Two, and not both the same, so a writer that emitted one and a reader that took the
    // other would show up as a mismatch rather than as two equal floats agreeing.
    d.clothVertices.push_back({0.0f});
    d.clothVertices.push_back({0.25f});
    d.indexCopy = {0, 1, 2};

    d.boundsMin = {-5.0f, -1.0f, -5.0f};
    d.boundsMax = {5.0f, 3.0f, 5.0f};
    d.stats.nodes = 9;
    d.stats.draws = 1;
    d.stats.vertexCount = 2;
    return d;
}

} // namespace

TEST(SceneCache, EveryContainerSurvivesTheRoundTrip) {
    Fixture fx;
    const SceneData in = populated();
    ASSERT_TRUE(writeSceneCache(fx.source, in));

    SceneData out;
    ASSERT_TRUE(readSceneCache(fx.source, out));

    EXPECT_EQ(out.vertices.size(), in.vertices.size());
    EXPECT_EQ(out.vertices[1].position, in.vertices[1].position);
    EXPECT_EQ(out.vertices[1].tangent, in.vertices[1].tangent);
    EXPECT_EQ(out.indices, in.indices);
    EXPECT_EQ(out.materialEmissive, in.materialEmissive);
    ASSERT_EQ(out.materials.size(), 1u);
    EXPECT_EQ(out.materials[0].baseColorTexture, 1);
    EXPECT_EQ(out.materials[0].emissiveFactor, in.materials[0].emissiveFactor);

    ASSERT_EQ(out.images.size(), 2u);
    EXPECT_EQ(out.images[0].uri, "albedo.png");
    EXPECT_TRUE(out.images[0].srgb);
    EXPECT_FALSE(out.images[1].srgb);

    ASSERT_EQ(out.primitives.size(), 1u);
    EXPECT_TRUE(out.primitives[0].blended);
    EXPECT_EQ(out.primitives[0].clothOffset, 1u);
    EXPECT_EQ(out.primitives[0].localMax, in.primitives[0].localMax);
    ASSERT_EQ(out.placements.size(), 1u);
    EXPECT_EQ(out.placements[0].transform, in.placements[0].transform);
    EXPECT_EQ(out.placements[0].node, 7u);

    ASSERT_EQ(out.lights.size(), 1u);
    EXPECT_EQ(out.lights[0].color, in.lights[0].color);

    ASSERT_EQ(out.emitters.size(), 1u);
    EXPECT_EQ(out.emitters[0].name, "sparks");
    EXPECT_FLOAT_EQ(out.emitters[0].rate, 42.0f);
    EXPECT_TRUE(out.emitters[0].emissive);

    ASSERT_EQ(out.colliders.size(), 1u);
    EXPECT_EQ(out.colliders[0].name, "floor");
    EXPECT_EQ(out.colliders[0].shape, ColliderShape::Mesh);
    EXPECT_EQ(out.colliders[0].points.size(), 3u);
    EXPECT_EQ(out.colliders[0].indices, in.colliders[0].indices);

    ASSERT_EQ(out.audioSources.size(), 1u);
    EXPECT_EQ(out.audioSources[0].file, "audio/hum.wav");
    EXPECT_EQ(out.audioSources[0].bus, "ambience");
    EXPECT_FALSE(out.audioSources[0].loop);
    EXPECT_EQ(out.audioSources[0].attenuation, AudioAttenuation::Linear);
    EXPECT_FLOAT_EQ(out.audioSources[0].volume, 0.5f);

    ASSERT_EQ(out.rig.clips.size(), 1u);
    const AnimationClip& clip = out.rig.clips[0];
    EXPECT_EQ(clip.name, "walk");
    EXPECT_FLOAT_EQ(clip.duration, 1.5f);
    ASSERT_EQ(clip.events.size(), 2u);
    EXPECT_EQ(clip.events[1].name, "footstep_right");
    ASSERT_EQ(clip.samplers.size(), 1u);
    EXPECT_EQ(clip.samplers[0].interpolation, AnimationInterpolation::Step);
    EXPECT_EQ(clip.samplers[0].values, in.rig.clips[0].samplers[0].values);
    ASSERT_EQ(clip.channels.size(), 1u);
    EXPECT_EQ(clip.channels[0].path, AnimationPath::Translation);

    ASSERT_EQ(out.rig.skins.size(), 1u);
    EXPECT_EQ(out.rig.skins[0].joints, in.rig.skins[0].joints);
    EXPECT_EQ(out.rig.skins[0].inverseBind[1], glm::mat4(3.0f));
    ASSERT_EQ(out.rig.bind.nodes.size(), 1u);
    EXPECT_EQ(out.rig.bind.nodes[0].weightCount, 2u);
    EXPECT_EQ(out.rig.bind.weights, in.rig.bind.weights);
    // **The names, and this is not a formality.** They are the only way a game can reach
    // `SceneAnimator::setRootNode`, and a baked scene is the *fast* path -- so a writer that
    // dropped them would leave root motion working from a glTF and silently unheld from every
    // cache, which is a character that walks correctly until somebody runs `scripts/bake.sh`.
    // That is exactly what happened before this was written.
    EXPECT_EQ(out.rig.nodeNames, in.rig.nodeNames);

    EXPECT_EQ(out.skinVertices.size(), 1u);
    EXPECT_EQ(out.morphDeltas.size(), 1u);
    ASSERT_EQ(out.clothVertices.size(), 2u);
    EXPECT_EQ(out.clothVertices[0].invMass, 0.0f);
    EXPECT_EQ(out.clothVertices[1].invMass, 0.25f);
    EXPECT_EQ(out.indexCopy, in.indexCopy);
    EXPECT_EQ(out.boundsMin, in.boundsMin);
    EXPECT_EQ(out.boundsMax, in.boundsMax);
    EXPECT_EQ(out.stats.nodes, 9u);
    EXPECT_EQ(out.stats.vertexCount, 2u);
}

TEST(SceneCache, TheWriterAndTheReaderAgreeOnTheLength) {
    // The one assertion that catches a field written and not read: the reader refuses a
    // file it did not consume to the last byte, so a writer that emits more than the
    // reader takes fails here rather than silently dropping the tail.
    Fixture fx;
    const SceneData in = populated();
    ASSERT_TRUE(writeSceneCache(fx.source, in));

    std::error_code ec;
    EXPECT_EQ(std::filesystem::file_size(sceneCachePath(fx.source), ec), sceneCacheSize(in));
}

TEST(SceneCache, AnEmptySceneRoundTripsToAnEmptyScene) {
    // Every count is zero and every string absent, which is the shape most likely to walk
    // a reader off the end of a short buffer.
    Fixture fx;
    const SceneData in;
    ASSERT_TRUE(writeSceneCache(fx.source, in));

    SceneData out = populated();
    ASSERT_TRUE(readSceneCache(fx.source, out));
    EXPECT_TRUE(out.vertices.empty());
    EXPECT_TRUE(out.primitives.empty());
    EXPECT_TRUE(out.colliders.empty());
    EXPECT_TRUE(out.rig.clips.empty());
}

TEST(SceneCache, AMissingCacheIsNotAnError) {
    Fixture fx;
    SceneData out;
    EXPECT_FALSE(readSceneCache(fx.source, out));
}

TEST(SceneCache, EditingTheSourceInvalidatesIt) {
    Fixture fx;
    ASSERT_TRUE(writeSceneCache(fx.source, populated()));
    SceneData out;
    ASSERT_TRUE(readSceneCache(fx.source, out));

    // A different length is a different file, whatever the clock says. Size is checked as
    // well as mtime precisely because a filesystem's timestamp resolution is coarse enough
    // that two writes in one tick can share one.
    Fixture::write(fx.source, "{\"asset\":{\"version\":\"2.0\"},\"scenes\":[{\"nodes\":[]}]}");
    EXPECT_FALSE(readSceneCache(fx.source, out));
}

TEST(SceneCache, AMissingSourceInvalidatesIt) {
    // Checked before the cache is even opened: a stamp cannot be compared against a file
    // that is not there, and a cache for a deleted scene is not a scene.
    Fixture fx;
    ASSERT_TRUE(writeSceneCache(fx.source, populated()));
    std::filesystem::remove(fx.source);

    SceneData out;
    EXPECT_FALSE(readSceneCache(fx.source, out));
}

TEST(SceneCache, ATruncatedCacheIsRefusedRatherThanRead) {
    // The normal result of a build killed mid-write, and the case a bounds-unchecked
    // reader turns into a crash or a garbage scene.
    Fixture fx;
    ASSERT_TRUE(writeSceneCache(fx.source, populated()));

    const std::filesystem::path cache = sceneCachePath(fx.source);
    const auto full = std::filesystem::file_size(cache);
    for (double fraction : {0.05, 0.5, 0.95}) {
        std::filesystem::resize_file(cache, static_cast<uintmax_t>(static_cast<double>(full) * fraction));
        SceneData out;
        EXPECT_FALSE(readSceneCache(fx.source, out)) << "truncated to " << fraction;
    }
}

TEST(SceneCache, AFileThatIsNotOneOfTheseIsRefusedOnItsFirstWord) {
    Fixture fx;
    Fixture::write(sceneCachePath(fx.source), "this is not a scene cache, it is a note about one");
    SceneData out;
    EXPECT_FALSE(readSceneCache(fx.source, out));
}

TEST(SceneCache, AWrongVersionOrLayoutIsRefused) {
    // Both live in the header, and both are what stops a cache written by a different
    // build from loading as though it were this one's.
    Fixture fx;
    ASSERT_TRUE(writeSceneCache(fx.source, populated()));
    const std::filesystem::path cache = sceneCachePath(fx.source);

    std::vector<char> bytes;
    {
        std::ifstream in(cache, std::ios::binary);
        bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    ASSERT_GT(bytes.size(), 16u);

    const auto rewrite = [&](size_t offset) {
        std::vector<char> edited = bytes;
        edited[offset] = static_cast<char>(edited[offset] ^ 0x5A);
        std::ofstream out(cache, std::ios::binary | std::ios::trunc);
        out.write(edited.data(), static_cast<std::streamsize>(edited.size()));
    };

    rewrite(8); // version
    SceneData out;
    EXPECT_FALSE(readSceneCache(fx.source, out));

    rewrite(12); // layout digest
    EXPECT_FALSE(readSceneCache(fx.source, out));
}

TEST(SceneCache, AnEmbeddedImageWithNoKtx2IsRefusedAtBakeTime) {
    // The one refusal that is about the *contents* rather than the file. An embedded image
    // is reachable only through the document, so a sidecar for a scene holding one would
    // need the very file it exists to avoid opening. See SceneData.h.
    Fixture fx;
    SceneData d = populated();
    d.images.push_back({"", false}); // embedded, index 2
    EXPECT_FALSE(writeSceneCache(fx.source, d));
    EXPECT_FALSE(std::filesystem::exists(sceneCachePath(fx.source)));

    // With the sidecar beside it, the same scene bakes: the cached load reaches that image
    // through the `.ktx2` and never needs the document.
    Fixture::write(fx.dir / "scene.image2.ktx2", "not really a ktx2, but present");
    EXPECT_TRUE(writeSceneCache(fx.source, d));

    SceneData out;
    ASSERT_TRUE(readSceneCache(fx.source, out));
    ASSERT_EQ(out.images.size(), 3u);
    EXPECT_TRUE(out.images[2].uri.empty());
}

namespace {

/// Leave `v`'s fields exactly as they were and every byte between them 0xAB.
///
/// Destroy, fill the raw storage, construct in place, assign back: the default constructor
/// writes the members and touches nothing else, and the assignment that follows is
/// memberwise, so the padding keeps the poison. That is not a contrivance -- it is what an
/// object built in memory somebody else used already looks like, and it is why baking
/// `particles.gltf` alone and baking it second in one run of `substrate-bake` produced
/// files that differed in 16 bytes before D9.
template <typename T> void poisonPadding(T& v) {
    const T saved = v;
    v.~T();
    std::memset(static_cast<void*>(&v), 0xAB, sizeof(T));
    // `T` and not `T()`. The parenthesised form is *value*-initialisation, which
    // zero-initialises the whole object -- padding included -- and would quietly undo the
    // poison this function exists to plant.
    new (static_cast<void*>(&v)) T;
    v = saved;
}

} // namespace

TEST(SceneCache, TwoWritesOfOneSceneAgreeToTheByte) {
    // D9's whole correctness claim is `cmp`, and a build artifact that cannot be compared
    // is one nobody can check. Three of the payload writers emit a byte *range* of their
    // struct rather than field by field, so anything the compiler left between a `bool` and
    // the `float` after it goes into the file -- and in an object nobody zero-filled, that
    // is whatever the previous tenant of the memory wrote.
    Fixture clean;
    Fixture poisoned;

    const SceneData a = populated();
    SceneData b = populated();
    ASSERT_FALSE(b.emitters.empty());
    ASSERT_FALSE(b.colliders.empty());
    ASSERT_FALSE(b.audioSources.empty());
    poisonPadding(b.emitters[0]);
    poisonPadding(b.colliders[0]);
    poisonPadding(b.audioSources[0]);

    ASSERT_TRUE(writeSceneCache(clean.source, a));
    ASSERT_TRUE(writeSceneCache(poisoned.source, b));

    const auto bytesOf = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    std::string first = bytesOf(sceneCachePath(clean.source));
    std::string second = bytesOf(sceneCachePath(poisoned.source));
    ASSERT_EQ(first.size(), second.size());

    // Everything but the header's source stamp, which is per-file by design: the two
    // fixtures are two different documents and are supposed to disagree about that.
    ASSERT_GT(first.size(), 32u);
    first.replace(16, 16, 16, '\0');
    second.replace(16, 16, 16, '\0');
    EXPECT_EQ(first, second);
}

TEST(SceneCache, TheCachePathSitsBesideTheSourceAndKeepsItsExtension) {
    // `.gltf` and `.glb` of the same name are two scenes; appending rather than replacing
    // is what stops them sharing one cache. The same rule `ktx2.py` follows.
    EXPECT_EQ(sceneCachePath("/a/b/main.gltf").string(), "/a/b/main.gltf.scene");
    EXPECT_EQ(sceneCachePath("/a/b/main.glb").string(), "/a/b/main.glb.scene");
}

// ============================================================================ scale pass
//
// `--scene-scale` exists to buy room to move, and the way to fail at that is to scale
// everything: a character grown with the cathedral has exactly the nave it started with.
// So these pin the split rather than the arithmetic -- what keeps its size, and what only
// gets carried outward.

namespace {

Placement staticPlacement(const glm::vec3& at) {
    Placement p;
    p.transform = glm::translate(glm::mat4(1.0f), at);
    return p; // skin and colliderNode keep their UINT32_MAX defaults
}

} // namespace

TEST(SceneScale, GeometryThatNeverMovesIsScaledWhole) {
    SceneData d;
    d.placements.push_back(staticPlacement({3.0f, 1.0f, -2.0f}));
    d.boundsMin = {-10.0f, -1.0f, -5.0f};
    d.boundsMax = {10.0f, 8.0f, 5.0f};

    scaleSceneData(d, 2.0f);

    EXPECT_EQ(glm::vec3(d.placements[0].transform[3]), glm::vec3(6.0f, 2.0f, -4.0f));
    EXPECT_FLOAT_EQ(d.placements[0].transform[0][0], 2.0f); // and its size, not only its place
    EXPECT_EQ(d.boundsMin, glm::vec3(-20.0f, -2.0f, -10.0f));
    EXPECT_EQ(d.boundsMax, glm::vec3(20.0f, 16.0f, 10.0f));
}

TEST(SceneScale, ARiggedMeshIsCarriedOutwardWithoutGrowing) {
    // The whole point of the flag. A skinned placement is posed by a rig whose proportions
    // are its own, so it moves to stay with the building and stays 1.8 m tall.
    SceneData d;
    Placement rigged = staticPlacement({4.0f, 0.0f, 0.0f});
    rigged.skin = 0;
    d.placements.push_back(rigged);

    scaleSceneData(d, 4.0f);

    EXPECT_EQ(glm::vec3(d.placements[0].transform[3]), glm::vec3(16.0f, 0.0f, 0.0f));
    EXPECT_FLOAT_EQ(d.placements[0].transform[0][0], 1.0f);
}

TEST(SceneScale, AColliderASolverDrivesKeepsItsShapeAndTakesEveryPlacementUnderItWithIt) {
    SceneData d;
    ColliderDesc character;
    character.motion = ColliderMotion::Character;
    character.node = 7;
    character.radius = 0.3f;
    character.halfHeight = 0.6f;
    character.transform = glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.9f});
    d.colliders.push_back(character);

    ColliderDesc ground;
    ground.motion = ColliderMotion::Static;
    ground.node = 3;
    ground.transform = glm::translate(glm::mat4(1.0f), {0.0f, -0.5f, 0.0f});
    d.colliders.push_back(ground);

    // An unskinned mesh hanging off the character's node -- a helmet, a weapon. It is
    // dynamic by inheritance, which is what `colliderNode` is for.
    Placement worn = staticPlacement({0.0f, 1.5f, 0.9f});
    worn.colliderNode = 7;
    d.placements.push_back(worn);

    scaleSceneData(d, 2.0f);

    // The character: carried, not grown. The shape numbers are untouched *and* the
    // transform carries no scale, because `createBody` would apply one from it and the
    // capsule would end up 3.6 m tall.
    EXPECT_EQ(glm::vec3(d.colliders[0].transform[3]), glm::vec3(0.0f, 0.0f, 1.8f));
    EXPECT_FLOAT_EQ(d.colliders[0].transform[0][0], 1.0f);
    EXPECT_FLOAT_EQ(d.colliders[0].radius, 0.3f);
    EXPECT_FLOAT_EQ(d.colliders[0].halfHeight, 0.6f);

    // The ground: scaled whole, and its shape comes along through the transform.
    EXPECT_EQ(glm::vec3(d.colliders[1].transform[3]), glm::vec3(0.0f, -1.0f, 0.0f));
    EXPECT_FLOAT_EQ(d.colliders[1].transform[0][0], 2.0f);

    // Carried by the *anchor's* delta, not by its own scaled position. This assertion read
    // `(0, 3.0, 1.8)` when the pass shipped, which scaled the half-metre gap between the
    // helmet and the head along with everything else -- and a gap that grows with the world
    // is what made the character swing around its body when it turned. The collider moved
    // by (0, 0, 0.9), so everything bolted to it moves by (0, 0, 0.9).
    EXPECT_EQ(glm::vec3(d.placements[0].transform[3]), glm::vec3(0.0f, 1.5f, 1.8f));
    EXPECT_FLOAT_EQ(d.placements[0].transform[0][0], 1.0f);
}

TEST(SceneScale, APunctualLightTakesItsRangeLinearlyAndItsIntensityAsTheSquare) {
    // Inverse square: at 2x every surface is twice as far from every lamp, so a quarter of
    // the light reaches it. Without the square the scaled scene is a cave.
    SceneData d;
    gfx::GpuLight point{};
    point.position = {2.0f, 3.0f, 0.0f, 10.0f};
    point.color = {1.0f, 1.0f, 1.0f, 8.0f};
    point.params = {0.0f, 0.0f, static_cast<float>(gfx::LightType::Point), 0.0f};
    d.lights.push_back(point);

    gfx::GpuLight sun{};
    sun.position = {0.0f, 0.0f, 0.0f, 0.0f};
    sun.color = {1.0f, 1.0f, 1.0f, 3.0f};
    sun.params = {0.0f, 0.0f, static_cast<float>(gfx::LightType::Directional), 0.0f};
    d.lights.push_back(sun);

    scaleSceneData(d, 2.0f);

    EXPECT_EQ(glm::vec3(d.lights[0].position), glm::vec3(4.0f, 6.0f, 0.0f));
    EXPECT_FLOAT_EQ(d.lights[0].position.w, 20.0f);
    EXPECT_FLOAT_EQ(d.lights[0].color.w, 32.0f);

    // The sun has no distance term, so none of the three means anything on it.
    EXPECT_FLOAT_EQ(d.lights[1].color.w, 3.0f);
}

TEST(SceneScale, AudioKeepsItsFalloffCurveOverTheNewDistances) {
    SceneData d;
    AudioSourceDesc hum;
    hum.transform = glm::translate(glm::mat4(1.0f), {5.0f, 0.0f, 0.0f});
    hum.minDistance = 2.0f;
    hum.maxDistance = 40.0f;
    d.audioSources.push_back(hum);

    scaleSceneData(d, 2.0f);

    EXPECT_EQ(glm::vec3(d.audioSources[0].transform[3]), glm::vec3(10.0f, 0.0f, 0.0f));
    EXPECT_FLOAT_EQ(d.audioSources[0].minDistance, 4.0f);
    EXPECT_FLOAT_EQ(d.audioSources[0].maxDistance, 80.0f);
}

TEST(SceneScale, OneAndTheNonsenseFactorsAreRefusedRatherThanApplied) {
    // 1 is what every caller passes by default, so it has to cost nothing. The rest are
    // refused because there is no scene a caller meant by them, and one silently collapsed
    // to the origin is harder to recognise than one that did not move.
    for (const float factor : {1.0f, 0.0f, -2.0f, std::numeric_limits<float>::quiet_NaN()}) {
        SceneData d;
        d.placements.push_back(staticPlacement({3.0f, 1.0f, -2.0f}));
        d.boundsMax = {10.0f, 8.0f, 5.0f};

        scaleSceneData(d, factor);

        EXPECT_EQ(glm::vec3(d.placements[0].transform[3]), glm::vec3(3.0f, 1.0f, -2.0f)) << "factor " << factor;
        EXPECT_EQ(d.boundsMax, glm::vec3(10.0f, 8.0f, 5.0f)) << "factor " << factor;
    }
}

// ============================================================================ place pass
//
// The other half of an import: `scaleSceneData` says how big, this says where. The split
// the scale pass draws is deliberately absent here -- moving a character with the room is
// exactly what a caller placing a building thirty metres east means.

TEST(ScenePlace, EverythingASceneHasGoesThroughTheSameMatrix) {
    SceneData d;
    d.placements.push_back(staticPlacement({1.0f, 0.0f, 0.0f}));

    ColliderDesc c;
    c.transform = glm::translate(glm::mat4(1.0f), {2.0f, 0.0f, 0.0f});
    d.colliders.push_back(c);

    ParticleEmitter e;
    e.transform = glm::translate(glm::mat4(1.0f), {3.0f, 0.0f, 0.0f});
    d.emitters.push_back(e);

    AudioSourceDesc a;
    a.transform = glm::translate(glm::mat4(1.0f), {4.0f, 0.0f, 0.0f});
    d.audioSources.push_back(a);

    placeSceneData(d, glm::translate(glm::mat4(1.0f), {0.0f, 10.0f, 0.0f}));

    EXPECT_EQ(glm::vec3(d.placements[0].transform[3]), glm::vec3(1.0f, 10.0f, 0.0f));
    EXPECT_EQ(glm::vec3(d.colliders[0].transform[3]), glm::vec3(2.0f, 10.0f, 0.0f));
    EXPECT_EQ(glm::vec3(d.emitters[0].transform[3]), glm::vec3(3.0f, 10.0f, 0.0f));
    EXPECT_EQ(glm::vec3(d.audioSources[0].transform[3]), glm::vec3(4.0f, 10.0f, 0.0f));
}

TEST(ScenePlace, ARiggedMeshMovesWithTheBuildingUnlikeUnderTheScalePass) {
    // The one place these two passes deliberately disagree, pinned so neither grows the
    // other's rule by accident.
    SceneData d;
    Placement rigged = staticPlacement({0.0f, 0.0f, 0.0f});
    rigged.skin = 0;
    d.placements.push_back(rigged);

    placeSceneData(d, glm::translate(glm::mat4(1.0f), {30.0f, 0.0f, 0.0f}));

    EXPECT_EQ(glm::vec3(d.placements[0].transform[3]), glm::vec3(30.0f, 0.0f, 0.0f));
}

TEST(ScenePlace, ALightsDirectionTakesTheRotationAndNotTheTranslation) {
    // Running a direction through the full matrix drags it to wherever the import was put,
    // which aims every spot in the file at one point in the world.
    SceneData d;
    gfx::GpuLight spot{};
    spot.position = {0.0f, 0.0f, 0.0f, 10.0f};
    spot.direction = {0.0f, 0.0f, -1.0f, 0.0f};
    spot.params = {0.0f, 0.0f, static_cast<float>(gfx::LightType::Spot), 0.0f};
    d.lights.push_back(spot);

    // A quarter turn about Y, then twenty metres east: -Z becomes -X, and the position moves.
    const glm::mat4 turn = glm::translate(glm::mat4(1.0f), {20.0f, 0.0f, 0.0f}) *
                           glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
    placeSceneData(d, turn);

    EXPECT_NEAR(d.lights[0].direction.x, -1.0f, 1e-5f);
    EXPECT_NEAR(d.lights[0].direction.y, 0.0f, 1e-5f);
    EXPECT_NEAR(d.lights[0].direction.z, 0.0f, 1e-5f);
    EXPECT_NEAR(d.lights[0].position.x, 20.0f, 1e-5f);
    EXPECT_FLOAT_EQ(d.lights[0].position.w, 10.0f); // the range is not a place
}

TEST(ScenePlace, ADirectionalLightIsAimedAndNeverMoved) {
    SceneData d;
    gfx::GpuLight sun{};
    sun.position = {0.0f, 0.0f, 0.0f, 0.0f};
    sun.direction = {0.0f, -1.0f, 0.0f, 0.0f};
    sun.params = {0.0f, 0.0f, static_cast<float>(gfx::LightType::Directional), 0.0f};
    d.lights.push_back(sun);

    placeSceneData(d, glm::translate(glm::mat4(1.0f), {50.0f, 50.0f, 50.0f}));

    EXPECT_EQ(glm::vec3(d.lights[0].position), glm::vec3(0.0f));
    EXPECT_NEAR(d.lights[0].direction.y, -1.0f, 1e-5f);
}

TEST(ScenePlace, ARotatedBoxIsRefittedAroundItsEightCorners) {
    // Transforming `min` and `max` alone gives a box that does not contain the scene for
    // any rotation that is not a multiple of a quarter turn.
    SceneData d;
    d.boundsMin = {-2.0f, 0.0f, -1.0f};
    d.boundsMax = {2.0f, 1.0f, 1.0f};

    placeSceneData(d, glm::rotate(glm::mat4(1.0f), glm::quarter_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)));

    // The 4x2 footprint turned 45 degrees spans 3/sqrt(2) either way on both axes.
    const float halfSpan = 3.0f / std::sqrt(2.0f);
    EXPECT_NEAR(d.boundsMin.x, -halfSpan, 1e-4f);
    EXPECT_NEAR(d.boundsMax.x, halfSpan, 1e-4f);
    EXPECT_NEAR(d.boundsMin.z, -halfSpan, 1e-4f);
    EXPECT_NEAR(d.boundsMax.z, halfSpan, 1e-4f);
    // The turn is about Y, so the height is untouched.
    EXPECT_NEAR(d.boundsMin.y, 0.0f, 1e-5f);
    EXPECT_NEAR(d.boundsMax.y, 1.0f, 1e-5f);
}

TEST(ScenePlace, TheIdentityCostsNothingAndChangesNothing) {
    SceneData d;
    d.placements.push_back(staticPlacement({1.0f, 2.0f, 3.0f}));
    d.boundsMax = {5.0f, 5.0f, 5.0f};

    placeSceneData(d, glm::mat4(1.0f));

    EXPECT_EQ(glm::vec3(d.placements[0].transform[3]), glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(d.boundsMax, glm::vec3(5.0f, 5.0f, 5.0f));
}

TEST(SceneScale, AnAssemblyMovesRigidlySoAMeshStaysOnTheBodyThatDrivesIt) {
    // The regression this exists to keep fixed: a rig's mesh and its capsule are two
    // records for one object -- in showcase.gltf the Armature is a *child* of the collider
    // node -- and translating each by its own scaled position multiplied the gap between
    // them by the factor. `initPhysics` turns that gap into the mesh's attachment offset,
    // so the character swung around its body every time it turned instead of rotating on
    // the spot.
    SceneData d;
    ColliderDesc capsule;
    capsule.motion = ColliderMotion::Character;
    capsule.node = 78;
    capsule.transform = glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.9f});
    d.colliders.push_back(capsule);

    // The rig's mesh, at the same world point the collider is, as a child of it.
    Placement mesh = staticPlacement({0.0f, 0.0f, 0.9f});
    mesh.skin = 0;
    mesh.colliderNode = 78;
    d.placements.push_back(mesh);

    scaleSceneData(d, 2.0f);

    // Both moved, and -- the whole point -- they are still in the same place as each other.
    EXPECT_EQ(glm::vec3(d.colliders[0].transform[3]), glm::vec3(0.0f, 0.0f, 1.8f));
    EXPECT_EQ(glm::vec3(d.placements[0].transform[3]), glm::vec3(0.0f, 0.0f, 1.8f));
    EXPECT_EQ(glm::vec3(d.placements[0].transform[3]), glm::vec3(d.colliders[0].transform[3]));
}

TEST(SceneScale, AMeshOffsetFromItsBodyKeepsThatOffsetRatherThanScalingIt) {
    // The general case of the one above: a mesh authored half a metre in front of its body
    // is half a metre in front of it at any scale. Scaling the offset is what puts a turret
    // off the end of the tank it is bolted to.
    SceneData d;
    ColliderDesc body;
    body.motion = ColliderMotion::Dynamic;
    body.node = 4;
    body.transform = glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f});
    d.colliders.push_back(body);

    Placement turret = staticPlacement({10.5f, 0.0f, 0.0f});
    turret.colliderNode = 4;
    d.placements.push_back(turret);

    scaleSceneData(d, 3.0f);

    EXPECT_EQ(glm::vec3(d.colliders[0].transform[3]), glm::vec3(30.0f, 0.0f, 0.0f));
    EXPECT_EQ(glm::vec3(d.placements[0].transform[3]), glm::vec3(30.5f, 0.0f, 0.0f));
}
