#pragma once

#include "gfx/Light.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

/**
 * @file engine/scene/SceneTypes.h
 * @brief The plain data a loaded scene is made of, with no device in sight.
 *
 * Separate from `scene/GltfScene.h`, which owns Vulkan buffers and so puts `volk.h` on the
 * include path of anything naming a `Vertex`. Adding a device type here breaks the hosted
 * build: `scene/SceneData.cpp` is in `SUBSTRATE_HOSTED_SOURCES` and includes this.
 */
namespace scene {

/// Interleaved vertex. Tangent w carries bitangent handedness, per the glTF spec.
struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::vec2 uv;
};

/// Mirrors the std430 layout of the material storage buffer the shaders read.
struct GpuMaterial {
    glm::vec4 baseColorFactor;
    glm::vec4 emissiveFactor; ///< w unused
    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;
    float normalScale;

    int32_t baseColorTexture;
    int32_t metallicRoughnessTexture;
    int32_t normalTexture;
    int32_t occlusionTexture;

    int32_t emissiveTexture;
    uint32_t alphaMask; ///< 1 = ALPHA_MODE MASK, sample and discard

    /**
     * @brief A slot in `gfx::ImageTable`, for a variant that samples a game's own image.
     *
     * **Not the same array as the four `int32_t` texture fields above**, which are slots in
     * the scene's own bindless array. This one indexes what a game loaded through
     * `e.images()`, bound as set 2 of the G-buffer and shadow layouts. Zero is
     * `ImageTable::kFallbackSlot`, so a zero-initialised material names something visible
     * rather than a descriptor nobody wrote.
     */
    uint32_t gameImage;

    /**
     * @brief Which `gfx::ShaderVariant` draws a surface wearing this material.
     *
     * An index into the renderer's variant list; `0` is the engine's own
     * gbuffer/shadow/forward triple, so a zero-initialised material names the default.
     *
     * **Read by the CPU, not by a shader.** `Renderer::updateInstances` groups draw commands
     * by it to bind one pipeline per group; a fragment stage is already in its pipeline and
     * has nothing to select. What game GLSL reads is `params`.
     */
    uint32_t shader;

    /// Four floats a variant's own GLSL means whatever it likes by. Untouched by the loader
    /// and by every engine shader; nothing here may start writing them.
    glm::vec4 params;
};

static_assert(sizeof(GpuMaterial) % 16 == 0, "GpuMaterial must stay std430-aligned");

/**
 * @brief Skinning influences for one vertex.
 *
 * A parallel array sized from the *skinned* primitives only, with `Primitive::skinOffset`
 * saying where each starts in it. Folding these into `Vertex` costs 32 bytes on every
 * vertex of every scene, skinned or not -- 4.8 MB on Sponza, which has no skin at all.
 */
struct SkinVertex {
    /// glTF allows UNSIGNED_BYTE or UNSIGNED_SHORT joints; both widen to this.
    glm::uvec4 joints{0u};
    glm::vec4 weights{0.0f};
};

/**
 * @brief One morph target's displacement of one vertex.
 *
 * Nine tightly packed floats read under `layout(scalar)` -- see skinning.comp. Zero is the
 * identity, so a target carrying fewer than three channels leaves the rest alone and needs
 * no flag. Stored per (target, vertex) in **target-major** order, which is the order the
 * shader's addressing assumes.
 */
struct MorphDelta {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec3 tangent{0.0f};
};

static_assert(sizeof(MorphDelta) == 36, "MorphDelta must stay tightly packed for scalar layout");

/**
 * @brief What holds one vertex of a soft body up, or does not.
 *
 * A parallel array sized from the `FABRIC_` primitives alone, with
 * `Primitive::clothOffset` saying where each starts.
 *
 * **`invMass`, not `pinWeight`** -- the authoring value is the inverse of this, and
 * `clothInvMass()` is the one place the two vocabularies meet. **Consumed exactly once**,
 * when a soft body is built; from then on Jolt's own vertex array is the authority, so
 * writing here afterwards changes nothing.
 */
struct ClothVertex {
    /// Zero is pinned -- immovable, infinite mass. One is free. Between them is a vertex
    /// that is heavy but mobile, which is what a fractional `_PIN_WEIGHT` authors.
    float invMass = 1.0f;
};

static_assert(sizeof(ClothVertex) == 4, "ClothVertex is one float; the sidecar assumes it");

/// Coarser levels a primitive may carry beyond LOD 0. A budget, not a limit of the
/// technique: each level halves the triangle count, and it is what keeps `Primitive::lods`
/// inline rather than a `std::vector`.
inline constexpr uint32_t kMaxLodLevels = 3;

/// One level of a chain: a second range of the same shared index buffer, over the same
/// vertices -- `meshopt_simplify` returns indices into the original array, so a chain costs
/// indices and no vertices.
struct LodRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};

/// A contiguous range of the scene's shared index buffer.
struct Primitive {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t materialIndex = -1;

    /// First vertex of this primitive in the shared vertex buffer, and how many. The index
    /// buffer holds absolute indices; these are for the skinning dispatch, which works in
    /// vertices.
    uint32_t baseVertex = 0;
    uint32_t vertexCount = 0;
    /// Where this primitive's influences start in the skin-vertex array, or UINT32_MAX when
    /// it has none. The sentinel is the flag; a bool beside it could disagree with it.
    uint32_t skinOffset = 0xFFFFFFFFu;

    /**
     * @brief Where this primitive's inverse masses start in the scene's cloth array, or
     *        UINT32_MAX when it is not fabric.
     *
     * **Per primitive, not per mesh.** Blender splits a mesh by material, so a curtain
     * wearing two is two primitives -- and a half with no pinned vertex is a separate body
     * that falls. `scripts/check_pins.py` checks "at least one vertex pinned" at this same
     * granularity from the other side of the exporter.
     */
    uint32_t clothOffset = 0xFFFFFFFFu;

    /// Where this primitive's morph deltas start, and how many targets it has. Zero targets
    /// means the offset is never read, so here the count is the flag.
    uint32_t morphOffset = 0;
    uint32_t morphTargets = 0;

    /// ALPHA_MODE BLEND, resolved from the material at load so that neither the record loop
    /// nor the instance table reaches back into the material array to ask.
    bool blended = false;

    /// ALPHA_MODE MASK. A *shadow* property rather than a lighting one: the depth-only pass
    /// drops its fragment shader for everything that is not masked, and needs to know which
    /// draws those are without reading the material buffer per fragment.
    bool masked = false;

    /// Object-space bounds. Scene bounds must be accumulated from these *after* the
    /// node transform is applied — Sponza's root node scales by 0.008, so raw vertex
    /// bounds are off by more than two orders of magnitude.
    glm::vec3 localMin{0.0f};
    glm::vec3 localMax{0.0f};

    /**
     * @brief The simplified levels this primitive carries, coarsest last.
     *
     * **Inline, so that a chain is rebased, serialised, appended and read by the same
     * statement that moves the primitive.** A parallel array indexed by primitive goes out
     * of step the moment `appendModel` grows `prims`.
     *
     * `lodCount` counts entries of `lods`, and zero -- the default -- means no chain. Level
     * 0 is `firstIndex` and `indexCount` above and is not repeated here.
     */
    LodRange lods[kMaxLodLevels];
    uint32_t lodCount = 0;
};

/**
 * @brief Geometry a game built rather than loaded: the input to `GltfScene::createMesh`.
 *
 * Bounds are computed from the vertices when `localMax` is not greater than `localMin`.
 * Supplying them is for a mesh a vertex shader will displace past them.
 */
struct MeshData {
    std::vector<Vertex> vertices;
    /// Zero-based, into `vertices`. `createMesh` rebases them onto whatever range the
    /// shared buffer hands out.
    std::vector<uint32_t> indices;
    /// Index into the material table, from `createMaterial` or from the loaded scene.
    /// Out of range falls back to material 0 with a warning rather than reading past it.
    uint32_t material = 0;
    /// Where the one placement goes. A caller wanting forty makes forty instances.
    glm::mat4 transform{1.0f};
    bool blended = false;
    bool masked = false;
    glm::vec3 localMin{0.0f};
    glm::vec3 localMax{0.0f};

    /**
     * @brief Morph targets, one entry per target. Deltas, not displaced positions.
     *
     * **Each target must be exactly `vertices.size()` long.** The shader addresses a
     * displacement as `morphOffset + target * vertexCount + vertex`, so a short target does
     * not read a default -- it reads the next target's first rows. `createMesh` refuses the
     * whole mesh rather than padding.
     */
    std::vector<std::vector<MorphDelta>> morphTargets;
};

/// One primitive at one world transform, as the glTF node hierarchy placed it. What the
/// file says, not what the renderer draws: `addSceneInstances()` is the separate call that
/// makes a placement drawable.
struct Placement {
    uint32_t primitive = 0;
    glm::mat4 transform{1.0f};
    /// Node that placed it, so an animated hierarchy can push a new transform into the
    /// instance every frame.
    uint32_t node = 0;
    /// Skin driving it, or UINT32_MAX for rigid geometry.
    uint32_t skin = 0xFFFFFFFFu;
    /**
     * @brief Nearest ancestor node carrying a `substrate_collider`, or UINT32_MAX.
     *
     * **Not `node`.** A rig's meshes hang several levels below the node an author puts a
     * capsule on, so matching a body to what it moves by `node` alone finds nothing for
     * every skinned character. Inherited down the import walk, which is the only place the
     * hierarchy exists -- `GltfScene` keeps placements and a rig, not a scene graph.
     */
    uint32_t colliderNode = 0xFFFFFFFFu;
};

struct SceneStats {
    /// The three phases inside `parseMs`: the mmap, the shared `extras` pass and the walks
    /// over its nodes, and fastgltf's own structure building, base64 and external buffers.
    /// One number over all three attributes nothing to any of them.
    double mmapMs = 0.0;
    double extrasMs = 0.0;
    double gltfMs = 0.0;

    uint32_t nodes = 0;
    uint32_t meshes = 0;
    uint32_t primitives = 0;
    uint32_t draws = 0;
    uint32_t blendedDraws = 0; ///< of `draws`, how many went to the forward list
    uint32_t materials = 0;
    uint32_t textures = 0;
    uint32_t alphaMaskedMaterials = 0;
    uint32_t blendedMaterials = 0;
    uint64_t vertexCount = 0;
    uint64_t indexCount = 0;
    uint64_t textureBytes = 0;
    /// Images that came from a `.ktx2` cache entry rather than a PNG decode.
    uint32_t compressedTextures = 0;
    uint32_t skins = 0;
    uint32_t animations = 0;
    /// Particle emitters placed by a node's `extras.substrate_emitter`.
    uint32_t emitters = 0;
    /// Collision shapes placed by a node's `extras.substrate_collider`.
    uint32_t colliders = 0;
    /// Sounds placed by a node's `extras.substrate_audio`.
    uint32_t audioSources = 0;
    uint32_t skinnedVertices = 0;
    /// Primitives carrying an LOD chain, and the indices those chains added to the shared
    /// buffer. Both zero for a scene loaded from a document; chains are a *bake* product.
    uint32_t lodPrimitives = 0;
    uint32_t lodIndices = 0;
    /// Morph targets summed over every primitive that has any, and the vertices they
    /// displace -- the two numbers that decide what the delta buffer costs.
    uint32_t morphTargets = 0;
    uint32_t morphedVertices = 0;
    /// `FABRIC_` primitives and the vertices they pin.
    uint32_t clothPrimitives = 0;
    uint32_t clothVertices = 0;
    double parseMs = 0.0;
    double geometryMs = 0.0;
    /// Time spent reading the sidecar instead of the document. Non-zero exactly when
    /// `parseMs` and `geometryMs` are zero, which is what says which path a load took.
    double cacheMs = 0.0;
    double textureMs = 0.0;
    double totalMs = 0.0;
};

} // namespace scene
