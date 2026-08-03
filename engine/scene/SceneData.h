#pragma once

#include "scene/Animation.h"
#include "scene/AudioSource.h"
#include "scene/Collider.h"
#include "scene/SceneTypes.h"
#include "scene/ParticleSystem.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/**
 * @file engine/scene/SceneData.h
 * @brief Everything `GltfScene::load` derives from a document, without the device (C15).
 *
 * ## Why this is a struct and not a method
 *
 * Re-deriving a scene from JSON on every launch -- parsing the document, de-interleaving
 * every accessor, flattening the node tree and cooking every collision mesh -- is a
 * development convenience. No engine ships it to a player, which is why every engine that
 * ships has a cooked form of its scenes. C13 measured what that costs here: on an
 * 8000-node scene the whole load was ~44 ms after C14, and none of it is work the answer
 * changes between runs.
 *
 * So the CPU half of the load is named. `SceneData` is what it produces and the only
 * thing the sidecar holds; `GltfScene::load` keeps the half that needs a `VkDevice` --
 * decode, upload, descriptors -- and that half runs identically whichever way this struct
 * was filled.
 *
 * ## The contract, which is `ktx2.py`'s
 *
 * > Nothing rewrites the glTF, and deleting the cache restores the original behaviour
 * > exactly.
 *
 * A sidecar beside the source, optional, silently ignored when it does not apply. It is
 * invalidated three ways and every one of them is checked before a byte of payload is
 * read:
 *
 * 1. **A format version** in the header, bumped by hand when the payload changes shape.
 * 2. **A layout digest** folded from `sizeof` of every POD written. A `Vertex` that grows
 *    a field is a `static_assert` at build time *and* a rejected cache at run time, which
 *    is the belt the version alone does not provide -- nobody remembers to bump it.
 * 3. **The source's size and mtime.** Editing the glTF invalidates the cache without
 *    anyone having to remember the cache exists.
 *
 * ## What it deliberately does not hold
 *
 * **Pixels.** Textures are `scripts/ktx2.py`'s problem and were solved there; a scene
 * sidecar that embedded them would be a second texture format to keep in step, and larger
 * than the glTF it came from. What travels instead is the *image list* -- a URI and an
 * sRGB flag per image -- which is the one part of the texture pass that is not device
 * work.
 *
 * That has a consequence worth stating rather than discovering: an image embedded in a
 * `.glb` cannot be reached without the document, so **a scene whose embedded images have
 * no `.ktx2` beside them is refused at bake time** rather than written as a sidecar that
 * silently needs the very file it exists to avoid opening.
 */
namespace scene {

/// One image the scene references, in the form the texture pass needs and nothing more.
struct SceneImageRef {
    /// Document-relative, exactly as authored. Empty means the payload is embedded in the
    /// document -- reachable only through the `.ktx2` beside it, which is why
    /// `writeSceneCache` refuses a scene where one of those is missing.
    std::string uri;
    /// Decided by which material slot uses it, not by the file: colour is authored in
    /// sRGB and must be decoded on read, a normal or ORM map must not be. Getting it
    /// wrong is silent, which is why it is carried rather than recomputed.
    bool srgb = false;
};

/**
 * @brief The whole CPU-side result of loading a scene.
 *
 * Field for field what `GltfScene` used to fill directly. The members are public and
 * plain because this is a payload, not an owner: it is filled by the parser, written to a
 * blob, read back from one, and consumed by the upload half.
 */
/// Payloads for images a GLB embedded, parallel to `SceneData::images`. Kept *outside*
/// `SceneData` on purpose -- C15's sidecar holds an image *list*, never pixels -- and
/// therefore empty for a scene that came from a cache, which by construction has a `.ktx2`
/// beside every embedded image it once had.
using EmbeddedImages = std::vector<std::vector<uint8_t>>;

struct SceneData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GpuMaterial> materials;
    /// One byte per material: does it emit? The acceleration-structure build needs it to
    /// decide which geometry may occlude a shadow ray, and it is the one part of the
    /// material table that stays on the CPU.
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
    /// One inverse mass per vertex of every `FABRIC_` primitive, and nothing else (C19).
    /// Empty for every scene in this repository that does not author cloth, which is all
    /// of them but `cloth.gltf`.
    std::vector<ClothVertex> clothVertices;
    /// Retained only when something in the scene deforms (S2.5). Empty for Sponza.
    std::vector<uint32_t> indexCopy;

    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    /// Braced, and not for style. `SceneStats` is written to the sidecar as one `memcpy`,
    /// which carries the padding between its `uint32_t` runs and its `double`s along with
    /// it -- and padding in a merely default-initialised object is indeterminate, so the
    /// bake would not be reproducible. Value-initialisation zeroes the whole subobject,
    /// padding included, which is what makes `cmp` a usable check on a `.scene` (D9).
    SceneStats stats{};
};

/**
 * @brief Resize a loaded scene in place, before anything has consumed it.
 *
 * ## Static content scales; dynamic content only moves
 *
 * The point of scaling a level is to get *room*, and scaling everything gives none: a
 * character grown with the building has exactly the nave it started with. So the rule is
 * that geometry which never moves is scaled whole -- position and size -- while anything
 * a solver or a rig drives keeps the size its author gave it and has only its position
 * carried outward. Two tests decide it, and neither needs a list:
 *
 *   - `Placement::skin` names a rig, and a rig's proportions are its own.
 *   - `ColliderDesc::motion` is not `Static`, which is the whole of what "dynamic" means
 *     here; a placement inherits it through `Placement::colliderNode`.
 *
 * A shape needs no separate treatment: `makeShape` builds it from `halfExtent`/`radius`/
 * `halfHeight` and `createBody` then applies whatever scale is in the transform, so a
 * static collider that got the scale in its transform gets it in its shape for free --
 * and scaling the numbers here as well would apply it twice.
 *
 * ## Intensity is not a length, and is scaled anyway
 *
 * A punctual light obeys inverse square, so a surface that was 5 m from a lamp and is now
 * 10 m from it receives a quarter of the light. Range scales linearly and intensity by the
 * square, which is what leaves the room looking like the room rather than like a cave.
 * A directional light has no position term and is left alone.
 *
 * ## Once, and after the cache
 *
 * Applied to the CPU-side data between `loadSceneCpu` and the upload, which is the one
 * point both the document and the `.scene` sidecar have passed through. Baking the scale
 * into the sidecar instead would give every factor its own cache.
 *
 * A `scale` of 1 returns immediately, and so does a non-positive or NaN one -- a scene
 * mirrored or collapsed to a point is not something a caller meant to ask for.
 */
void scaleSceneData(SceneData& data, float scale);

/**
 * @brief Move a loaded scene bodily to where a caller wants it, before anything has
 *        consumed it.
 *
 * The other half of importing a second document: `scaleSceneData` decides how big it is,
 * this decides where it is. Everything a scene *has* goes through the same matrix --
 * placements, colliders, emitters, audio sources, lights and the bounds.
 *
 * ## No static/dynamic split here, unlike the scale pass
 *
 * Scaling has one because growing a character defeats the point of growing the room. Moving
 * one does not: a rotation and a translation apply to a rig exactly as they apply to a wall,
 * and an import placed thirty metres east means its character is thirty metres east too. So
 * this pass is uniform and needs none of `scaleSceneData`'s tests.
 *
 * ## The two things that are not transforms
 *
 * A light's **direction** takes the rotation and not the translation -- it is a direction,
 * and `transform * vec4(dir, 1)` would drag it to wherever the import was placed. A
 * directional light has no position to move and takes only the rotation.
 *
 * The **bounds** are an axis-aligned box, and a rotated box is not one. All eight corners go
 * through the matrix and the result is refitted around them, which is the only way to get a
 * box that still contains the scene. Taking `transform * min` and `transform * max` gives a
 * box that is wrong for every rotation that is not a multiple of a quarter turn.
 *
 * The identity returns immediately, which is what an import placed at the origin costs.
 */
void placeSceneData(SceneData& data, const glm::mat4& transform);

/// Where the sidecar for `source` lives: beside it, with `.scene` appended to the whole
/// name. Appended rather than substituted for the same reason `ktx2.py` writes
/// `foo.png.ktx2` -- `foo.gltf` and `foo.glb` in one directory are two scenes, and
/// replacing the extension would give them one cache between them.
[[nodiscard]] std::filesystem::path sceneCachePath(const std::filesystem::path& source);

/**
 * @brief Read the sidecar for `source`, if there is a usable one.
 *
 * @return false whenever the cache does not apply -- absent, truncated, a different
 *         format version, a different struct layout, or older than the source. Never an
 *         error: falling back to the document is the correct behaviour in every one of
 *         those cases, and a load that logged an error for a cache nobody built would
 *         teach people to build one to silence it.
 */
[[nodiscard]] bool readSceneCache(const std::filesystem::path& source, SceneData& out);

/**
 * @brief Write the sidecar for `source`.
 *
 * **Defined in `SceneCacheWrite.cpp`, which a game does not link** (D9). Declared here
 * beside its reader because the two are one contract, but only `substrate-bake` and the
 * unit suite have a definition for it -- a static library links by object file, so the
 * separate translation unit is what makes "a shipped game cannot produce a `.scene`" a
 * link-time fact rather than a promise.
 *
 * @return false if it could not be written, or if the scene has an embedded image with no
 *         `.ktx2` beside it -- see the file comment for why that case is refused rather
 *         than written. Both say so in the log, because unlike a *missing* cache, a
 *         *refused* one is something the person who asked for it wants to know about.
 */
[[nodiscard]] bool writeSceneCache(const std::filesystem::path& source, const SceneData& data);

/// Bytes the sidecar for `data` would occupy. Exposed for the log line the bake prints,
/// and for the round-trip test, which is the only way to assert that a reader consumed
/// exactly what a writer produced.
[[nodiscard]] size_t sceneCacheSize(const SceneData& data);

/// The format version in the header. Bumped by hand when the payload's shape changes;
/// the layout digest below catches the changes people forget to bump for.
///
/// **3 is C17's LOD chains.** A `Primitive` grew its levels, so every version-2 sidecar in
/// existence describes a scene whose chains are not in the bytes -- and the point of a
/// version is that such a cache is *dropped* rather than read as though the missing fields
/// were zero. The layout digest would have caught this one on its own, since `Primitive`
/// changed size; the bump is here because the rule is that a payload changing shape bumps
/// it, and a rule followed only when the belt is missing is not a rule.
///
/// **4 is C19's cloth.** `clothVertices` is a new array in the payload and `Primitive` grew
/// a `clothOffset`, so a version-3 sidecar is one array short *and* describes primitives of
/// the wrong size. Every existing `.scene` is invalidated by this and the next load of each
/// is slow; that is the cache working rather than a regression, and it is a one-off per
/// sidecar. Re-bake with `scripts/bake.sh` to get the fast path back.
///
/// **5 is `AnimationRig::nodeNames`.** A list of strings in the payload, and the layout digest
/// could not have caught it: the digest folds `sizeof` over the PODs written verbatim, and a
/// `std::vector<std::string>` on a hand-serialised struct changes none of those numbers. This
/// is the case the rule above exists for -- a payload changing shape with the belt silent --
/// and reading a version-4 sidecar with the version-5 reader would take the clip count out of
/// the middle of the names.
///
/// **6 is the `.collider` node-name convention**, and it is the case where nothing about the
/// format moved at all. A version-5 sidecar is structurally readable and *semantically wrong*:
/// it was baked by a parser that drew collision nodes and authored no body for them, so its
/// `placements` still hold the meshes this rule suppresses and its `colliders` lack the ones it
/// synthesises. `SourceStamp` cannot catch it either -- the `.gltf` is byte-identical, only the
/// code reading it changed -- so the version is the only belt that fits. A rule that silently
/// does nothing on every already-baked scene is the failure this number prevents.
inline constexpr uint32_t kSceneCacheVersion = 6;

} // namespace scene
