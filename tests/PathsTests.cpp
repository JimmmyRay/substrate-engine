#include "core/Paths.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace core;

namespace fs = std::filesystem;

/**
 * @file tests/PathsTests.cpp
 * @brief `executableDir()`, and the operator identity the packaged build rests on.
 *
 * The interesting property is not the lookup -- it is that one expression serves both a
 * development build and a package. `executableDir() / MACRO` has to be exactly the macro
 * when the macro is absolute, and beside the binary when it is relative, because that is
 * what lets `readShaderBinary` and `Resources` relocate without growing a second code
 * path. If `operator/` ever stopped replacing on an absolute right operand, hot reload in
 * the dev tree would start resolving into the build directory's parent and the failure
 * would look like a missing shader.
 *
 * The test binary is a real executable, so the lookup itself can be checked directly:
 * the answer must be the directory the suite is running from.
 */
TEST(PathsTest, ResolvesToTheDirectoryHoldingTheTestBinary) {
    const fs::path& dir = executableDir();

    EXPECT_FALSE(dir.empty());
    EXPECT_TRUE(dir.is_absolute()) << dir.string();
    EXPECT_TRUE(fs::is_directory(dir)) << dir.string();

    // Named with the platform's suffix rather than checked both ways: this asserts the
    // directory holds *this* binary, and accepting either spelling would let it pass on a
    // directory holding the other one.
#ifdef _WIN32
    EXPECT_TRUE(fs::exists(dir / "substrate_tests.exe")) << dir.string();
#else
    EXPECT_TRUE(fs::exists(dir / "substrate_tests")) << dir.string();
#endif
}

TEST(PathsTest, IsCachedSoRepeatedCallsAreTheSameObject) {
    // Same reference, not merely an equal value: callers put this on the left of an
    // operator/ in hot paths, and re-resolving it per shader would mean a readlink each.
    EXPECT_EQ(&executableDir(), &executableDir());
}

TEST(PathsTest, AnAbsoluteRightOperandReplacesTheExecutableDir) {
    // The development build. SUBSTRATE_SHADER_DIR and the two asset roots are absolute,
    // so the prefix must vanish entirely and leave behaviour bit-identical to before
    // executableDir() existed.
    //
    // Built from root_path() rather than written as "/tmp/...", which is only absolute on
    // POSIX. A Windows path needs a root *name* -- a drive letter -- and `C:` / `/tmp/x`
    // keeps the drive, correctly, because a rooted path with no drive is relative to the
    // current one. Writing the literal made this test assert something untrue on Windows
    // about an operator that was behaving exactly as documented.
    const fs::path absolute = fs::current_path().root_path() / "substrate_build" / "shaders";
    ASSERT_TRUE(absolute.is_absolute()) << absolute.string();

    EXPECT_EQ(executableDir() / absolute, absolute);
}

TEST(PathsTest, ARelativeRightOperandLandsBesideTheBinary) {
    // The packaged build, where the macros fall back to their relative defaults.
    EXPECT_EQ(executableDir() / "shaders", executableDir() / fs::path("shaders"));
    EXPECT_EQ((executableDir() / "shaders").parent_path(), executableDir());
    EXPECT_TRUE((executableDir() / "shaders").is_absolute());
}

TEST(PathsTest, SeedingAfterTheAnswerIsCachedChangesNothing) {
    const fs::path before = executableDir();

    // Documented as harmless rather than as an error, and nullptr is part of that: main()
    // passes argv[0] straight through, and argc can be 0.
    seedExecutablePath(nullptr);
    seedExecutablePath("/definitely/not/where/this/binary/lives/x");

    EXPECT_EQ(executableDir(), before);
}
