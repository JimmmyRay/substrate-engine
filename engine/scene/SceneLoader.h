#pragma once

#include "scene/SceneData.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

/**
 * @file engine/scene/SceneLoader.h
 * @brief One scene parsed on a worker thread while the frame keeps running.
 *
 * A single slot: `begin`, `ready`, `take`.
 *
 * **The worker touches nothing but its own `SceneData` and `EmbeddedImages`** -- not the
 * device, the scene or the instance table, none of which is thread-safe. Everything that
 * needs those happens in `take`'s caller, on the frame thread, after `ready()`.
 *
 * The work is a `std::function` so this file stays in `SUBSTRATE_HOSTED_SOURCES`, which is
 * the only place `./test.sh tsan` reaches. Naming `scene::loadSceneCpu` here is a link
 * error in the unit suite, which links no glTF parser.
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
     * @return false when one is already in flight. A second `begin` cannot replace the
     *         first, which is already writing the result.
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
     * Joins the worker first, which is what makes this safe to call without asking
     * `ready()`; after a `ready()` the join costs nothing.
     *
     * @return false when nothing is ready, or when the load failed. The outputs are
     *         untouched in the first case and meaningless in the second.
     */
    [[nodiscard]] bool take(SceneData& outData, EmbeddedImages& outEmbedded);

    /// Block until the worker finishes.
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
