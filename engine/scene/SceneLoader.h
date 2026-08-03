#pragma once

#include "scene/SceneData.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

/**
 * @file engine/scene/SceneLoader.h
 * @brief One scene parsed on a worker thread while the frame keeps running (C10).
 *
 * ## What it is
 *
 * A single-slot job: `begin` starts one, `ready` says whether it has finished, `take` moves
 * the result out. No queue, no pool, no scheduler -- there is exactly one thing a game
 * streams at a time in this engine today, and a work queue built before a second caller
 * exists is a queue whose shape is a guess.
 *
 * ## Why the work is a `std::function` and not a call to `scene::loadSceneCpu`
 *
 * Because that is what keeps this file **hosted**, and hosted is where threading gets
 * checked. The unit suite is the only place `./test.sh tsan` runs, and a threaded class with
 * no ThreadSanitizer coverage is precisely the kind that works for months and then corrupts
 * a scene on somebody else's machine. Naming the load directly would put it out of reach:
 * until D9 that was because `GltfScene.h` reaches Vulkan, and since D9 it is because
 * `scene/SceneParse.cpp` is linked by the engine and by `substrate-bake` and not by the
 * suite, which has no glTF to parse. Either way the link fails, which is the point.
 *
 * So the caller supplies the work. `Engine` passes a lambda calling `scene::loadSceneCpu`;
 * the tests pass a lambda that fills a `SceneData` by hand and can be made to fail, block,
 * or race on demand. One `std::function` parameter, and it is not an abstraction layer over
 * anything -- there is no second implementation and no interface, only a seam where the
 * device half would otherwise have to be.
 *
 * ## The one rule
 *
 * **The worker touches nothing but its own `SceneData` and `EmbeddedImages`.** It does not
 * touch the device, the scene, or the instance table, because a queue is not thread-safe
 * and neither is anything else here. Everything that needs those happens in `take`'s
 * caller, on the frame thread, after `ready()`.
 */
namespace scene {

class SceneLoader {
  public:
    /// Fills the two outputs; returns false on failure. Runs on the worker thread and must
    /// touch nothing else.
    using Work = std::function<bool(SceneData&, EmbeddedImages&)>;

    SceneLoader() = default;
    /// Joins. A worker still parsing when the loader dies would write into freed memory,
    /// and a detached thread outliving the process's Vulkan teardown is worse.
    ~SceneLoader();

    SceneLoader(const SceneLoader&) = delete;
    SceneLoader& operator=(const SceneLoader&) = delete;

    /**
     * @brief Start a load.
     *
     * @param label What is being loaded, for the log line and for `label()`. Usually a path.
     * @return false when one is already in flight, which is not an error the caller has to
     *         handle so much as a question it has to answer: a second `begin` cannot
     *         silently replace the first, because the first is already writing.
     */
    bool begin(std::string label, Work work);

    /// A load is in flight. False before the first `begin` and after `take`.
    [[nodiscard]] bool busy() const { return state.load(std::memory_order_acquire) == State::Running; }
    /// The worker has finished, successfully or not, and the result is waiting to be taken.
    [[nodiscard]] bool ready() const {
        const State s = state.load(std::memory_order_acquire);
        return s == State::Succeeded || s == State::Failed;
    }
    /// Meaningful once `ready()`.
    [[nodiscard]] bool succeeded() const { return state.load(std::memory_order_acquire) == State::Succeeded; }
    [[nodiscard]] const std::string& label() const { return name; }

    /**
     * @brief Move the result out and return to idle.
     *
     * Joins the worker first, so what comes back is complete rather than nearly complete.
     * That join is free in the case that matters -- the caller asked `ready()` first -- and
     * it is the reason `take` is safe to call without one.
     *
     * @return false when nothing is ready, or when the load failed. The outputs are
     *         untouched in the first case and meaningless in the second.
     */
    [[nodiscard]] bool take(SceneData& outData, EmbeddedImages& outEmbedded);

    /// Block until the worker finishes. For a shutdown that cannot leave one running, and
    /// for a caller that has decided to wait after all.
    void wait();

  private:
    enum class State : uint8_t { Idle, Running, Succeeded, Failed };

    std::thread worker;
    /// The whole synchronisation. The worker writes it exactly once, with release, as its
    /// last act; every reader acquires. Everything the worker produced happens-before that
    /// store, so a caller that saw `Succeeded` sees finished data.
    std::atomic<State> state{State::Idle};
    std::string name;
    SceneData data;
    EmbeddedImages embedded;
};

} // namespace scene
