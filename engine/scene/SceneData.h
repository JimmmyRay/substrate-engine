#pragma once

#include "scene/Animation.h"
#include "scene/AudioSource.h"
#include "scene/Collider.h"
#include "scene/SceneTypes.h"
#include "scene/ParticleEmitter.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/**
 * @file engine/scene/SceneData.h
 * @brief Everything `GltfScene::load` derives from a document, without the device.
 *
 * The payload of the `.scene` sidecar, and the half of a load that a `VkDevice` is not
 * needed for. It carries an image *list*, never pixels -- so a scene with an embedded
 * image and no `.ktx2` beside it is refused at bake time rather than written as a sidecar
 * that still needs the document. See systems.md, "glTF loading".
 */
namespace scene {

/// One image the scene references, in the form the texture pass needs and nothing more.
struct SceneImageRef {
    /// Document-relative, exactly as authored. Empty means the payload is embedded in the
    /// document -- reachable only through the `.ktx2` beside it, which is why
    /// `writeSceneCache` refuses a scene where one of those is missing.
    std::string uri;
    /// Decided by which material slot uses it, not by the file: colour is authored in
    /// sRGB and must be decoded on read, a normal or ORM map must not be. Carried rather
    /// than recomputed because getting it wrong is silent.
    bool srgb = false;
};

/// Payloads for images a GLB embedded, parallel to `SceneData::images`, and outside
/// `SceneData` because the sidecar never holds pixels. Empty for a scene that came from a
/// cache, which by construction has a `.ktx2` beside every embedded image it once had.
using EmbeddedImages = std::vector<std::vector<uint8_t>>;

/// The whole CPU-side result of loading a scene: a payload, not an owner.
struct SceneData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GpuMaterial> materials;
    /// One byte per material: does it emit? Read by the acceleration-structure build to
    /// decide which geometry may occlude a shadow ray.
    std::vector<uint8_t> materialEmissive;
    std::vector<SceneImageRef> images;

    std::vector<Primitive> primitives;
    std::vector<Placement> placements;

    std::vector<gfx::GpuLight> lights;
    std::vector<ParticleEmitter> emitters;
    std::vector<ColliderDesc> colliders;
    std::vector<AudioSourceDesc> audioSources;

    AnimationRig rig;
    std::vector<SkinVertex> skinVertices;
    std::vector<MorphDelta> morphDeltas;
    /// One inverse mass per vertex of every `FABRIC_` primitive, and nothing else.
    std::vector<ClothVertex> clothVertices;
    /// Retained only when something in the scene deforms.
    std::vector<uint32_t> indexCopy;

    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    /// **Braced, and not for style.** `SceneStats` is written to the sidecar as one
    /// `memcpy` including its padding, and padding in a merely default-initialised object
    /// is indeterminate -- which makes the bake unreproducible and `cmp` useless on a
    /// `.scene`.
    SceneStats stats{};
};

/**
 * @brief Resize a loaded scene in place, before anything has consumed it.
 *
 * **Static content scales; dynamic content only moves.** Anything a solver or a rig drives
 * -- a placement naming a skin, a collider whose motion is not `Static` -- keeps the size
 * its author gave it and has only its position carried outward, because a character grown
 * with the building has exactly the room it started with. Collider *shapes* are left alone
 * for a different reason: `createBody` applies the scale already in the transform, so
 * scaling the half-extents here applies it twice.
 *
 * Light range scales linearly and **intensity by the square**, which is inverse-square
 * falloff over the new distance; a directional light has neither and is left alone.
 *
 * Must run between `loadSceneCpu` and the upload, the one point both the document and the
 * sidecar have passed through -- baking the scale in would give every factor its own cache.
 * A `scale` of 1, or a non-positive or NaN one, returns immediately.
 */
void scaleSceneData(SceneData& data, float scale);

/**
 * @brief Move a loaded scene bodily to where a caller wants it, before anything has
 *        consumed it.
 *
 * Uniform, with none of `scaleSceneData`'s static/dynamic split: an import placed thirty
 * metres east means its character is thirty metres east too.
 *
 * Two things are not points. A light's **direction** takes the rotation only, since
 * `transform * vec4(dir, 1)` would drag it to wherever the import was placed. The
 * **bounds** are refitted around all eight transformed corners; transforming `min` and
 * `max` alone gives a box that is wrong for every rotation off a quarter turn.
 */
void placeSceneData(SceneData& data, const glm::mat4& transform);

/// Where the sidecar for `source` lives: beside it, with `.scene` appended to the whole
/// name. Appended rather than substituted, or `foo.gltf` and `foo.glb` in one directory
/// would share one cache.
[[nodiscard]] std::filesystem::path sceneCachePath(const std::filesystem::path& source);

/**
 * @brief Read the sidecar for `source`, if there is a usable one.
 *
 * @return false whenever the cache does not apply -- absent, truncated, a different format
 *         version, a different struct layout, or older than the source. Not an error in any
 *         of those cases: falling back to the document is the correct behaviour, and a load
 *         that logged for a cache nobody built teaches people to build one to silence it.
 */
[[nodiscard]] bool readSceneCache(const std::filesystem::path& source, SceneData& out);

/**
 * @brief Write the sidecar for `source`.
 *
 * **Defined in `SceneCacheWrite.cpp`, which a game does not link.** A static library links
 * by object file, so the separate translation unit is what makes "a shipped game cannot
 * produce a `.scene`" a link-time fact rather than a promise.
 *
 * @return false if it could not be written, or if the scene has an embedded image with no
 *         `.ktx2` beside it. Both log, because unlike a *missing* cache a *refused* one is
 *         something the person who asked for it wants to know about.
 */
[[nodiscard]] bool writeSceneCache(const std::filesystem::path& source, const SceneData& data);

/// Bytes the sidecar for `data` would occupy. Exposed for the bake's log line and for the
/// round-trip test, which asserts a reader consumed exactly what a writer produced.
[[nodiscard]] size_t sceneCacheSize(const SceneData& data);

/// The format version in the header.
///
/// **Bump it whenever the payload changes shape**, including when the layout digest would
/// have caught it anyway and when nothing binary moved at all -- a reader that changes what
/// the same bytes *mean* invalidates every sidecar in existence, and neither the digest nor
/// `SourceStamp` can see that. Not bumping leaves an old sidecar read as though the missing
/// fields were zero. Every `.scene` is then re-baked on its next load, once, by
/// `scripts/bake.sh` or the load itself.
inline constexpr uint32_t kSceneCacheVersion = 6;

} // namespace scene
