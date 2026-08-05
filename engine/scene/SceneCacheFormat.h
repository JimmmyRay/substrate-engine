#pragma once

#include "scene/SceneData.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <type_traits>
#include <vector>

/**
 * @file engine/scene/SceneCacheFormat.h
 * @brief The `.scene` wire format, shared by the reader and the writer.
 *
 * Every `put` is adjacent to its `get`: the two halves of a serializer are the classic place
 * for a field to be added to one and not the other, and the round-trip test in
 * tests/SceneDataTests.cpp is what catches it when they disagree.
 *
 * The two functions are two translation units on purpose -- `SceneData.cpp` reads,
 * `SceneCacheWrite.cpp` writes, and only `substrate-bake` and the unit suite link the
 * second. A static library links by object file, so putting them together would ship every
 * game that can read a sidecar the code to write one.
 */
namespace scene::cache {

/// Eight bytes, so a file that is not one of these is rejected on its first word rather than
/// by arithmetic on garbage. **Never version it** -- the version is its own field, and a
/// magic that moved with the format would report every old cache as "not a scene file"
/// instead of "a scene file this build cannot read".
inline constexpr char kMagic[8] = {'S', 'B', 'S', 'C', 'E', 'N', 'E', '\0'};

// Every POD written verbatim below. These are the build-time half of the layout check -- a
// field added to any of them stops the build here -- and `kLayoutDigest` is the run-time
// half, folded from the same numbers. Between them a struct change cannot produce a cache
// that loads and is wrong, which is the failure a version number alone leaves open.
static_assert(sizeof(Vertex) == 48, "Vertex changed: bump kSceneCacheVersion");
static_assert(sizeof(SkinVertex) == 32, "SkinVertex changed: bump kSceneCacheVersion");
static_assert(sizeof(MorphDelta) == 36, "MorphDelta changed: bump kSceneCacheVersion");
static_assert(sizeof(ClothVertex) == 4, "ClothVertex changed: bump kSceneCacheVersion");
static_assert(sizeof(GpuMaterial) % 16 == 0, "GpuMaterial changed: bump kSceneCacheVersion");
static_assert(sizeof(Primitive) == 92, "Primitive changed: bump kSceneCacheVersion");
static_assert(sizeof(Placement) == 80, "Placement changed: bump kSceneCacheVersion");
static_assert(sizeof(gfx::GpuLight) == 64, "GpuLight changed: bump kSceneCacheVersion");
static_assert(sizeof(SceneNode) == 52, "SceneNode changed: bump kSceneCacheVersion");
static_assert(sizeof(AnimationChannel) == 12, "AnimationChannel changed: bump kSceneCacheVersion");
static_assert(sizeof(SceneStats) == 184, "SceneStats changed: bump kSceneCacheVersion");

/// The same sizes, folded. FNV-1a over the numbers rather than a sum, so two changes that
/// cancel -- a field moved from one struct to another -- still change the digest.
constexpr uint32_t layoutDigest() {
    const size_t sizes[] = {sizeof(Vertex),      sizeof(SkinVertex),       sizeof(MorphDelta),
                            sizeof(ClothVertex), sizeof(GpuMaterial),      sizeof(Primitive),
                            sizeof(Placement),   sizeof(gfx::GpuLight),    sizeof(SceneNode),
                            sizeof(AnimationChannel),
                            sizeof(SceneStats),  sizeof(ColliderDesc),     sizeof(AudioSourceDesc),
                            sizeof(ParticleEmitter)};
    uint32_t h = 2166136261u;
    for (size_t s : sizes) {
        for (unsigned b = 0; b < sizeof(size_t); ++b) {
            h ^= static_cast<uint32_t>((s >> (b * 8)) & 0xFFu);
            h *= 16777619u;
        }
    }
    return h;
}

inline constexpr uint32_t kLayoutDigest = layoutDigest();

/// The source's identity, so an edited glTF invalidates its own cache. Size and mtime rather
/// than a content hash: hashing a 60 MB document costs a measurable fraction of the parse it
/// is trying to skip.
struct SourceStamp {
    uint64_t size = 0;
    int64_t mtime = 0;
};

[[nodiscard]] inline bool stampOf(const std::filesystem::path& source, SourceStamp& out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(source, ec);
    if (ec) return false;
    const auto when = std::filesystem::last_write_time(source, ec);
    if (ec) return false;
    out.size = static_cast<uint64_t>(size);
    out.mtime = when.time_since_epoch().count();
    return true;
}

/// Magic, version, layout digest, stamp. Named because the reader checks a file is at
/// least this long before it reads a byte and the writer adds it to the payload size.
inline constexpr size_t kHeaderSize = sizeof(kMagic) + sizeof(uint32_t) * 2 + sizeof(SourceStamp);

/// Appended to, never seeked: the whole payload is built in memory and written once, since a
/// write per field would be thousands of syscalls to save an allocation of a few megabytes.
struct Writer {
    std::vector<uint8_t> bytes;

    void raw(const void* p, size_t n) {
        const auto* src = static_cast<const uint8_t*>(p);
        bytes.insert(bytes.end(), src, src + n);
    }
    template <typename T> void pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        raw(&v, sizeof(T));
    }
    void count(size_t n) { pod(static_cast<uint64_t>(n)); }
    template <typename T> void podVector(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        count(v.size());
        if (!v.empty()) raw(v.data(), v.size() * sizeof(T));
    }
    void text(const std::string& s) {
        count(s.size());
        if (!s.empty()) raw(s.data(), s.size());
    }
};

/// Bounds-checked on every read: a truncated cache is the normal result of a build killed
/// mid-write, and it has to fall back to the document rather than read past the buffer.
struct Reader {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    bool ok = true;

    void raw(void* dst, size_t n) {
        if (!ok || static_cast<size_t>(end - p) < n) {
            ok = false;
            return;
        }
        std::memcpy(dst, p, n);
        p += n;
    }
    template <typename T> T pod() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v{};
        raw(&v, sizeof(T));
        return v;
    }
    /// Checked against the bytes that remain before anything is reserved for it: a trusted
    /// length is how a truncated file turns into a multi-gigabyte allocation.
    size_t count(size_t elementSize) {
        const auto n = static_cast<size_t>(pod<uint64_t>());
        if (!ok) return 0;
        if (elementSize != 0 && n > static_cast<size_t>(end - p) / elementSize) {
            ok = false;
            return 0;
        }
        return n;
    }
    template <typename T> void podVector(std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const size_t n = count(sizeof(T));
        if (!ok) return;
        v.resize(n);
        if (n != 0) raw(v.data(), n * sizeof(T));
    }
    void text(std::string& s) {
        const size_t n = count(1);
        if (!ok) return;
        s.resize(n);
        if (n != 0) raw(s.data(), n);
    }
};

/**
 * @brief A copy of `v` whose padding bytes are zero.
 *
 * Three of the `put`s below write a *byte range* of their struct, which includes whatever
 * the compiler left between a `bool` and the `float` after it. Nothing reads those bytes
 * back, but they are written, and in an object nobody zero-filled they are indeterminate --
 * enough to make two bakes of one unchanged document differ under `cmp`, which is how every
 * build output in this tree is checked.
 *
 * `T out{}` value-initialises the padding as well, and the memberwise assignment that
 * follows cannot become a `memcpy` that brings it back -- which is what the static_assert
 * holds to.
 */
template <typename T> [[nodiscard]] T zeroPadded(const T& v) {
    static_assert(!std::is_trivially_copyable_v<T>,
                  "zeroPadded relies on a memberwise copy assignment, which a trivially copyable type does not have");
    T out{};
    out = v;
    return out;
}

inline void put(Writer& w, const SceneImageRef& v) {
    w.text(v.uri);
    w.pod(v.srgb);
}
inline void get(Reader& r, SceneImageRef& v) {
    r.text(v.uri);
    v.srgb = r.pod<bool>();
}

inline void put(Writer& w, const ParticleEmitter& v) {
    w.text(v.name);
    // The trailing runtime fields (`accumulator`, `emitted`) go with it: they are zero in an
    // emitter the loader just built, and the whole range is one memcpy rather than thirty
    // field writes to save eight bytes per emitter.
    const ParticleEmitter z = zeroPadded(v);
    w.raw(reinterpret_cast<const uint8_t*>(&z) + offsetof(ParticleEmitter, transform),
          sizeof(ParticleEmitter) - offsetof(ParticleEmitter, transform));
}
inline void get(Reader& r, ParticleEmitter& v) {
    r.text(v.name);
    r.raw(reinterpret_cast<uint8_t*>(&v) + offsetof(ParticleEmitter, transform),
          sizeof(ParticleEmitter) - offsetof(ParticleEmitter, transform));
}

inline void put(Writer& w, const ColliderDesc& v) {
    w.text(v.name);
    const ColliderDesc z = zeroPadded(v);
    w.raw(reinterpret_cast<const uint8_t*>(&z) + offsetof(ColliderDesc, transform),
          offsetof(ColliderDesc, points) - offsetof(ColliderDesc, transform));
    w.podVector(v.points);
    w.podVector(v.indices);
}
inline void get(Reader& r, ColliderDesc& v) {
    r.text(v.name);
    r.raw(reinterpret_cast<uint8_t*>(&v) + offsetof(ColliderDesc, transform),
          offsetof(ColliderDesc, points) - offsetof(ColliderDesc, transform));
    r.podVector(v.points);
    r.podVector(v.indices);
}

inline void put(Writer& w, const AudioSourceDesc& v) {
    w.text(v.name);
    w.text(v.file);
    w.text(v.bus);
    const AudioSourceDesc z = zeroPadded(v);
    w.raw(reinterpret_cast<const uint8_t*>(&z) + offsetof(AudioSourceDesc, transform),
          offsetof(AudioSourceDesc, bus) - offsetof(AudioSourceDesc, transform));
    w.raw(reinterpret_cast<const uint8_t*>(&z) + offsetof(AudioSourceDesc, volume),
          sizeof(AudioSourceDesc) - offsetof(AudioSourceDesc, volume));
}
inline void get(Reader& r, AudioSourceDesc& v) {
    r.text(v.name);
    r.text(v.file);
    r.text(v.bus);
    r.raw(reinterpret_cast<uint8_t*>(&v) + offsetof(AudioSourceDesc, transform),
          offsetof(AudioSourceDesc, bus) - offsetof(AudioSourceDesc, transform));
    r.raw(reinterpret_cast<uint8_t*>(&v) + offsetof(AudioSourceDesc, volume),
          sizeof(AudioSourceDesc) - offsetof(AudioSourceDesc, volume));
}

inline void put(Writer& w, const AnimationSampler& v) {
    w.podVector(v.times);
    w.podVector(v.values);
    w.pod(v.interpolation);
    w.podVector(v.weights);
    w.pod(v.stride);
}
inline void get(Reader& r, AnimationSampler& v) {
    r.podVector(v.times);
    r.podVector(v.values);
    v.interpolation = r.pod<AnimationInterpolation>();
    r.podVector(v.weights);
    v.stride = r.pod<uint32_t>();
}

inline void put(Writer& w, const AnimationClip& v) {
    w.text(v.name);
    w.count(v.events.size());
    for (const core::AnimationEvent& e : v.events) {
        w.pod(e.time);
        w.text(e.name);
    }
    w.pod(v.duration);
    w.count(v.samplers.size());
    for (const AnimationSampler& s : v.samplers) put(w, s);
    w.podVector(v.channels);
}
inline void get(Reader& r, AnimationClip& v) {
    r.text(v.name);
    // Two fields per event and neither is fixed-size, so the count is checked against one
    // byte per element rather than a stride: the weakest bound that still refuses a length
    // which could not possibly fit.
    const size_t events = r.count(1);
    if (!r.ok) return;
    v.events.resize(events);
    for (core::AnimationEvent& e : v.events) {
        e.time = r.pod<float>();
        r.text(e.name);
    }
    v.duration = r.pod<float>();
    const size_t samplers = r.count(1);
    if (!r.ok) return;
    v.samplers.resize(samplers);
    for (AnimationSampler& s : v.samplers) get(r, s);
    r.podVector(v.channels);
}

inline void put(Writer& w, const AnimationRig& v) {
    w.podVector(v.bind.nodes);
    // Its own run rather than folded into the nodes: `SceneNode` is a POD written verbatim,
    // and a string in it would end that. Dropping these makes `SceneAnimator::findNode`
    // answer `kNoNode` for every name on the cached path -- which is the usual path.
    w.count(v.nodeNames.size());
    for (const std::string& n : v.nodeNames) w.text(n);
    w.podVector(v.bind.weights);
    w.count(v.skins.size());
    for (const Skin& s : v.skins) {
        w.podVector(s.joints);
        w.podVector(s.inverseBind);
    }
    w.count(v.clips.size());
    for (const AnimationClip& c : v.clips) put(w, c);
}
inline void get(Reader& r, AnimationRig& v) {
    r.podVector(v.bind.nodes);
    const size_t names = r.count(1);
    if (!r.ok) return;
    v.nodeNames.resize(names);
    for (std::string& n : v.nodeNames) r.text(n);
    r.podVector(v.bind.weights);
    const size_t skins = r.count(1);
    if (!r.ok) return;
    v.skins.resize(skins);
    for (Skin& s : v.skins) {
        r.podVector(s.joints);
        r.podVector(s.inverseBind);
    }
    const size_t clips = r.count(1);
    if (!r.ok) return;
    v.clips.resize(clips);
    for (AnimationClip& c : v.clips) get(r, c);
}

template <typename T> void putList(Writer& w, const std::vector<T>& v) {
    w.count(v.size());
    for (const T& item : v) put(w, item);
}
template <typename T> bool getList(Reader& r, std::vector<T>& v) {
    const size_t n = r.count(1);
    if (!r.ok) return false;
    v.resize(n);
    for (T& item : v) get(r, item);
    return r.ok;
}

/**
 * @brief The stats block as it is written, which is not the stats block as it was
 *        measured.
 *
 * Every duration in `SceneStats` is time *this* run spent, so writing them makes two bakes
 * of one unchanged document differ in those eight doubles and stops the sidecar being
 * checkable with `cmp`. Nothing reads them back: `loadSceneCpu` clears the parse timings on
 * a cache hit, and `textureMs` and `totalMs` are measured by the upload half after the bake
 * is written. The counts beside them are properties of the scene and stay.
 */
[[nodiscard]] inline SceneStats bakedStats(const SceneStats& measured) {
    SceneStats out = measured;
    out.mmapMs = 0.0;
    out.extrasMs = 0.0;
    out.gltfMs = 0.0;
    out.parseMs = 0.0;
    out.geometryMs = 0.0;
    out.cacheMs = 0.0;
    out.textureMs = 0.0;
    out.totalMs = 0.0;
    return out;
}

inline void writeBody(Writer& w, const SceneData& d) {
    w.podVector(d.vertices);
    w.podVector(d.indices);
    w.podVector(d.materials);
    w.podVector(d.materialEmissive);
    putList(w, d.images);
    w.podVector(d.primitives);
    w.podVector(d.placements);
    w.podVector(d.lights);
    putList(w, d.emitters);
    putList(w, d.colliders);
    putList(w, d.audioSources);
    put(w, d.rig);
    w.podVector(d.skinVertices);
    w.podVector(d.morphDeltas);
    w.podVector(d.clothVertices);
    w.podVector(d.indexCopy);
    w.pod(d.boundsMin);
    w.pod(d.boundsMax);
    w.pod(bakedStats(d.stats));
}

} // namespace scene::cache
