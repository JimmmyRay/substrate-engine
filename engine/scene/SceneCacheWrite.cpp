#include "scene/SceneData.h"

#include "scene/SceneCacheFormat.h"

#include "core/FileWrite.h"
#include "core/Logger.h"

#include <filesystem>

/**
 * @file engine/scene/SceneCacheWrite.cpp
 * @brief The sidecar **writer**, in the one translation unit a game never links.
 *
 * `substrate-bake` links this and so does the unit suite; `libsubstrate.a` does not, and
 * neither does any game built on it. That link-time boundary is what holds the invariant
 * that a running process never writes a file a later run reads as an input -- a `#if` here
 * could be crossed by accident, and this cannot.
 */
namespace scene {

size_t sceneCacheSize(const SceneData& data) {
    cache::Writer w;
    cache::writeBody(w, data);
    return w.bytes.size() + cache::kHeaderSize;
}

bool writeSceneCache(const std::filesystem::path& source, const SceneData& data) {
    // An embedded image is reachable only through the document -- see SceneData.h -- so a
    // sidecar for a scene holding one with no `.ktx2` beside it would need the very file it
    // exists to avoid opening. Refused loudly: whoever asked for a bake wants to know it did
    // not happen.
    for (size_t i = 0; i < data.images.size(); ++i) {
        if (!data.images[i].uri.empty()) continue;
        const std::filesystem::path ktx =
            source.parent_path() / (source.stem().string() + ".image" + std::to_string(i) + ".ktx2");
        if (std::filesystem::exists(ktx)) continue;
        core::Logger::warn(core::LogCategory::GLTF,
                           "%s: image %zu is embedded and has no %s beside it, so a scene cache could not be read "
                           "back without the document. Run scripts/ktx2.py first; nothing written",
                           source.string().c_str(), i, ktx.filename().string().c_str());
        return false;
    }

    cache::SourceStamp stamp;
    if (!cache::stampOf(source, stamp)) return false;

    cache::Writer w;
    w.raw(cache::kMagic, sizeof(cache::kMagic));
    w.pod(kSceneCacheVersion);
    w.pod(cache::kLayoutDigest);
    w.pod(stamp);
    cache::writeBody(w, data);

    // Written and then renamed, so a build killed mid-write leaves the previous cache rather
    // than destroying a good answer to produce a truncated one.
    const std::filesystem::path cachePath = sceneCachePath(source);
    if (!core::writeFileAtomically(cachePath, w.bytes.data(), w.bytes.size(), core::LogCategory::GLTF,
                                   "scene cache")) {
        return false;
    }

    core::Logger::status(core::LogCategory::GLTF, "scene cache: %s (%.2f MB)",
                         cachePath.filename().string().c_str(),
                         static_cast<double>(w.bytes.size()) / (1024.0 * 1024.0));
    return true;
}

} // namespace scene
