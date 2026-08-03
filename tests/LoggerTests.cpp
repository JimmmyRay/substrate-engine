#include "core/Logger.h"

#include "core/Format.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace core;

namespace fs = std::filesystem;

/**
 * @file tests/LoggerTests.cpp
 * @brief Categorised logging and its async file writer (5.1).
 *
 * Two things here are worth a test rather than a read-through. The filter is a pair of
 * relaxed atomics consulted before every message, so getting it wrong drops output
 * silently -- which is indistinguishable from nothing having happened. And the file
 * path is a queue drained by a background thread, so "the message was logged" and "the
 * message is in the file" are separated by a join that `shutdown()` owes the caller.
 */

namespace {

/// Restores the process-wide filter state, which is shared with every other test in
/// the binary and with `tests/main.cpp`'s decision to quieten the suite.
class LoggerTest : public ::testing::Test {
  protected:
    fs::path dir;
    LogLevel savedLevel = LogLevel::Error;
    uint32_t savedCategories = AllLogCategories;

    void SetUp() override {
        savedLevel = Logger::level();
        savedCategories = Logger::categories();

        dir = fs::temp_directory_path() / "substrate_logger_tests";
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    void TearDown() override {
        Logger::shutdown();
        Logger::setLevel(savedLevel);
        Logger::setCategories(savedCategories);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    static std::string readAll(const fs::path& p) {
        std::ifstream in(p);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
};

/// A variadic caller for `Logger::vformat`, which only takes a `va_list` and so cannot
/// be reached without one.
__attribute__((format(SUBSTRATE_PRINTF_FORMAT, 1, 2))) std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string s = Logger::vformat(fmt, args);
    va_end(args);
    return s;
}

} // namespace

// ==================================================================== filters

TEST_F(LoggerTest, LevelFilterAdmitsThisLevelAndEverythingMoreSevere) {
    Logger::setCategories(AllLogCategories);
    Logger::setLevel(LogLevel::Warn);

    EXPECT_TRUE(Logger::wants(LogLevel::Critical, LogCategory::Core));
    EXPECT_TRUE(Logger::wants(LogLevel::Error, LogCategory::Core));
    EXPECT_TRUE(Logger::wants(LogLevel::Warn, LogCategory::Core));
    EXPECT_FALSE(Logger::wants(LogLevel::Status, LogCategory::Core));
    EXPECT_FALSE(Logger::wants(LogLevel::Debug, LogCategory::Core));
}

TEST_F(LoggerTest, CategoryFilterIsIndependentOfLevel) {
    Logger::setLevel(LogLevel::Debug);
    Logger::setCategories(static_cast<uint32_t>(LogCategory::Vulkan));

    EXPECT_TRUE(Logger::wants(LogLevel::Debug, LogCategory::Vulkan));
    // An error in a disabled category is still filtered out. That is the contract, and
    // it is the one that surprises people.
    EXPECT_FALSE(Logger::wants(LogLevel::Error, LogCategory::Render));
}

TEST_F(LoggerTest, EnableCategoryTogglesOneBitAndLeavesTheRest) {
    Logger::setCategories(static_cast<uint32_t>(LogCategory::Core) | static_cast<uint32_t>(LogCategory::GLTF));

    Logger::enableCategory(LogCategory::Render, true);
    EXPECT_EQ(Logger::categories(), static_cast<uint32_t>(LogCategory::Core) |
                                        static_cast<uint32_t>(LogCategory::GLTF) |
                                        static_cast<uint32_t>(LogCategory::Render));

    Logger::enableCategory(LogCategory::GLTF, false);
    EXPECT_EQ(Logger::categories(),
              static_cast<uint32_t>(LogCategory::Core) | static_cast<uint32_t>(LogCategory::Render));

    // Enabling something already enabled is not a toggle.
    Logger::enableCategory(LogCategory::Core, true);
    EXPECT_EQ(Logger::categories(),
              static_cast<uint32_t>(LogCategory::Core) | static_cast<uint32_t>(LogCategory::Render));
}

TEST_F(LoggerTest, EnableCategoryIsSafeFromSeveralThreadsAtOnce) {
    // The compare-exchange loop exists because two subsystems can flip their own
    // category concurrently. A plain read-modify-write would lose one of them.
    Logger::setCategories(0);

    std::vector<std::thread> threads;
    // Every category, and the assertion below is what keeps it that way: a category added
    // without being listed here fails this test rather than quietly narrowing what the
    // concurrency check covers. S5 added Audio and this is where it was noticed.
    const LogCategory cats[] = {LogCategory::Core,   LogCategory::Vulkan,  LogCategory::GLTF,
                                LogCategory::Scene,  LogCategory::Render,  LogCategory::Asset,
                                LogCategory::Input,  LogCategory::Profile, LogCategory::Audio};
    for (LogCategory c : cats) {
        threads.emplace_back([c] {
            for (int i = 0; i < 200; ++i) Logger::enableCategory(c, true);
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(Logger::categories(), AllLogCategories);
}

// ================================================================== vformat

TEST_F(LoggerTest, FormatsIntoAnExactlySizedBuffer) {
    EXPECT_EQ(format("%s %d %.2f", "a", 42, 1.5), "a 42 1.50");
    EXPECT_EQ(format("no args"), "no args");
    EXPECT_EQ(format("%s", ""), "");
}

TEST_F(LoggerTest, FormatsAMessageLongerThanAnyFixedBuffer) {
    // vformat measures first and allocates second, on purpose: a fixed stack buffer is
    // how a long shader-compile error arrives truncated at the one line that mattered.
    const std::string filler(9000, 'x');

    const std::string out = format("[%s]", filler.c_str());
    EXPECT_EQ(out.size(), filler.size() + 2);
    EXPECT_EQ(out.front(), '[');
    EXPECT_EQ(out.back(), ']');
}

// =============================================================== file output

TEST_F(LoggerTest, ShutdownDrainsTheQueueBeforeTheFileIsReadable) {
    const fs::path log = dir / "substrate.log";
    Logger::init(log.string(), LogOutput::File);
    Logger::setLevel(LogLevel::Debug);
    Logger::setCategories(AllLogCategories);

    Logger::status(LogCategory::Core, "first message %d", 1);
    Logger::warn(LogCategory::Render, "second message");
    Logger::shutdown();

    const std::string text = readAll(log);
    EXPECT_NE(text.find("first message 1"), std::string::npos);
    EXPECT_NE(text.find("second message"), std::string::npos);
    EXPECT_NE(text.find("[STATUS]"), std::string::npos);
    EXPECT_NE(text.find("[Render]"), std::string::npos);
}

TEST_F(LoggerTest, CreatesTheParentDirectoryOfTheLogFile) {
    const fs::path log = dir / "nested" / "deeper" / "substrate.log";
    ASSERT_FALSE(fs::exists(log.parent_path()));

    Logger::init(log.string(), LogOutput::File);
    Logger::setLevel(LogLevel::Debug);
    Logger::status(LogCategory::Core, "made it");
    Logger::shutdown();

    EXPECT_TRUE(fs::exists(log));
    EXPECT_NE(readAll(log).find("made it"), std::string::npos);
}

TEST_F(LoggerTest, ReInitAppendsRatherThanTruncating) {
    const fs::path log = dir / "substrate.log";

    Logger::init(log.string(), LogOutput::File);
    Logger::setLevel(LogLevel::Debug);
    Logger::status(LogCategory::Core, "run one");
    Logger::shutdown();

    Logger::init(log.string(), LogOutput::File);
    Logger::setLevel(LogLevel::Debug);
    Logger::status(LogCategory::Core, "run two");
    Logger::shutdown();

    const std::string text = readAll(log);
    EXPECT_NE(text.find("run one"), std::string::npos) << "a second run must not eat the first run's log";
    EXPECT_NE(text.find("run two"), std::string::npos);
}

TEST_F(LoggerTest, FilteredMessagesNeverReachTheFile) {
    const fs::path log = dir / "substrate.log";
    Logger::init(log.string(), LogOutput::File);
    Logger::setLevel(LogLevel::Warn);
    Logger::setCategories(static_cast<uint32_t>(LogCategory::Core));

    Logger::warn(LogCategory::Core, "kept");
    Logger::status(LogCategory::Core, "dropped by level");
    Logger::warn(LogCategory::Vulkan, "dropped by category");
    Logger::shutdown();

    const std::string text = readAll(log);
    EXPECT_NE(text.find("kept"), std::string::npos);
    EXPECT_EQ(text.find("dropped by level"), std::string::npos);
    EXPECT_EQ(text.find("dropped by category"), std::string::npos);
}

TEST_F(LoggerTest, ConcurrentWritersAllLandInTheFile) {
    const fs::path log = dir / "substrate.log";
    Logger::init(log.string(), LogOutput::File);
    Logger::setLevel(LogLevel::Debug);
    Logger::setCategories(AllLogCategories);

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 50; ++i) Logger::status(LogCategory::Core, "thread %d line %d", t, i);
        });
    }
    for (auto& t : threads) t.join();
    Logger::shutdown();

    const std::string text = readAll(log);
    size_t lines = 0;
    for (char c : text) {
        if (c == '\n') ++lines;
    }
    EXPECT_EQ(lines, 200u) << "the queue must not drop or duplicate under contention";
}

TEST_F(LoggerTest, ShutdownIsIdempotentAndSafeWithoutInit) {
    Logger::shutdown();
    Logger::shutdown();
    SUCCEED();
}

// ================================================================== critical

TEST_F(LoggerTest, CriticalFlushesAndExitsNonZero) {
    // A death test, because that is what `critical` is: it emits, drains the queue and
    // calls exit(1). The flush is the part worth proving -- the whole reason it exists
    // is so the reason for the exit actually reaches the log.
    EXPECT_EXIT(
        {
            // Terminal-only, so the assertion below reads the child's stderr rather than
            // a log file no other process is going to open.
            Logger::init("", LogOutput::Terminal);
            Logger::setLevel(LogLevel::Debug);
            Logger::setCategories(AllLogCategories);
            Logger::critical(LogCategory::Vulkan, "device lost: %d", -4);
        },
        ::testing::ExitedWithCode(1), "device lost: -4");
}

TEST_F(LoggerTest, CriticalIgnoresTheLevelFilter) {
    // Filtering out the reason the process is about to die would be the worst possible
    // application of a log level.
    EXPECT_EXIT(
        {
            Logger::init("", LogOutput::Terminal);
            Logger::setLevel(LogLevel::Critical);
            Logger::setCategories(0);
            Logger::critical(LogCategory::Core, "unfiltered");
        },
        ::testing::ExitedWithCode(1), "unfiltered");
}
