#include "scene/SceneLoader.h"

#include "core/Logger.h"
#include "core/Profiler.h"

namespace scene {

SceneLoader::~SceneLoader() { wait(); }

bool SceneLoader::begin(std::string label, Work work) {
    // One at a time, and refused rather than queued. A second load replacing the first
    // would have to stop a thread that is mid-parse, and there is no correct way to do that
    // which is cheaper than waiting for it.
    if (state.load(std::memory_order_acquire) != State::Idle) return false;
    if (!work) return false;

    // Joined here rather than left for the destructor: a loader that has been taken from is
    // Idle but may still hold the finished thread object, and starting a second load into a
    // joinable member is what std::terminate is for.
    if (worker.joinable()) worker.join();

    name = std::move(label);
    data = SceneData{};
    embedded.clear();
    state.store(State::Running, std::memory_order_release);

    worker = std::thread([this, work = std::move(work)]() {
        core::Profiler::nameThread("scene load");
        const bool ok = work(data, embedded);
        // The release store is the whole handshake. Everything the work wrote into `data`
        // and `embedded` happens-before it, so a frame thread that acquires `Succeeded`
        // sees all of it -- and nothing else here is synchronised, because nothing else is
        // shared.
        state.store(ok ? State::Succeeded : State::Failed, std::memory_order_release);
    });
    return true;
}

bool SceneLoader::take(SceneData& outData, EmbeddedImages& outEmbedded) {
    const State s = state.load(std::memory_order_acquire);
    if (s != State::Succeeded && s != State::Failed) return false;

    // The worker has stored its result but may not have returned. Joining costs nothing
    // here and is what makes "ready" mean "finished" rather than "nearly".
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
