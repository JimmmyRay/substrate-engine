#pragma once

#include "gfx/Light.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

/**
 * @file engine/scene/SceneTypes.h
 * @brief The plain data a loaded scene is made of, with no device in sight (C15).
 *
 * Split out of `scene/GltfScene.h` for the reason `ui/FontMetrics.h` was split out of
 * `ui/Font.h` and `gfx/Decal.h` out of `gfx/Renderer.h`: `GltfScene` owns Vulkan buffers,
 * so its header puts `volk.h` on the include path of anything that so much as names a
 * `Vertex`. These structs are arithmetic and layout -- what a vertex is, what a primitive
 * spans, what the loader counted -- and none of them has ever needed a `VkDevice`.
 *
 * C15 is what forced it. A scene cache is written and read without a device, which means
 * `scene/SceneData.cpp` is in `SUBSTRATE_HOSTED_SOURCES` and therefore cannot include a
 * header that reaches Vulkan. The split was already the right shape; the sidecar is only
 * the thing that made it a link error rather than a preference.
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
     * @brief A slot in `gfx::ImageTable`, for a variant that samples a game's own image (P6).
     *
     * The word that used to be `occlusionTextureUnused`: renamed rather than added, so the
     * struct's size and every offset in it stay exactly where they were. **This is not
     * `baseColorTexture` and does not index the same array.** The four `int32_t` texture
     * fields above are slots in the *scene's* bindless array, which holds what a glTF
     * brought; this is a slot in the array a game loaded through `e.images()`, bound as set
     * 2 of the G-buffer and shadow layouts.
     *
     * Zero is `ImageTable::kFallbackSlot` -- the font atlas -- so a zero-initialised
     * material names something visible and harmless rather than a descriptor nobody wrote,
     * which is the same property C5 chose over `PARTIALLY_BOUND` and P1 kept. No loader
     * writes it and no engine shader but `sprite_lit` reads it.
     */
    uint32_t gameImage;

    /**
     * @brief Which `gfx::ShaderVariant` draws a surface wearing this material (G5).
     *
     * An index into the renderer's variant list, `0` being the engine's own
     * gbuffer/shadow/forward triple. So a zero-initialised material -- which is what
     * every loader and every `GpuMaterial m{}` produces -- already names the default,
     * and nothing that predates variants has to be told about them.
     *
     * **Read by the CPU, not by a shader.** `Renderer::updateInstances` groups draw
     * commands by it so a pass binds one pipeline per group; the fragment stage is
     * already *in* that pipeline by the time it runs and has nothing to select. What
     * game GLSL reads is `params`.
     */
    uint32_t shader;

    /**
     * @brief Four floats a variant's own GLSL means whatever it likes by (G5).
     *
     * Untouched by the loader and by every engine shader. That is the point: it is
     * where a game puts the numbers a glTF material has no field for -- a pulse rate,
     * a stripe width, a blend weight -- without every game widening this struct and
     * with no second buffer to keep in step with the material table.
     */
    glm::vec4 params;
};

static_assert(sizeof(GpuMaterial) % 16 == 0, "GpuMaterial must stay std430-aligned");

/**
 * @brief Skinning influences for one vertex (4.4).
 *
 * A parallel array rather than four more fields on `Vertex`. Sponza has 150k vertices
 * and no skin at all; widening every vertex by 32 bytes to carry joints nothing reads
 * would cost 4.8 MB to describe a feature the file does not use. This array is sized
 * from the *skinned* primitives only, and `Primitive::skinOffset` says where each one
 * starts in it.
 */
struct SkinVertex {
    /// glTF allows UNSIGNED_BYTE or UNSIGNED_SHORT joints; both widen to this.
    glm::uvec4 joints{0u};
    glm::vec4 weights{0.0f};
};

/**
 * @brief One morph target's displacement of one vertex (S2.1).
 *
 * Nine tightly packed floats, read under `layout(scalar)` for exactly the reason
 * `Vertex` is -- see skinning.comp. The three channels are the three glTF permits a
 * target to carry; a target that declares fewer leaves the rest zero, which is the
 * identity for a displacement and needs no flag to say so.
 *
 * Stored per (target, vertex) in target-major order, so the weighted sum a shader
 * computes walks one contiguous run per target rather than striding by target count.
 */
struct MorphDelta {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec3 tangent{0.0f};
};

static_assert(sizeof(MorphDelta) == 36, "MorphDelta must stay tightly packed for scalar layout");

/**
 * @brief What holds one vertex of a soft body up, or does not (C19).
 *
 * A parallel array for the reason `SkinVertex` is one, and the arithmetic is the same:
 * Sponza has 150k vertices and not one of them is fabric, so a float on `Vertex` to
 * describe a feature the file does not use costs 600 KB to say nothing. Sized from the
 * `FABRIC_` primitives alone, with `Primitive::clothOffset` saying where each one starts.
 *
 * **`invMass`, not `pinWeight`.** The authoring value is a weight -- 1 pinned, 0 free --
 * and `JPH::SoftBodySharedSettings::Vertex::mInvMass` is its inverse, with zero meaning
 * pinned. `clothInvMass()` is the one place the two vocabularies meet, and this array is
 * on the engine's side of it, so nothing downstream has to remember which convention it
 * is holding.
 *
 * **This array is a load-time input consumed exactly once**, which is the whole of why it
 * cannot go stale the way `GltfScene::indexData()` did (G11). A soft body reads it when it
 * is built and never again; from that moment the authority for a vertex's inverse mass is
 * Jolt's own vertex array, which the solver owns and updates in place. There is no second
 * copy tracking a first, because after the build there is no first.
 */
struct ClothVertex {
    /// Zero is pinned -- immovable, infinite mass. One is free. Between them is a vertex
    /// that is heavy but mobile, which is what a fractional `_PIN_WEIGHT` authors.
    float invMass = 1.0f;
};

static_assert(sizeof(ClothVertex) == 4, "ClothVertex is one float; the sidecar assumes it");

/**
 * @brief Coarser levels a primitive may carry beyond LOD 0 (C17).
 *
 * Three, and the number is a budget rather than a limit the technique imposes: each level
 * halves the triangle count, so a fourth is an eighth of the original and a mesh that is
 * an eighth of anything at the distance this fires is already fewer pixels than triangles.
 * It is also what keeps the chain *inline* -- see `Primitive::lods`, which is the whole of
 * why this is a constant and not a `std::vector`.
 */
inline constexpr uint32_t kMaxLodLevels = 3;

/// One level of a chain: a second range of the same shared index buffer, over the same
/// vertices. Levels share the vertex buffer -- `meshopt_simplify` returns indices into the
/// original array -- so a chain costs indices and no vertices at all.
struct LodRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};

/// A contiguous range of the scene's shared index buffer.
struct Primitive {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t materialIndex = -1;

    /// First vertex of this primitive in the shared vertex buffer, and how many. The
    /// index buffer already holds absolute indices, so these exist for the skinning
    /// dispatch -- which works in vertices, not in indices.
    uint32_t baseVertex = 0;
    uint32_t vertexCount = 0;
    /// Where this primitive's influences start in the skin-vertex array, or UINT32_MAX
    /// when it has none. Not a bool plus an offset: one value cannot disagree with
    /// itself.
    uint32_t skinOffset = 0xFFFFFFFFu;

    /**
     * @brief Where this primitive's inverse masses start in the scene's cloth array, or
     *        UINT32_MAX when it is not fabric (C19).
     *
     * The `skinOffset` spelling rather than `morphOffset`'s offset-plus-count, because
     * there is no count here to serve as the flag -- a cloth primitive's vertex count is
     * `vertexCount`, which every primitive already has, so a second one would be a value
     * that could disagree with it.
     *
     * **Per primitive, not per mesh, and that is the granularity the convention needs.**
     * Blender splits a mesh by material, so a curtain wearing two is two primitives; a
     * lower half with no pinned vertex is a separate body that falls, and treating the
     * mesh as one cloth would hide it. `scripts/check_pins.py` checks "at least one vertex
     * pinned" per primitive for exactly this reason, from the other side of the exporter.
     */
    uint32_t clothOffset = 0xFFFFFFFFu;

    /// Where this primitive's morph deltas start in the scene's delta array, and how
    /// many targets it has (S2.1). Zero targets means the offset is never read, so
    /// unlike `skinOffset` the count is the flag and there is nothing to disagree with.
    uint32_t morphOffset = 0;
    uint32_t morphTargets = 0;

    /// ALPHA_MODE BLEND. A load-time property of the material, resolved here once so
    /// that neither the record loop nor the instance table has to reach back into the
    /// material array to ask.
    bool blended = false;

    /// ALPHA_MODE MASK. Resolved beside `blended` and for the same reason, but it is a
    /// *shadow* property rather than a lighting one: the depth-only pass can drop its
    /// fragment shader entirely for everything that is not masked, and it needs to know
    /// which draws those are without reading the material buffer per fragment.
    bool masked = false;

    /// Object-space bounds. Scene bounds must be accumulated from these *after* the
    /// node transform is applied — Sponza's root node scales by 0.008, so raw vertex
    /// bounds are off by more than two orders of magnitude.
    glm::vec3 localMin{0.0f};
    glm::vec3 localMax{0.0f};

    /**
     * @brief The simplified levels this primitive carries, coarsest last (C17).
     *
     * **Inline, and that is the design rather than a size optimisation.** The obvious
     * shape for per-level ranges is a second array indexed by primitive, and this session
     * has already fixed that shape three times -- `PhysicsWorld::snapshot`,
     * `GltfScene::indexData` under the BLAS build, and the joint packing C1 avoided by
     * never repacking. Two arrays laid out to match get out of step the moment one of them
     * grows, and `appendModel` grows `prims` by construction. A chain that travels *inside*
     * the record it describes cannot: it is rebased, serialised, appended and read by the
     * same statement that moves the primitive.
     *
     * `lodCount` is how many entries of `lods` are real -- the array it indexes, so there
     * is no total to be off by one against. **Zero is the default and means no chain**,
     * which is what LOD being optional per mesh comes to: nothing requires one, and every
     * mesh in the tree has none until a bake gives it one.
     *
     * Level 0 is `firstIndex` and `indexCount` above and is deliberately *not* repeated
     * here, because a copy of a value is a second thing that can disagree with it.
     */
    LodRange lods[kMaxLodLevels];
    uint32_t lodCount = 0;
};

/**
 * @brief Geometry a game built rather than loaded (G4).
 *
 * The input to `GltfScene::createMesh`, and deliberately the same `Vertex` and index
 * types the loader produces: procedural geometry lands in the same shared buffers, is
 * drawn by the same pipelines and is freed by the same `unloadModel`. A second buffer
 * for meshes made in code would be a second everything.
 *
 * Bounds are computed from the vertices when `localMax` is not greater than `localMin`,
 * which is what a caller who has not thought about it gets. Supplying them is for a
 * caller who knows better -- a mesh that will be displaced by a vertex shader, say.
 */
struct MeshData {
    std::vector<Vertex> vertices;
    /// Zero-based, into `vertices`. `createMesh` rebases them onto whatever range the
    /// shared buffer hands out, exactly as `appendModel` does for a file's.
    std::vector<uint32_t> indices;
    /// Index into the material table, from `createMaterial` or from the loaded scene.
    /// Out of range falls back to material 0 with a warning rather than reading past it.
    uint32_t material = 0;
    /// Where it is placed. One placement, because a mesh made in code is one thing; a
    /// caller wanting forty makes forty instances, which is what the instance table is.
    glm::mat4 transform{1.0f};
    bool blended = false;
    bool masked = false;
    glm::vec3 localMin{0.0f};
    glm::vec3 localMax{0.0f};

    /**
     * @brief Morph targets, one entry per target (G11).
     *
     * Deltas rather than displaced positions, because that is what a glTF target is, what
     * the loader produces and what `skinning.comp` adds -- a second convention here would
     * mean the two producers of the one delta array disagreed about what they held.
     *
     * **Each target must be exactly `vertices.size()` long.** The shader addresses a
     * displacement as `morphOffset + target * vertexCount + vertex`, so a short target
     * does not read a default: it reads the *next* target's first rows. `createMesh`
     * refuses the whole mesh rather than padding, because a mesh silently missing the
     * last rows of a target is a wrong shape nobody will attribute to this call.
     *
     * A target that displaces only positions leaves normals and tangents zero, which is
     * the identity for a displacement -- exactly as `MorphDelta` says.
     */
    std::vector<std::vector<MorphDelta>> morphTargets;
};

/// One primitive at one world transform, as the glTF node hierarchy placed it.
///
/// This is what the file says, not what the renderer draws. Turning a placement into
/// something drawable is `addSceneInstances()`, and it is a separate explicit call
/// rather than a side effect of load() — see property (ii) in the roadmap's 4.1b.
struct Placement {
    uint32_t primitive = 0;
    glm::mat4 transform{1.0f};
    /// Node that placed it, so an animated hierarchy can push a new transform into the
    /// instance every frame (4.4).
    uint32_t node = 0;
    /// Skin driving it, or UINT32_MAX for rigid geometry.
    uint32_t skin = 0xFFFFFFFFu;
    /**
     * @brief Nearest ancestor node carrying a `substrate_collider`, or UINT32_MAX.
     *
     * `node` is not enough to bind a body to what it moves, and a skinned character is
     * the case that shows why. A collider authored on the node that carries the mesh is
     * matched by `node` alone -- which is every collider in `physics.gltf`. A rig is not
     * shaped like that: its meshes hang several levels below the node an author would put
     * a capsule on, so matching by `node` finds nothing and the controller drives an empty
     * list. It walks, and the character it was supposed to be does not.
     *
     * Inherited down the walk rather than searched for afterwards, because the walk is
     * the only place the hierarchy exists -- `GltfScene` keeps placements and a rig, not
     * a scene graph.
     */
    uint32_t colliderNode = 0xFFFFFFFFu;
};

struct SceneStats {
    /// The three phases inside `parseMs` (C13). It used to be one number covering an
    /// mmap, three whole-document `extras` scans and everything fastgltf does -- base64
    /// decoding, external `.bin` reads and its own structure building -- so nothing could
    /// be attributed to any of them, and C14 and C15 could not be ranked against each
    /// other. They are split here because that is where the attribution has to come from:
    /// a profiler zone measures whichever call it wraps, and these are the three calls.
    double mmapMs = 0.0;
    /// The one rapidjson pass the three `extras` readers now share, and the walk each
    /// makes over its nodes. It was three full passes over the same bytes until C14, and
    /// on an 8000-node scene that was ~57 ms of a ~80 ms load; it is ~19 ms of ~44 ms now.
    double extrasMs = 0.0;
    /// fastgltf's own work: structure building, base64, external buffers. **This is
    /// C15's number** -- what a baked sidecar would replace.
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
    /// Images that came from a `.ktx2` cache entry rather than a PNG decode (4.6a).
    uint32_t compressedTextures = 0;
    uint32_t skins = 0;
    uint32_t animations = 0;
    /// Particle emitters placed by a node's `extras.substrate_emitter` (S3.1).
    uint32_t emitters = 0;
    /// Collision shapes placed by a node's `extras.substrate_collider` (S4.2).
    uint32_t colliders = 0;
    /// Sounds placed by a node's `extras.substrate_audio` (S5.2).
    uint32_t audioSources = 0;
    uint32_t skinnedVertices = 0;
    /// Primitives carrying an LOD chain, and the indices those chains added to the shared
    /// buffer (C17). Both zero for a scene loaded from a document, because chains are a
    /// *bake* product -- so this pair is also how a log line says whether the run that
    /// produced it had any levels to select between at all.
    uint32_t lodPrimitives = 0;
    uint32_t lodIndices = 0;
    /// Morph targets summed over every primitive that has any, and the vertices they
    /// displace -- the two numbers that decide what the delta buffer costs (S2.1).
    uint32_t morphTargets = 0;
    uint32_t morphedVertices = 0;
    /// `FABRIC_` primitives and the vertices they pin (C19). Both zero for every scene in
    /// this repository but `cloth.gltf`, which is what makes this pair the log line that
    /// says whether a run had any fabric in it at all -- the same job `lodPrimitives` does
    /// one field up.
    uint32_t clothPrimitives = 0;
    uint32_t clothVertices = 0;
    double parseMs = 0.0;
    double geometryMs = 0.0;
    /// Time spent reading the C15 sidecar instead of the document. Non-zero exactly when
    /// `parseMs` and `geometryMs` are zero, which is the pair that says which path a load
    /// took -- worth more in a log line than either number alone.
    double cacheMs = 0.0;
    double textureMs = 0.0;
    double totalMs = 0.0;
};

} // namespace scene
