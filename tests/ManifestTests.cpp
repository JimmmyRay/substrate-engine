#include "cli/Manifest.h"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

/**
 * @file tests/ManifestTests.cpp
 * @brief The packaging closure, over temporary trees.
 *
 * Every case builds the two asset roots on disk rather than mocking a filesystem, because
 * what is being checked is which files exist and where they sit relative to one another --
 * the one property a fake would have to reimplement in order to test.
 */
namespace {

namespace fs = std::filesystem;
using namespace tool::manifest;

class TreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code ec;
        root = fs::temp_directory_path(ec) /
               ("substrate_manifest_" + std::string(
                    ::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(root, ec);
        game = root / "game";
        engine = root / "engine";
        fs::create_directories(game, ec);
        fs::create_directories(engine, ec);
        resolver = Resolver{game, engine, "game/demo/assets", "engine/assets"};
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path write(const fs::path& path, const std::string& body = "") {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary) << body;
        return path;
    }

    fs::path gltf(const fs::path& path, const std::string& json) { return write(path, json); }

    /// The destinations of everything staged, for the `contains` assertions below.
    std::vector<std::string> destinations(const Closure& closure) const {
        std::vector<std::string> out;
        for (const auto& [src, dest] : closure.staged) out.push_back(dest);
        return out;
    }

    bool staged(const Closure& closure, const std::string& dest) const {
        const std::vector<std::string> all = destinations(closure);
        return std::find(all.begin(), all.end(), dest) != all.end();
    }

    fs::path root;
    fs::path game;
    fs::path engine;
    Resolver resolver;
};

// ------------------------------------------------- resolution, as ResourcesTests pins it

TEST_F(TreeTest, ResolvesOutOfTheEngineTree) {
    write(engine / "only_engine.gltf");
    fs::path path;
    std::string tree;
    ASSERT_TRUE(resolver.resolve("only_engine.gltf", path, tree));
    EXPECT_EQ(tree, "engine");
}

TEST_F(TreeTest, ResolvesOutOfTheGameTree) {
    write(game / "only_game.gltf");
    fs::path path;
    std::string tree;
    ASSERT_TRUE(resolver.resolve("only_game.gltf", path, tree));
    EXPECT_EQ(tree, "game");
}

TEST_F(TreeTest, TheGameTreeWinsWhenBothHoldTheName) {
    write(game / "both.gltf", "game");
    write(engine / "both.gltf", "engine");
    fs::path path;
    std::string tree;
    ASSERT_TRUE(resolver.resolve("both.gltf", path, tree));
    EXPECT_EQ(tree, "game");
}

TEST_F(TreeTest, ANameInNeitherTreeIsNotResolved) {
    fs::path path;
    std::string tree;
    EXPECT_FALSE(resolver.resolve("nowhere.gltf", path, tree));
}

// ------------------------------------------------------------------ what a document names

TEST_F(TreeTest, FollowsBuffersAndImages) {
    write(game / "s.bin");
    write(game / "t.png");
    gltf(game / "s.gltf",
         R"({"buffers":[{"uri":"s.bin"}],"images":[{"uri":"t.png"}]})");

    const Closure closure = build(resolver, {"s.gltf"}, {}, false);
    EXPECT_TRUE(closure.missing.empty());
    EXPECT_TRUE(staged(closure, "game/demo/assets/s.bin"));
    EXPECT_TRUE(staged(closure, "game/demo/assets/t.png"));
}

TEST_F(TreeTest, SkipsDataUris) {
    gltf(game / "s.gltf", R"({"buffers":[{"uri":"data:application/octet-stream;base64,AA=="}]})");

    const Closure closure = build(resolver, {"s.gltf"}, {}, false);
    EXPECT_TRUE(closure.missing.empty());
    EXPECT_EQ(closure.staged.size(), 1u);
}

TEST_F(TreeTest, StagesAKtx2SidecarWhenOneExists) {
    write(game / "t.png");
    write(game / "t.png.ktx2");
    gltf(game / "s.gltf", R"({"images":[{"uri":"t.png"}]})");

    const Closure closure = build(resolver, {"s.gltf"}, {}, false);
    EXPECT_TRUE(staged(closure, "game/demo/assets/t.png.ktx2"));
}

TEST_F(TreeTest, AMissingKtx2SidecarIsNotAnError) {
    write(game / "t.png");
    gltf(game / "s.gltf", R"({"images":[{"uri":"t.png"}]})");

    const Closure closure = build(resolver, {"s.gltf"}, {}, false);
    EXPECT_TRUE(closure.missing.empty());
    EXPECT_FALSE(staged(closure, "game/demo/assets/t.png.ktx2"));
}

TEST_F(TreeTest, FollowsAudioDeclaredInNodeExtras) {
    write(game / "hit.wav");
    gltf(game / "s.gltf",
         R"({"nodes":[{"extras":{"substrate_audio":{"file":"hit.wav"}}}]})");

    const Closure closure = build(resolver, {"s.gltf"}, {}, false);
    EXPECT_TRUE(closure.missing.empty());
    EXPECT_TRUE(staged(closure, "game/demo/assets/hit.wav"));
}

TEST_F(TreeTest, FollowsAReferenceAcrossIntoTheOtherTree) {
    // What the composite scenes actually do: a game scene grafted onto a glTF in the engine
    // tree, reached with ../../../engine/assets/...
    write(engine / "shared" / "tex.png");
    gltf(game / "s.gltf", R"({"images":[{"uri":"../engine/shared/tex.png"}]})");

    const Closure closure = build(resolver, {"s.gltf"}, {}, false);
    EXPECT_TRUE(closure.missing.empty());
    EXPECT_TRUE(staged(closure, "engine/assets/shared/tex.png"));
}

TEST_F(TreeTest, AReferenceOutsideBothTreesIsReported) {
    write(root / "stray.png");
    gltf(game / "s.gltf", R"({"images":[{"uri":"../stray.png"}]})");

    const Closure closure = build(resolver, {"s.gltf"}, {}, false);
    ASSERT_EQ(closure.missing.size(), 1u);
    EXPECT_NE(closure.missing[0].find("outside both asset trees"), std::string::npos);
}

TEST_F(TreeTest, ACycleTerminates) {
    gltf(game / "a.gltf", R"({"buffers":[{"uri":"b.gltf"}]})");
    gltf(game / "b.gltf", R"({"buffers":[{"uri":"a.gltf"}]})");

    const Closure closure = build(resolver, {"a.gltf"}, {}, false);
    EXPECT_TRUE(closure.missing.empty());
    EXPECT_EQ(closure.staged.size(), 2u);
}

TEST_F(TreeTest, ReadsAGlbJsonChunk) {
    std::string doc = R"({"buffers":[{"uri":"s.bin"}]})";
    doc.append((4 - doc.size() % 4) % 4, ' ');

    std::string blob = "glTF";
    const auto append = [&blob](uint32_t value) {
        char bytes[4];
        std::memcpy(bytes, &value, 4);
        blob.append(bytes, 4);
    };
    append(2);
    append(static_cast<uint32_t>(12 + 8 + doc.size()));
    append(static_cast<uint32_t>(doc.size()));
    append(0x4E4F534Au);
    blob += doc;

    write(game / "s.glb", blob);
    write(game / "s.bin");

    const Closure closure = build(resolver, {"s.glb"}, {}, false);
    EXPECT_TRUE(closure.missing.empty());
    EXPECT_TRUE(staged(closure, "game/demo/assets/s.bin"));
}

// ---------------------------------------------------------------------------- the seeds

TEST_F(TreeTest, ARequiredSeedThatResolvesToNothingFails) {
    const Closure closure = build(resolver, {"absent.gltf"}, {}, false);
    ASSERT_EQ(closure.missing.size(), 1u);
    EXPECT_EQ(closure.missing[0], "absent.gltf");
}

TEST_F(TreeTest, AnOptionalSeedThatResolvesToNothingIsFine) {
    const Closure closure = build(resolver, {}, {"absent.gltf"}, false);
    EXPECT_TRUE(closure.missing.empty());
    EXPECT_TRUE(closure.staged.empty());
}

// ---------------------------------------------------------------------- the texture cache

TEST_F(TreeTest, AColdSidecarIsReportedWhenTheCacheIsRequired) {
    write(game / "t.png");
    gltf(game / "s.gltf", R"({"images":[{"uri":"t.png"}]})");

    const Closure closure = build(resolver, {"s.gltf"}, {}, true);
    ASSERT_EQ(closure.cold.size(), 1u);
    EXPECT_NE(closure.cold.begin()->find("t.png.ktx2"), std::string::npos);
}

TEST_F(TreeTest, AColdSidecarIsSilentByDefault) {
    write(game / "t.png");
    gltf(game / "s.gltf", R"({"images":[{"uri":"t.png"}]})");

    const Closure closure = build(resolver, {"s.gltf"}, {}, false);
    EXPECT_TRUE(closure.cold.empty());
}

TEST_F(TreeTest, ABuiltSidecarIsNotCold) {
    write(game / "t.png");
    write(game / "t.png.ktx2");
    gltf(game / "s.gltf", R"({"images":[{"uri":"t.png"}]})");

    const Closure closure = build(resolver, {"s.gltf"}, {}, true);
    EXPECT_TRUE(closure.cold.empty());
}

// ------------------------------------------------------------------- restricted content

TEST_F(TreeTest, RestrictedContentIsPackagedAndListed) {
    write(engine / "Sponza" / "s.gltf", "{}");

    const Closure closure = build(resolver, {"Sponza/s.gltf"}, {}, false);
    EXPECT_TRUE(staged(closure, "engine/assets/Sponza/s.gltf"));
    ASSERT_EQ(closure.restricted.size(), 1u);
    EXPECT_EQ(*closure.restricted.begin(), "engine/assets/Sponza/s.gltf");
}

TEST_F(TreeTest, UnrestrictedContentReportsNothing) {
    write(game / "s.gltf", "{}");

    const Closure closure = build(resolver, {"s.gltf"}, {}, false);
    EXPECT_TRUE(closure.restricted.empty());
}

TEST(RestrictedPart, NamesTheOffendingComponent) {
    EXPECT_EQ(restrictedPart("Sponza/glTF/a.jpg"), "Sponza");
    EXPECT_EQ(restrictedPart("a/Sponza/b.jpg"), "Sponza");
    EXPECT_TRUE(restrictedPart("SponzaLike/a.jpg").empty());
    EXPECT_TRUE(restrictedPart("engine/assets/a.jpg").empty());
}

// -------------------------------------------------------------------- the source scanner

TEST(SourceScan, FindsANameInAStringLiteral) {
    const std::set<std::string> names = resNamesInSource(R"(load("res:/a.gltf");)");
    EXPECT_EQ(names, std::set<std::string>{"a.gltf"});
}

TEST(SourceScan, IgnoresANameInALineComment) {
    EXPECT_TRUE(resNamesInSource("// res:/a.gltf\n").empty());
}

TEST(SourceScan, IgnoresANameInABlockComment) {
    EXPECT_TRUE(resNamesInSource("/* res:/a.gltf */").empty());
}

TEST(SourceScan, IgnoresAPrintfFormatString) {
    EXPECT_TRUE(resNamesInSource(R"(warn("res:/%.*s not found");)").empty());
}

TEST(SourceScan, ALiteralContainingADoubleSlashIsNotCutShort) {
    // Skipping comments without tracking strings would cut this literal in half and lose
    // the name after it.
    const std::set<std::string> names =
        resNamesInSource(R"(url("http://x"); load("res:/a.gltf");)");
    EXPECT_EQ(names, std::set<std::string>{"a.gltf"});
}

TEST(SourceScan, AQuotedExampleInAFileCommentIsNotADependency) {
    // Resources.h's own file comment quotes `Resources("res:/showcase.gltf")` as prose about
    // how the scheme works. Treating it as a dependency had the manifest demanding a scene
    // the package never asks for.
    EXPECT_TRUE(resNamesInSource("/** Resources(\"res:/showcase.gltf\") */").empty());
}

} // namespace
