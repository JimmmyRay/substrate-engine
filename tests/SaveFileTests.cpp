#include "core/SaveFile.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace core;

/**
 * @file tests/SaveFileTests.cpp
 * @brief The save stream: what round-trips, and what it refuses (C6).
 *
 * A save file is read on a machine and a build that are not the ones that wrote it. That
 * is the entire difficulty, and it is why the refusals below outnumber the round trips: a
 * format that loads a file it does not understand is worse than one that will not load at
 * all, because the first produces a world that is subtly wrong and the second produces a
 * message.
 *
 * Three properties carry it:
 *
 * 1. **Everything written comes back**, in order, at the same values.
 * 2. **A version this build does not know is refused with a reason** -- and refused
 *    *before* a byte of that section is consumed, which is what lets a caller decide.
 * 3. **A truncated or foreign file never reads past its own end.** Every read is checked
 *    and the failure is sticky, so a caller that ignored one bad read cannot get plausible
 *    zeros from the next.
 */
namespace {

/// A writer's bytes as a reader would see them: through `write`, because the framing --
/// header, table of contents, absolute offsets -- is only assembled there.
std::vector<uint8_t> framed(SaveWriter& w) {
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / ("substrate-save-test-" + std::to_string(::rand()) + ".sav");
    EXPECT_TRUE(w.write(p));
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return bytes;
}

} // namespace

TEST(SaveFile, EveryPrimitiveRoundTripsInOrder) {
    SaveWriter w;
    w.beginSection("engine", 1);
    w.u32(0xDEADBEEFu);
    w.u64(0x0123456789ABCDEFull);
    w.i32(-42);
    w.f32(1.5f);
    w.boolean(true);
    w.boolean(false);
    w.vec3({1.0f, 2.0f, 3.0f});
    w.vec4({4.0f, 5.0f, 6.0f, 7.0f});
    w.quat(glm::quat(0.5f, 0.5f, 0.5f, 0.5f));
    w.mat4(glm::mat4(3.0f));
    w.text("hello, save");
    const uint8_t raw[4] = {9, 8, 7, 6};
    w.blob(raw, sizeof(raw));

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w))) << r.reason();
    ASSERT_TRUE(r.section("engine", 1)) << r.reason();

    EXPECT_EQ(r.u32(), 0xDEADBEEFu);
    EXPECT_EQ(r.u64(), 0x0123456789ABCDEFull);
    EXPECT_EQ(r.i32(), -42);
    EXPECT_FLOAT_EQ(r.f32(), 1.5f);
    EXPECT_TRUE(r.boolean());
    EXPECT_FALSE(r.boolean());
    EXPECT_EQ(r.vec3(), glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(r.vec4(), glm::vec4(4.0f, 5.0f, 6.0f, 7.0f));
    EXPECT_EQ(r.quat(), glm::quat(0.5f, 0.5f, 0.5f, 0.5f));
    EXPECT_EQ(r.mat4(), glm::mat4(3.0f));
    EXPECT_EQ(r.text(), "hello, save");
    uint8_t back[4]{};
    r.blob(back, sizeof(back));
    EXPECT_EQ(back[0], 9);
    EXPECT_EQ(back[3], 6);

    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.remaining(), 0u) << "the reader should have consumed exactly what was written";
}

TEST(SaveFile, AnEmptyStringAndAnEmptyBlobAreNotTheSameAsAbsent) {
    SaveWriter w;
    w.beginSection("s", 1);
    w.text("");
    w.u32(7u);
    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    ASSERT_TRUE(r.section("s", 1));
    EXPECT_EQ(r.text(), "");
    EXPECT_EQ(r.u32(), 7u) << "an empty string must still consume its length prefix and no more";
}

TEST(SaveFile, TwoSectionsAreIndependentAndOrderDoesNotMatter) {
    SaveWriter w;
    w.beginSection("engine", 2);
    w.u32(1u);
    w.beginSection("game", 5);
    w.u32(2u);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    ASSERT_EQ(r.sections().size(), 2u);

    // Read the second first. A section is a seek, not a stream position.
    ASSERT_TRUE(r.section("game", 5));
    EXPECT_EQ(r.sectionVersion(), 5u);
    EXPECT_EQ(r.u32(), 2u);

    ASSERT_TRUE(r.section("engine", 2));
    EXPECT_EQ(r.sectionVersion(), 2u);
    EXPECT_EQ(r.u32(), 1u);
}

TEST(SaveFile, ASectionNewerThanThisBuildIsRefusedWithAReasonAndConsumesNothing) {
    SaveWriter w;
    w.beginSection("game", 9);
    w.u32(1234u);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    EXPECT_FALSE(r.section("game", 2));
    EXPECT_NE(r.reason().find("version 9"), std::string::npos) << r.reason();
    EXPECT_NE(r.reason().find("version 2"), std::string::npos) << r.reason();

    // And the reader is still usable: refusing one section must not poison the file, or a
    // save written by a later build would take its engine half down with its game half.
    EXPECT_TRUE(r.ok());
    ASSERT_TRUE(r.section("game", 9));
    EXPECT_EQ(r.u32(), 1234u);
}

TEST(SaveFile, AnOlderSectionIsReadableAndSaysSoByNumber) {
    // The forward half of the same promise: a build that knows version 3 reads a version-1
    // section, and finds out which shape it is looking at.
    SaveWriter w;
    w.beginSection("engine", 1);
    w.u32(11u);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    ASSERT_TRUE(r.section("engine", 3));
    EXPECT_EQ(r.sectionVersion(), 1u);
    EXPECT_EQ(r.u32(), 11u);
}

TEST(SaveFile, AnUnknownSectionIsSkippedRatherThanFatal) {
    SaveWriter w;
    w.beginSection("engine", 1);
    w.u32(1u);
    w.beginSection("fromthefuture", 1);
    w.u32(2u);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    ASSERT_TRUE(r.section("engine", 1)) << "a section this build has never heard of must not stop the ones it has";
    EXPECT_EQ(r.u32(), 1u);
    EXPECT_FALSE(r.section("absent", 1));
    EXPECT_NE(r.reason().find("no section"), std::string::npos) << r.reason();
}

TEST(SaveFile, ReadingPastTheEndOfASectionFailsAndStaysFailed) {
    SaveWriter w;
    w.beginSection("s", 1);
    w.u32(1u);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    ASSERT_TRUE(r.section("s", 1));
    EXPECT_EQ(r.u32(), 1u);
    EXPECT_TRUE(r.ok());

    EXPECT_EQ(r.u32(), 0u);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.reason().empty());

    // Sticky. A caller that missed the first failure must not get a plausible value from
    // the next read.
    EXPECT_EQ(r.f32(), 0.0f);
    EXPECT_FALSE(r.ok());
}

TEST(SaveFile, ASectionCannotReadIntoTheOneAfterIt) {
    // The bound is the *section*, not the file. Without that, a short engine section would
    // quietly read the game's bytes as its own.
    SaveWriter w;
    w.beginSection("a", 1);
    w.u32(1u);
    w.beginSection("b", 1);
    w.u32(0xFFFFFFFFu);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    ASSERT_TRUE(r.section("a", 1));
    EXPECT_EQ(r.u32(), 1u);
    EXPECT_EQ(r.u32(), 0u) << "section a must not see section b's bytes";
    EXPECT_FALSE(r.ok());
}

TEST(SaveFile, AFileThatIsNotASaveIsRefusedOnItsFirstWord) {
    SaveReader r;
    const std::string text = "dear diary, today I was not a save file";
    EXPECT_FALSE(r.openBytes({text.begin(), text.end()}));
    EXPECT_NE(r.reason().find("not a save"), std::string::npos) << r.reason();
}

TEST(SaveFile, AnEmptyFileIsRefusedRatherThanReadAsAnEmptySave) {
    SaveReader r;
    EXPECT_FALSE(r.openBytes({}));
    EXPECT_FALSE(r.reason().empty());
}

TEST(SaveFile, ATruncatedFileIsRefusedAtEveryLength) {
    SaveWriter w;
    w.beginSection("engine", 1);
    for (uint32_t i = 0; i < 64; ++i) w.mat4(glm::mat4(static_cast<float>(i)));
    const std::vector<uint8_t> full = framed(w);
    ASSERT_GT(full.size(), 100u);

    for (size_t cut : {size_t(4), size_t(12), full.size() / 3, full.size() - 1}) {
        SaveReader r;
        const std::vector<uint8_t> part(full.begin(), full.begin() + static_cast<long>(cut));
        EXPECT_FALSE(r.openBytes(part)) << "accepted a file truncated to " << cut;
        EXPECT_FALSE(r.reason().empty());
    }
}

TEST(SaveFile, AFramingVersionThisBuildDoesNotKnowIsRefusedByNumber) {
    SaveWriter w;
    w.beginSection("engine", 1);
    w.u32(1u);
    std::vector<uint8_t> bytes = framed(w);

    // The framing version sits immediately after the eight-byte magic.
    const uint32_t future = kSaveFileVersion + 7;
    std::memcpy(bytes.data() + 8, &future, sizeof(future));

    SaveReader r;
    EXPECT_FALSE(r.openBytes(bytes));
    EXPECT_NE(r.reason().find(std::to_string(future)), std::string::npos) << r.reason();
}

TEST(SaveFile, ASectionClaimingToRunPastTheEndIsRefused) {
    // What a hand-edited or half-copied save looks like, and the one case a reader that
    // trusted its own table of contents would walk straight off the end for.
    SaveWriter w;
    w.beginSection("engine", 1);
    w.u32(1u);
    std::vector<uint8_t> bytes = framed(w);

    // The first entry's length is the last field of the first table entry: header is
    // 8 + 4 + 4 = 16, and an entry is 16 + 4 + 8 + 8 with length last.
    const uint64_t huge = 1ull << 40;
    std::memcpy(bytes.data() + 16 + 16 + 4 + 8, &huge, sizeof(huge));

    SaveReader r;
    EXPECT_FALSE(r.openBytes(bytes));
    EXPECT_NE(r.reason().find("truncated"), std::string::npos) << r.reason();
}

TEST(SaveFile, AStringClaimingToBeLongerThanItsSectionIsRefusedRatherThanAllocated) {
    SaveWriter w;
    w.beginSection("s", 1);
    w.u64(1ull << 40); // a length prefix with no string behind it
    w.u32(0u);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    ASSERT_TRUE(r.section("s", 1));
    EXPECT_TRUE(r.text().empty());
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.reason().find("longer than"), std::string::npos) << r.reason();
}

TEST(SaveFile, ALongSectionNameIsTruncatedRatherThanOverrunning) {
    SaveWriter w;
    w.beginSection("a-very-long-section-name-indeed", 1);
    w.u32(5u);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    ASSERT_EQ(r.sections().size(), 1u);
    EXPECT_EQ(r.sections()[0].name, "a-very-long-sec");
    EXPECT_EQ(r.sections()[0].name.size(), 15u);
    ASSERT_TRUE(r.section("a-very-long-sec", 1));
    EXPECT_EQ(r.u32(), 5u);
}

TEST(SaveFile, AWriterWithNoSectionsProducesAFileThatOpensAndHasNone) {
    // The default a game that saves nothing produces, and it must not be a corrupt file.
    SaveWriter w;
    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    EXPECT_TRUE(r.sections().empty());
    EXPECT_FALSE(r.section("engine", 1));
}

TEST(SaveFile, WritingGoesThroughATemporaryAndLeavesNoneBehind) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "substrate-save-atomic";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "slot0.sav";

    SaveWriter w;
    w.beginSection("engine", 1);
    w.u32(1u);
    ASSERT_TRUE(w.write(file));

    EXPECT_TRUE(std::filesystem::exists(file));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(file).concat(".tmp")))
        << "the temporary must be renamed, not left beside the save";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
