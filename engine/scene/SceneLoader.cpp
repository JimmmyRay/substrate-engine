#include "scene/SceneLoader.h"

#include "core/Logger.h"
#include "core/Profiler.h"

namespace scene {

SceneLoader::~SceneLoader() { wait(); }

bool SceneLoader::begin(std::string label, Work work) {
    if (state.load(std::memory_order_acquire) != State::Idle) return false;
    if (!work) return false;

    // A loader that has been taken from is Idle but may still hold the finished thread
    // object, and assigning over a joinable member calls std::terminate.
    if (worker.joinable()) worker.join();

    name = std::move(label);
    data = SceneData{};
    embedded.clear();
    state.store(State::Running, std::memory_order_release);

    worker = std::thread([this, work = std::move(work)]() {
        core::Profiler::nameThread("scene load");
        const bool ok = work(data, embedded);
        // The release store is the whole handshake: everything the work wrote into `data`
        // and `embedded` happens-before it. Nothing else here is synchronised, so anything
        // this lambda touches beyond those two is a race.
        state.store(ok ? State::Succeeded : State::Failed, std::memory_order_release);
    });
    return true;
}

bool SceneLoader::take(SceneData& outData, EmbeddedImages& outEmbedded) {
    const State s = state.load(std::memory_order_acquire);
    if (s != State::Succeeded && s != State::Failed) return false;

    // The worker has stored its result but may not have returned; without this join the
    // thread is still running when the caller starts using what it produced.
    if (worker.joinable()) worker.join();
    state.store(State::Idle, std::memory_order_release);

    if (s == State::Failed) {
        core::Logger::warn(core::LogCategory::Scene, "async load of %s failed", name.c_str());
        return false;
    }
    outData = std::move(data);
    outEmbedded = std::move(embedded);
    data = SceneData{};
    embedded.clear();
    return true;
}

void SceneLoader::wait() {
    if (worker.joinable()) worker.join();
}

} // namespace scene
