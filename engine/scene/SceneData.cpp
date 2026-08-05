#include "scene/SceneData.h"

#include "scene/SceneCacheFormat.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <system_error>

/**
 * @file engine/scene/SceneData.cpp
 * @brief What can be done to a loaded `SceneData` without a device: the sidecar **reader**,
 * the scale pass and the place pass.
 *
 * The writer stays in `SceneCacheWrite.cpp`: a static library links by object file, so
 * anything added here is code every binary that reads a sidecar also carries. The format
 * itself is `SceneCacheFormat.h`, where each `get` sits beside its `put`.
 */
namespace scene {

std::filesystem::path sceneCachePath(const std::filesystem::path& source) {
    return std::filesystem::path(source).concat(".scene");
}

namespace {

/// Move a transform's translation without touching its basis, so a thing that keeps its
/// authored size still lands where the scaled world put its surroundings.
void carryTranslation(glm::mat4& transform, float scale) {
    transform[3] = glm::vec4(glm::vec3(transform[3]) * scale, transform[3].w);
}

} // namespace

void scaleSceneData(SceneData& data, float scale) {
    // A non-positive or NaN factor is refused rather than clamped: a scene silently
    // collapsed to the origin is harder to recognise than one that did not move. The
    // `!(x > 0)` spelling is what catches the NaN.
    if (!(scale > 0.0f) || scale == 1.0f) return;

    const glm::mat4 whole = glm::scale(glm::mat4(1.0f), glm::vec3(scale));

    // Gathered before the placement loop, which asks about its ancestor rather than itself.
    std::vector<uint32_t> drivenNodes;
    for (const ColliderDesc& c : data.colliders) {
        if (c.motion != ColliderMotion::Static && c.node != kNoNode) drivenNodes.push_back(c.node);
    }
    const auto driven = [&drivenNodes](uint32_t node) {
        return node != kNoNode && std::find(drivenNodes.begin(), drivenNodes.end(), node) != drivenNodes.end();
    };

    // **An assembly has to move rigidly.** A rig's mesh and the capsule that drives it are
    // two records of one object, so translating each by its own scaled position multiplies
    // the gap between them by the factor. Nothing looks wrong standing still -- but
    // `initPhysics` binds the mesh through `inverse(rest) * meshTransform`, and the mesh
    // then swings around the body on that radius every time it turns. So the collider is
    // the anchor and every placement it drives takes *its* delta.
    std::vector<std::pair<uint32_t, glm::vec3>> anchorDelta;
    anchorDelta.reserve(drivenNodes.size());
    for (const ColliderDesc& c : data.colliders) {
        if (c.motion == ColliderMotion::Static || c.node == kNoNode) continue;
        anchorDelta.emplace_back(c.node, glm::vec3(c.transform[3]) * (scale - 1.0f));
    }
    const auto deltaFor = [&anchorDelta](uint32_t node) -> const glm::vec3* {
        for (const auto& [n, d] : anchorDelta) {
            if (n == node) return &d;
        }
        return nullptr;
    };

    for (Placement& p : data.placements) {
        const bool keepsItsSize = p.skin != 0xFFFFFFFFu || driven(p.colliderNode);
        if (!keepsItsSize) {
            p.transform = whole * p.transform;
            continue;
        }
        if (const glm::vec3* anchor = deltaFor(p.colliderNode)) {
            p.transform[3] = glm::vec4(glm::vec3(p.transform[3]) + *anchor, p.transform[3].w);
        } else {
            carryTranslation(p.transform, scale);
        }
    }

    for (ColliderDesc& c : data.colliders) {
        if (c.motion == ColliderMotion::Static) {
            c.transform = whole * c.transform;
        } else {
            carryTranslation(c.transform, scale);
        }
    }

    for (gfx::GpuLight& l : data.lights) {
        // A directional light has no position, no meaningful range and no falloff, so all
        // three writes below would be noise on it.
        if (static_cast<uint32_t>(l.params.z) == static_cast<uint32_t>(gfx::LightType::Directional)) continue;
        l.position = glm::vec4(glm::vec3(l.position) * scale, l.position.w * scale);
        l.color.w *= scale * scale; // inverse square; see the header
    }

    for (ParticleEmitter& e : data.emitters) {
        // Lengths only. Particle sizes and speeds keep their authored values -- an ember is
        // an ember in a cathedral of any size.
        e.transform = whole * e.transform;
        e.boxExtent *= scale;
    }

    for (AudioSourceDesc& a : data.audioSources) {
        // Translation only: `AudioSourceDesc`'s basis is read for aim, and scaling it
        // would change nothing but the numbers.
        carryTranslation(a.transform, scale);
        a.minDistance *= scale;
        a.maxDistance *= scale;
    }

    data.boundsMin *= scale;
    data.boundsMax *= scale;
}

void placeSceneData(SceneData& data, const glm::mat4& transform) {
    if (transform == glm::mat4(1.0f)) return;

    const glm::mat3 rotation(transform);

    for (Placement& p : data.placements) p.transform = transform * p.transform;
    for (ColliderDesc& c : data.colliders) c.transform = transform * c.transform;
    for (ParticleEmitter& e : data.emitters) e.transform = transform * e.transform;
    for (AudioSourceDesc& a : data.audioSources) a.transform = transform * a.transform;

    for (gfx::GpuLight& l : data.lights) {
        // The rotation alone: the full matrix drags a direction to wherever the import was
        // placed, pointing every spot in the file at one spot in the world.
        const glm::vec3 aimed = rotation * glm::vec3(l.direction);
        l.direction = glm::vec4(aimed, l.direction.w);
        if (static_cast<uint32_t>(l.params.z) == static_cast<uint32_t>(gfx::LightType::Directional)) continue;
        l.position = glm::vec4(glm::vec3(transform * glm::vec4(glm::vec3(l.position), 1.0f)), l.position.w);
    }

    // Eight corners, refitted: transforming `min` and `max` alone gives a box that does not
    // contain the scene for any rotation off a multiple of a quarter turn.
    const glm::vec3 lo = data.boundsMin;
    const glm::vec3 hi = data.boundsMax;
    glm::vec3 newMin(std::numeric_limits<float>::max());
    glm::vec3 newMax(std::numeric_limits<float>::lowest());
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const glm::vec3 point((corner & 1u) != 0u ? hi.x : lo.x, (corner & 2u) != 0u ? hi.y : lo.y,
                              (corner & 4u) != 0u ? hi.z : lo.z);
        const glm::vec3 moved(transform * glm::vec4(point, 1.0f));
        newMin = glm::min(newMin, moved);
        newMax = glm::max(newMax, moved);
    }
    data.boundsMin = newMin;
    data.boundsMax = newMax;
}

bool readSceneCache(const std::filesystem::path& source, SceneData& out) {
    using namespace scene::cache;

    const std::filesystem::path cachePath = sceneCachePath(source);

    SourceStamp want;
    if (!stampOf(source, want)) return false;

    std::error_code ec;
    const auto cacheBytes = std::filesystem::file_size(cachePath, ec);
    if (ec) return false;

    std::ifstream in(cachePath, std::ios::binary);
    if (!in) return false;

    // Sized, then one `read`. `istreambuf_iterator` into a vector goes through the stream a
    // character at a time and reallocates as it grows: 34 ms against 4 ms on Sponza's 12 MB
    // sidecar, which is a cache that loses to the parser it replaces.
    std::vector<uint8_t> bytes(static_cast<size_t>(cacheBytes));
    if (bytes.size() < kHeaderSize) return false;
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<size_t>(in.gcount()) != bytes.size()) return false;

    Reader r{bytes.data(), bytes.data() + bytes.size(), true};

    char magic[sizeof(kMagic)]{};
    r.raw(magic, sizeof(magic));
    if (!r.ok || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return false;

    if (r.pod<uint32_t>() != kSceneCacheVersion) return false;
    if (r.pod<uint32_t>() != kLayoutDigest) return false;

    const auto have = r.pod<SourceStamp>();
    if (!r.ok || have.size != want.size || have.mtime != want.mtime) return false;

    SceneData data;
    r.podVector(data.vertices);
    r.podVector(data.indices);
    r.podVector(data.materials);
    r.podVector(data.materialEmissive);
    if (!getList(r, data.images)) return false;
    r.podVector(data.primitives);
    r.podVector(data.placements);
    r.podVector(data.lights);
    if (!getList(r, data.emitters)) return false;
    if (!getList(r, data.colliders)) return false;
    if (!getList(r, data.audioSources)) return false;
    get(r, data.rig);
    r.podVector(data.skinVertices);
    r.podVector(data.morphDeltas);
    r.podVector(data.clothVertices);
    r.podVector(data.indexCopy);
    data.boundsMin = r.pod<glm::vec3>();
    data.boundsMax = r.pod<glm::vec3>();
    data.stats = r.pod<SceneStats>();

    // Every byte, or none: a reader that stopped early consumed a file it did not
    // understand, whatever the header claimed.
    if (!r.ok || r.p != r.end) return false;

    out = std::move(data);
    return true;
}

} // namespace scene
