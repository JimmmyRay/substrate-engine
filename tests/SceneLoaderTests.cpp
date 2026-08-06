#include "scene/SceneLoader.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace scene;

/**
 * @file tests/SceneLoaderTests.cpp
 * @brief The worker handshake, under ThreadSanitizer.
 *
 * These exist because of where they run rather than what they assert. `SceneLoader` is the
 * only threaded thing C10 adds, and the unit suite is the only place `scripts/test.sh tsan` is
 * able to look at it -- a loader wired directly to `GltfScene` could not be linked here at
 * all, which is the entire reason its work is a `std::function`.
 *
 * So the load-bearing test is `ResultsPublishedByTheWorkerAreVisibleToTheTaker`: it writes
 * a megabyte on the worker and reads it back on the main thread, which is the pattern a
 * missing release/acquire pair actually breaks. It passes trivially on x86 whatever the
 * memory ordering says; it is TSan that makes it mean something.
 */
namespace {

/// Work that succeeds after filling `data` with something recognisable.
SceneLoader::Work fills(uint32_t vertexCount, bool ok = true) {
    return [vertexCount, ok](SceneData& data, EmbeddedImages& embedded) {
        data.vertices.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i) data.vertices[i].position = glm::vec3(static_cast<float>(i));
        embedded.resize(2);
        embedded[0] = {1, 2, 3};
        return ok;
    };
}

void spinUntilReady(const SceneLoader& loader) {
    while (!loader.ready()) std::this_thread::yield();
}

} // namespace

TEST(SceneLoader, StartsIdle) {
    const SceneLoader loader;
    EXPECT_FALSE(loader.busy());
    EXPECT_FALSE(loader.ready());
    EXPECT_TRUE(loader.label().empty());
}

TEST(SceneLoader, ResultsPublishedByTheWorkerAreVisibleToTheTaker) {
    // A megabyte of vertices, written on one thread and read on another. This is the shape
    // a missing release/acquire pair breaks, and the reason the suite runs under TSan.
    constexpr uint32_t kCount = 65536;
    SceneLoader loader;
    ASSERT_TRUE(loader.begin("big.gltf", fills(kCount)));
    spinUntilReady(loader);
    EXPECT_TRUE(loader.succeeded());

    SceneData data;
    EmbeddedImages embedded;
    ASSERT_TRUE(loader.take(data, embedded));
    ASSERT_EQ(data.vertices.size(), kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        ASSERT_FLOAT_EQ(data.vertices[i].position.x, static_cast<float>(i)) << "vertex " << i << " was not published";
    }
    EXPECT_EQ(embedded.size(), 2u);
}

TEST(SceneLoader, TakeReturnsToIdleSoAnotherLoadCanStart) {
    SceneLoader loader;
    ASSERT_TRUE(loader.begin("first", fills(4)));
    spinUntilReady(loader);

    SceneData data;
    EmbeddedImages embedded;
    ASSERT_TRUE(loader.take(data, embedded));
    EXPECT_FALSE(loader.busy());
    EXPECT_FALSE(loader.ready());

    // The second begin has to join the first worker before reusing the thread member.
    ASSERT_TRUE(loader.begin("second", fills(7)));
    spinUntilReady(loader);
    ASSERT_TRUE(loader.take(data, embedded));
    EXPECT_EQ(data.vertices.size(), 7u);
    EXPECT_EQ(loader.label(), "second");
}

TEST(SceneLoader, ASecondBeginWhileBusyIsRefused) {
    // Refused rather than queued: replacing an in-flight load means stopping a thread
    // mid-parse, and there is no correct way to do that cheaper than waiting.
    std::atomic<bool> release{false};
    SceneLoader loader;
    ASSERT_TRUE(loader.begin("slow", [&release](SceneData& data, EmbeddedImages&) {
        while (!release.load()) std::this_thread::yield();
        data.vertices.resize(1);
        return true;
    }));

    EXPECT_TRUE(loader.busy());
    EXPECT_FALSE(loader.begin("other", fills(1))) << "the first load is still writing";
    EXPECT_EQ(loader.label(), "slow");

    release.store(true);
    spinUntilReady(loader);
    SceneData data;
    EmbeddedImages embedded;
    EXPECT_TRUE(loader.take(data, embedded));
}

TEST(SceneLoader, AFailedLoadIsReadyButNotSucceeded) {
    SceneLoader loader;
    ASSERT_TRUE(loader.begin("broken.gltf", fills(3, false)));
    spinUntilReady(loader);

    EXPECT_TRUE(loader.ready());
    EXPECT_FALSE(loader.succeeded());

    SceneData data;
    EmbeddedImages embedded;
    data.vertices.resize(9);
    EXPECT_FALSE(loader.take(data, embedded)) << "a failed load must not be applied";
    // And it still returns to idle, so a failure does not wedge the loader.
    EXPECT_FALSE(loader.busy());
    EXPECT_TRUE(loader.begin("retry", fills(2)));
    spinUntilReady(loader);
    EXPECT_TRUE(loader.take(data, embedded));
}

TEST(SceneLoader, TakingBeforeReadyReturnsNothing) {
    std::atomic<bool> release{false};
    SceneLoader loader;
    ASSERT_TRUE(loader.begin("slow", [&release](SceneData&, EmbeddedImages&) {
        while (!release.load()) std::this_thread::yield();
        return true;
    }));

    SceneData data;
    EmbeddedImages embedded;
    data.vertices.resize(5);
    EXPECT_FALSE(loader.take(data, embedded));
    EXPECT_EQ(data.vertices.size(), 5u) << "a take that returned false must not have written";

    release.store(true);
    loader.wait();
    EXPECT_TRUE(loader.take(data, embedded));
}

TEST(SceneLoader, EmptyWorkIsRefused) {
    SceneLoader loader;
    EXPECT_FALSE(loader.begin("nothing", {}));
    EXPECT_FALSE(loader.busy());
}

TEST(SceneLoader, DestructionJoinsARunningWorker) {
    // The worker writes into members of the loader. One still parsing when the loader dies
    // is a use-after-free, and it is the failure ASan and TSan are both here for.
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    {
        SceneLoader loader;
        ASSERT_TRUE(loader.begin("slow", [&release, &finished](SceneData& data, EmbeddedImages&) {
            while (!release.load()) std::this_thread::yield();
            data.vertices.resize(128);
            finished.store(true);
            return true;
        }));
        release.store(true);
    }
    EXPECT_TRUE(finished.load()) << "the destructor returned before the worker did";
}

TEST(SceneLoader, WaitIsSafeWhenNothingIsRunning) {
    SceneLoader loader;
    loader.wait();
    EXPECT_FALSE(loader.busy());
    ASSERT_TRUE(loader.begin("x", fills(1)));
    loader.wait();
    EXPECT_TRUE(loader.ready());
    loader.wait();
    SceneData data;
    EmbeddedImages embedded;
    EXPECT_TRUE(loader.take(data, embedded));
}
