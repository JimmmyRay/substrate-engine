#include "core/Logger.h"

#include <gtest/gtest.h>

using namespace core;

/**
 * @file tests/main.cpp
 * @brief Entry point for the unit suite (5.1).
 *
 * A hand-written main rather than `gtest_main` for one reason: the code under test
 * logs on purpose. `Profiler::initialize` announces itself, the scopef name pool warns
 * when it fills, and `Config::loadFromFile` warns about every missing file a test
 * deliberately points it at. At the default level that is several hundred lines of
 * noise wrapped around the handful that matter, so the floor is raised here and the
 * three tests that assert on a specific message lower it themselves.
 *
 * The binary links only the hosted translation units -- no Vulkan, no window, no
 * shaders -- which is what lets it run under ThreadSanitizer, where the renderer
 * cannot go at all because the proprietary driver segfaults inside vkCreateDevice.
 */
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    Logger::setLevel(LogLevel::Error);
    return RUN_ALL_TESTS();
}
