#include "core/Resources.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace core;

namespace fs = std::filesystem;

/**
 * @file tests/ResourcesTests.cpp
 * @brief `res:/` lookup: two trees, game first, everything else passed through.
 *
 * Every test uses the roots-injecting constructor against directories built in
 * `temp_directory_path()`, the same way `AudioTests` writes its own WAVs: the asset trees
 * are generated and gitignored, so a suite that resolved against the real ones would be a
 * suite that skips itself on a fresh clone.
 *
 * The property most worth pinning is the third one -- when both trees hold a name, the
 * game's wins. That is what makes a game able to ship its own version of an engine asset,
 * and it is invisible until the day two trees disagree.
 */
class ResourcesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        const auto unique = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        root = fs::temp_directory_path() / ("substrate_res_" + std::string(unique));
        gameRoot = root / "game";
        engineRoot = root / "engine";
        fs::remove_all(root);
        fs::create_directories(gameRoot);
        fs::create_directories(engineRoot);
    }

    void TearDown() override { fs::remove_all(root); }

    static void write(const fs::path& p, std::string_view body) {
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << body;
    }

    fs::path root, gameRoot, engineRoot;
};

TEST_F(ResourcesTest, ResolvesOutOfTheEngineTree) {
    write(engineRoot / "only_engine.gltf", "e");

    const Resources r("res:/only_engine.gltf", gameRoot, engineRoot);
    EXPECT_TRUE(r.found());
    EXPECT_EQ(r.path(), engineRoot / "only_engine.gltf");
}

TEST_F(ResourcesTest, ResolvesOutOfTheGameTree) {
    write(gameRoot / "only_game.gltf", "g");

    const Resources r("res:/only_game.gltf", gameRoot, engineRoot);
    EXPECT_TRUE(r.found());
    EXPECT_EQ(r.path(), gameRoot / "only_game.gltf");
}

// The override rule. Both trees carry the name; the game's is the one that runs.
TEST_F(ResourcesTest, GameTreeWinsWhenBothHoldTheName) {
    write(gameRoot / "shared.gltf", "g");
    write(engineRoot / "shared.gltf", "e");

    const Resources r("res:/shared.gltf", gameRoot, engineRoot);
    ASSERT_TRUE(r.found());
    EXPECT_EQ(r.path(), gameRoot / "shared.gltf");

    std::ifstream in(r.path());
    std::string body;
    in >> body;
    EXPECT_EQ(body, "g");
}

TEST_F(ResourcesTest, EmptyGameRootFallsStraightToTheEngine) {
    write(engineRoot / "thing.gltf", "e");

    const Resources r("res:/thing.gltf", fs::path(), engineRoot);
    EXPECT_TRUE(r.found());
    EXPECT_EQ(r.path(), engineRoot / "thing.gltf");
}

// An empty game root must not become a bare relative name that matches something in the
// working directory. The name below exists nowhere, and the answer must be the engine's
// candidate rather than a hit.
TEST_F(ResourcesTest, EmptyGameRootDoesNotMatchTheWorkingDirectory) {
    const Resources r("res:/substrate.json", fs::path(), engineRoot);
    EXPECT_FALSE(r.found());
    EXPECT_EQ(r.path(), engineRoot / "substrate.json");
}

TEST_F(ResourcesTest, LeadingSlashesAreNotSignificant) {
    write(engineRoot / "thing.gltf", "e");

    const Resources one("res:/thing.gltf", gameRoot, engineRoot);
    const Resources two("res://thing.gltf", gameRoot, engineRoot);
    const Resources bare("res:thing.gltf", gameRoot, engineRoot);

    EXPECT_EQ(one.path(), two.path());
    EXPECT_EQ(one.path(), bare.path());
    EXPECT_TRUE(one.found());
}

TEST_F(ResourcesTest, NestedNamesJoin) {
    write(engineRoot / "Sponza" / "glTF" / "Sponza.gltf", "e");

    const Resources r("res:/Sponza/glTF/Sponza.gltf", gameRoot, engineRoot);
    EXPECT_TRUE(r.found());
    EXPECT_EQ(r.path(), engineRoot / "Sponza" / "glTF" / "Sponza.gltf");
}

// A miss is not an error. It reports itself and still yields the path a caller can name
// in its own diagnostic.
TEST_F(ResourcesTest, MissReportsItselfAndStillYieldsAPath) {
    const Resources r("res:/absent.gltf", gameRoot, engineRoot);
    EXPECT_FALSE(r.found());
    EXPECT_EQ(r.path(), engineRoot / "absent.gltf");
    EXPECT_FALSE(r.string().empty());
}

// Everything that worked before the scheme existed keeps working, which is what lets call
// sites move over one at a time.
TEST_F(ResourcesTest, PathWithoutSchemeIsPassedThrough) {
    write(root / "loose.gltf", "l");

    const Resources absolute((root / "loose.gltf").string(), gameRoot, engineRoot);
    EXPECT_TRUE(absolute.found());
    EXPECT_EQ(absolute.path(), root / "loose.gltf");

    // A relative path still names the same file it did: absolute() only prepends the
    // working directory.
    const Resources relative("engine/assets/thing.gltf", gameRoot, engineRoot);
    EXPECT_EQ(relative.path(), fs::absolute("engine/assets/thing.gltf"));
}

TEST_F(ResourcesTest, ResultIsAlwaysAbsolute) {
    write(engineRoot / "thing.gltf", "e");

    EXPECT_TRUE(Resources("res:/thing.gltf", gameRoot, engineRoot).path().is_absolute());
    EXPECT_TRUE(Resources("res:/absent.gltf", gameRoot, engineRoot).path().is_absolute());
    EXPECT_TRUE(Resources("relative/thing.gltf", gameRoot, engineRoot).path().is_absolute());
}

// The one exception, and it is the reason the exception exists: `render.debugFont`
// defaults to "" meaning "use the embedded bitmap font", and Font::init decides that by
// asking whether the path is empty. Resolving "" to the working directory would have it
// try to read a directory as a TTF and warn on every launch.
TEST_F(ResourcesTest, EmptyNameStaysEmpty) {
    const Resources r("", gameRoot, engineRoot);
    EXPECT_TRUE(r.path().empty());
    EXPECT_FALSE(r.found());
}

// The conversion is what keeps the call sites free of `.path()`.
TEST_F(ResourcesTest, ConvertsImplicitlyToAPath) {
    write(engineRoot / "thing.gltf", "e");

    const auto take = [](const fs::path& p) { return p.filename().string(); };
    EXPECT_EQ(take(Resources("res:/thing.gltf", gameRoot, engineRoot)), "thing.gltf");
}

// A scheme-shaped prefix that is not the scheme must not be eaten.
TEST_F(ResourcesTest, OnlyTheResSchemeIsRecognised) {
    const Resources r("resources/thing.gltf", gameRoot, engineRoot);
    EXPECT_EQ(r.path(), fs::absolute("resources/thing.gltf"));
}
