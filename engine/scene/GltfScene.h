#pragma once

#include "core/RangeAllocator.h"
#include "gfx/Light.h"
#include "gfx/Resources.h"
#include "scene/Animation.h"
#include "scene/AudioSource.h"
#include "scene/Collider.h"
#include "scene/InstanceTable.h"
#include "core/RangeAllocator.h"
#include "scene/SceneData.h"
#include "scene/ParticleSystem.h"
#include "scene/SceneTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gfx {
struct VulkanContext;
class Uploader;
} // namespace gfx

namespace scene {


/**
 * @brief A loaded glTF scene, resident on the GPU.
 *
 * The whole scene shares one vertex buffer, one index buffer, one material buffer and one
 * bindless texture array: more geometry means sub-allocating out of those, not a second
 * scene, and a draw between primitives costs push constants and nothing else.
 */
class GltfScene {
  public:
    GltfScene() = default;
    GltfScene(const GltfScene&) = delete;
    GltfScene& operator=(const GltfScene&) = delete;

    /// Reads a cache sidecar when one is current, and never writes one: the writer is
    /// `substrate-bake`, which is not linked into a game.
    ///
    /// `scale` resizes the scene as it arrives -- `scaleSceneData` is what it reaches.
    [[nodiscard]] bool load(const gfx::VulkanContext& ctx, gfx::Uploader& uploader,
                            const std::filesystem::path& path, float scale = 1.0f);

    /// The device half of a load; pairs with `scene::loadSceneCpu` (scene/SceneParse.h).
    /// `data` and `embedded` are consumed.
    [[nodiscard]] bool upload(const gfx::VulkanContext& ctx, gfx::Uploader& uploader,
                              const std::filesystem::path& path, SceneData& data, EmbeddedImages& embedded);

    /**
     * @brief A scene with no document behind it: no geometry, and every GPU resource a
     *        scene owns.
     *
     * The renderer binds the scene's descriptor set layout into every pipeline layout it
     * builds, so a default-constructed `GltfScene` -- layout `VK_NULL_HANDLE` -- is not an
     * empty scene but an unusable one. The buffers this makes carry `upload`'s headroom, so
     * `createMesh` and `appendModel` work into it.
     */
    [[nodiscard]] bool createEmpty(const gfx::VulkanContext& ctx, gfx::Uploader& uploader);

    void destroy(const gfx::VulkanContext& ctx);

    /// Index into the model table. Never reissued: `unloadModel` marks the slot dead and it
    /// stays dead, so a held id can go stale but can never name a different model.
    using ModelId = uint32_t;
    static constexpr ModelId kNoModel = 0xFFFFFFFFu;
    /// An image that got no descriptor slot -- see `modelTextureSlot`.
    static constexpr uint32_t kNoTextureSlot = 0xFFFFFFFFu;
    /// "This import brought no skin", for `LoadedModel::skinBase`.
    static constexpr uint32_t kNoRig = 0xFFFFFFFFu;

    /**
     * @brief Load a second glTF into *these* buffers and return what to instantiate.
     *
     * An import sub-allocates out of the buffers that already exist, so everything file-local
     * in it is rebased on the way in: index values, `firstIndex`, `baseVertex`,
     * `materialIndex`, a material's texture indices, `skinOffset` and `morphOffset`. Anything
     * added here that carries a file-local index needs the same treatment, or it reads the
     * base scene's data instead of its own.
     *
     * The rig is the exception -- it cannot be merged here; see `takeAppendedRig`.
     *
     * `transform` places the import in the world (`scene::placeSceneData`); the identity
     * appends at the document's own coordinates.
     *
     * @return `kNoModel` when the file will not parse, or when the buffers have no room.
     */
    [[nodiscard]] ModelId appendModel(const gfx::VulkanContext& ctx, gfx::Uploader& uploader,
                                      const std::filesystem::path& path,
                                      const glm::mat4& transform = glm::mat4(1.0f));

    /// Give a model's geometry, materials and texture slots back. Its placements stay in the
    /// array as holes: compacting them would renumber the indices live instances hold.
    void unloadModel(const gfx::VulkanContext& ctx, ModelId id);

    /**
     * @brief Geometry a game built, into the same buffers a file's goes into.
     *
     * `data` is consumed. Its indices are rebased onto whatever range the shared buffer
     * hands out, exactly as an appended file's are, and `unloadModel` frees it the same way.
     *
     * @return `kNoModel` for an empty mesh, or when the buffers cannot grow far enough.
     */
    [[nodiscard]] ModelId createMesh(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, MeshData data);

    /**
     * @brief Add a material and return its index.
     *
     * The table is dense and shader-indexed, so nothing is ever freed out of the middle of
     * it -- `unloadModel` reclaims everything else an import brought but not these slots,
     * because a hole here renumbers every material a live instance names.
     *
     * @return `UINT32_MAX` when the material buffer cannot grow.
     */
    uint32_t createMaterial(const GpuMaterial& material);

    [[nodiscard]] const GpuMaterial& material(uint32_t index) const;
    /// Rewrite a material. Bumps `materialRevision`, which is what makes the GPU copy
    /// follow: a write that reaches the table any other way leaves it stale with nothing
    /// to say so.
    void setMaterial(uint32_t index, const GpuMaterial& material);

    /// Bumped by every material mutation; the renderer re-uploads a frame's buffer when it
    /// differs from what that buffer last saw.
    [[nodiscard]] uint64_t materialRevision() const { return materialRev; }
    /// The CPU-side table the renderer re-uploads from.
    [[nodiscard]] const GpuMaterial* materialData() const { return materialCpu.data(); }
    [[nodiscard]] uint32_t materialTableCount() const { return materialCount; }

    /**
     * @brief Take the rig an append parsed, to merge into whoever owns the animator.
     *
     * This moves, so the second call yields nothing. A caller that takes it **must** merge
     * it and then call `rebaseAppendedSkins` with what the merge returned, or every skinned
     * placement of the import names a skin of the base scene.
     */
    [[nodiscard]] AnimationRig takeAppendedRig(ModelId id);

    /**
     * @brief Shift an import's placements onto the skins the animator gave them.
     *
     * `Placement::skin` out of a file is file-local, and only the caller that owns the
     * animator knows where the import's skins landed. The order is take, merge, rebase.
     */
    void rebaseAppendedSkins(ModelId id, uint32_t skinBase);

    /// What `appendModel` added, for a caller that has to instantiate it.
    struct LoadedModel {
        uint32_t firstVertex = 0, vertexCount = 0;
        uint32_t firstIndex = 0, indexCount = 0;
        uint32_t firstPrimitive = 0, primitiveCount = 0;
        uint32_t firstPlacement = 0, placementCount = 0;
        uint32_t firstMaterial = 0, materialCount = 0;
        /**
         * @brief What the import brought besides geometry, as ranges into the scene's
         *        own containers.
         *
         * `unloadModel` retires a model by undoing exactly what its append did, and the flat
         * lists cannot answer "which of these came from that file" once the append is over.
         * A range dropped from here is content that leaks for the life of the scene.
         */
        uint32_t firstCollider = 0, colliderCount = 0;
        uint32_t firstLight = 0, lightCount = 0;
        uint32_t firstEmitter = 0, emitterCount = 0;
        uint32_t firstAudio = 0, audioCount = 0;
        uint32_t firstCloth = 0, clothCount = 0;
        /// This import's runs in the scene-wide deform arrays. `unloadModel` gives them back
        /// **only when they are at the tail**: a run freed out of the middle renumbers every
        /// later primitive's `skinOffset` under instances that still hold them.
        uint32_t firstSkinVertex = 0, skinVertexCount = 0;
        uint32_t firstMorphDelta = 0, morphDeltaCount = 0;
        /// Where this import's skins landed in the merged rig, and how many it brought.
        /// `Placement::skin` is file-local and is shifted by this. `kNoRig` when the file
        /// carried no skin at all.
        uint32_t skinBase = kNoRig;
        uint32_t skinCount = 0;
        /**
         * @brief Each placement's transform **as the document wrote it**, before the caller's
         *        placement was applied -- one entry per placement, in placement order.
         *
         * `Engine::addModel(NodeId, path)` hangs every instance under a scene node and the
         * sweep writes `node.worldTransform * offset` back over it, so this must carry what
         * the *file's* hierarchy contributed and nothing the caller added.
         */
        std::vector<glm::mat4> placementLocals;
        /**
         * @brief The rig this import parsed, until `takeAppendedRig` moves it out.
         *
         * `sceneRig` is moved out at load -- `Engine::loadScene` hands it to
         * `SceneAnimator::init`, which owns it from then on -- so merging an import into
         * `sceneRig` writes into a husk the animator never reads.
         */
        AnimationRig importedRig;
        /**
         * @brief What every node index in this import was shifted by.
         *
         * `Placement::colliderNode` and `AudioSourceDesc::node` are matched against
         * `ColliderDesc::node`, and a glTF's node indices are file-local. Two files both
         * numbering from zero put the second import's crate on the first import's collider.
         */
        uint32_t nodeBase = 0;
        /// One entry per *image in the file*, holding the descriptor slot it landed in or
        /// `kNoTextureSlot`. Indexed by image, never packed: an image that failed to decode
        /// or found no free slot leaves a hole, and closing it shifts every later image onto
        /// the wrong texture.
        std::vector<uint32_t> textureSlotsUsed;
        bool live = false;
    };
    [[nodiscard]] const LoadedModel& model(ModelId id) const { return models[id]; }
    /**
     * @brief Where an appended model's `image` ended up in the bindless array.
     *
     * @return `kNoTextureSlot` for an unknown model or an image that got none.
     */
    [[nodiscard]] uint32_t modelTextureSlot(ModelId id, uint32_t image) const {
        if (id >= models.size() || image >= models[id].textureSlotsUsed.size()) return kNoTextureSlot;
        return models[id].textureSlotsUsed[image];
    }
    [[nodiscard]] uint32_t modelCount() const { return static_cast<uint32_t>(models.size()); }
    /// Largest contiguous run left in each geometry buffer -- not capacity minus used, since
    /// an allocation has to be contiguous and unloads fragment the space.
    [[nodiscard]] uint32_t freeVertices() const { return vertexRanges.largestFree(); }
    [[nodiscard]] uint32_t freeIndices() const { return indexRanges.largestFree(); }

    /// Every primitive the node hierarchy placed, opaque and blended in one list;
    /// `Primitive::blended` is the split.
    const std::vector<Placement>& placements() const { return placedPrims; }
    const std::vector<Primitive>& primitives() const { return prims; }
    const SceneStats& stats() const { return sceneStats; }

    /// Lights parsed out of KHR_lights_punctual, already in world space.
    const std::vector<gfx::GpuLight>& lights() const { return sceneLights; }

    /// Particle emitters the file authored, already placed by their nodes' world transforms.
    /// `ParticleSystem::setEmitters` takes ownership by move.
    std::vector<ParticleEmitter>& emitters() { return sceneEmitters; }
    const std::vector<ParticleEmitter>& emitters() const { return sceneEmitters; }

    /// Colliders the file authored, placed by their nodes' world transforms and, where
    /// the shape is a hull or a triangle mesh, carrying a *copy* of the node's geometry in
    /// node space: the loader's vertex and index arrays are uploaded and dropped, and a
    /// collider outlives them.
    std::vector<ColliderDesc>& colliders() { return sceneColliders; }
    const std::vector<ColliderDesc>& colliders() const { return sceneColliders; }

    /// Sounds the file authored, placed by their nodes' world transforms and with `file`
    /// already resolved against the scene's own directory -- `"file": "hum.wav"` means the
    /// one next to the .gltf, and nothing downstream knows where that was.
    std::vector<AudioSourceDesc>& audioSources() { return sceneAudio; }
    const std::vector<AudioSourceDesc>& audioSources() const { return sceneAudio; }

    /// The retained node hierarchy, skins, clips and default morph weights, in the
    /// form `SceneAnimator::init` wants. Handed over once, by move.
    AnimationRig& rig() { return sceneRig; }
    const AnimationRig& rig() const { return sceneRig; }

    /// Per-vertex joint indices and weights for skinned primitives only.
    const std::vector<SkinVertex>& skinVertices() const { return skinData; }

    /// Per-target, per-vertex displacements for morphed primitives only, target-major.
    const std::vector<MorphDelta>& morphDeltas() const { return morphData; }

    /**
     * @brief One `FABRIC_` primitive's rest pose, complete and self-contained.
     *
     * `SceneData::clothVertices` and `Primitive::clothOffset` are the file format and are
     * read exactly once, in the statement that builds these records; a reader that goes back
     * to them instead is reading an offset a later `createMesh` may have invalidated. The
     * rest pose is held on the CPU because the GPU vertex buffer is not readable and a soft
     * body needs somewhere to start.
     */
    struct ClothSource {
        /// Index into `primitives()`.
        uint32_t primitive = 0;
        /// Rest pose in the primitive's object space; the placement's transform is applied
        /// when the soft body is built, not here.
        std::vector<Vertex> vertices;
        /// **Zero-based** into `vertices`, rebased off the scene's absolute indices, so this
        /// record needs nothing outside itself to be read.
        std::vector<uint32_t> indices;
        /// One per vertex.
        std::vector<ClothVertex> masses;
    };

    /// Every `FABRIC_` primitive the file declared.
    const std::vector<ClothSource>& clothSources() const { return clothSourceList; }

    /// The scene's indices, retained on the CPU **only** where something deforms: a dynamic
    /// BLAS build needs them rebased onto the deformed vertex buffer's ranges, and unlike an
    /// indirect draw's signed `vertexOffset` a build has no way to do that itself.
    ///
    /// **The invariant is positional.** Element `i` is element `i` of the device index
    /// buffer wherever this is written at all, so a reader indexes it by a primitive's
    /// `firstIndex`; the gaps are ranges no deformed primitive names. Packing it, or
    /// filling it for primitives that do not deform, breaks one of those two properties.
    const std::vector<uint32_t>& indexData() const { return indexCopy; }

    VkBuffer vertexBuffer() const { return vertices.buffer; }
    VkBuffer indexBuffer() const { return indices.buffer; }
    VkBuffer materialBuffer() const { return materials.buffer; }
    /// One byte per material: does it emit? Kept on the CPU when the rest of the material
    /// table is uploaded and dropped, because `buildSceneAccelStruct` decides from it which
    /// geometry may occlude a shadow ray.
    const std::vector<uint8_t>& emissiveMaterials() const { return materialEmissive; }
    /// Images the scene actually loaded -- smaller than the bindless array by the fallback
    /// plus the free slots. `textureCapacity()` is what bounds a slot index.
    uint32_t textureCount() const { return sceneStats.textures; }
    /// Vertices in the shared buffer; the acceleration-structure build's `maxVertex`, which
    /// bounds what the builder will read.
    uint32_t vertexCount() const { return static_cast<uint32_t>(sceneStats.vertexCount); }

    /// Slots the bindless array can address, including the fallback.
    uint32_t textureCapacity() const { return textureSlots; }

    /// Take a free slot, or UINT32_MAX when there are none left.
    uint32_t acquireTextureSlot();

    /// Hand a slot back. Repoints its descriptor at the fallback *before* destroying the
    /// image and freeing the slot, so no window exists in which a shader samples a
    /// destroyed view.
    void releaseTextureSlot(const gfx::VulkanContext& ctx, uint32_t slot);

    /// Put `image` in `slot` and update its descriptor. Takes ownership: the scene
    /// destroys it on shutdown or on the next release.
    void bindTexture(const gfx::VulkanContext& ctx, uint32_t slot, const gfx::GpuImage& image);

    /// The reserved slot every free one points at. 1x1 opaque white -- the neutral value
    /// through every material slot the shaders sample, so an unbound slot shades correctly
    /// rather than black.
    uint32_t fallbackTextureSlot() const { return fallbackSlot; }

    VkDescriptorSetLayout descriptorSetLayout() const { return setLayout; }
    VkDescriptorSet descriptorSet() const { return set; }

    /// What `setLayout` was created from, kept because Vulkan offers no way to read a layout
    /// back and the Debug reflection check in Renderer has to compare against something.
    const std::vector<VkDescriptorSetLayoutBinding>& descriptorBindings() const { return setBindings; }

    /// Axis-aligned bounds of the whole scene, for framing the camera. **In the scale the
    /// scene was loaded at** -- `sceneScale` below is the factor.
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    /// Whether anything has contributed to the two above. They default to `{0,0,0}`, where
    /// "empty" and "a point at the origin" read the same, and `appendModel` has to tell them
    /// apart to know whether to seed the bounds or union into them.
    bool boundsSet = false;

    /**
     * @brief What `load` resized the scene by, and 1 for a scene loaded as authored.
     *
     * A game placing props off the bounds above must scale absolute offsets in metres by
     * this and leave fractions of the extent alone: a crate is 0.62 m in a cathedral of any
     * size, but "one metre above the bounding box" is a measurement *of* the cathedral.
     * Confusing the two puts the props a metre into the ground at 2x.
     */
    float sceneScale = 1.0f;

  private:
    void buildDescriptors(const gfx::VulkanContext& ctx, gfx::Uploader& uploader);

    /// The pool, layout and set alone -- everything in `buildDescriptors` that survives being
    /// re-created against a different `textureSlots`. Re-running `buildDescriptors` instead
    /// would remake the sampler and the fallback image, which already exist and are indexed
    /// by the slots below.
    void createSceneDescriptors(const gfx::VulkanContext& ctx);

    /// Make room for `atLeast` more textures, growing the bindless array if the free list
    /// is short. **Replaces `setLayout` and `set`**, so a caller has to rebuild anything
    /// holding either -- `Engine::addModel` does it through `Renderer::setScene`.
    bool reserveTextureSlots(const gfx::VulkanContext& ctx, uint32_t atLeast);

    /**
     * @brief Replace one of the three buffers with a bigger one, keeping what it holds.
     *
     * **The caller is responsible for the device being idle.** Every path into this runs
     * under the `vkDeviceWaitIdle` in `Engine::addModel`, which is there because the
     * command buffers still in flight name the buffer about to be destroyed.
     *
     * @return false when the allocation fails, in which case the old buffer is untouched
     *         and still holds everything it held.
     */
    bool growBuffer(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, gfx::GpuBuffer& buffer,
                    VkBufferUsageFlags usage, uint32_t oldCapacity, uint32_t newCapacity, VkDeviceSize stride,
                    const char* name);
    /// Grow the vertex and index buffers until `needVertices` and `needIndices` fit, or
    /// report that they cannot. Doubles rather than fitting exactly, so a run of appends
    /// does not pay a full copy each time.
    bool reserveGeometry(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, uint32_t needVertices,
                         uint32_t needIndices);
    bool reserveMaterials(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, uint32_t need);
    /// Point the scene set's binding 0 at the material buffer -- growth replaces the buffer
    /// and leaves the descriptor naming the old one.
    void writeMaterialDescriptor(const gfx::VulkanContext& ctx);
    /// Point slot `slot`'s descriptor at `view`. The one place a bindless slot changes.
    void writeTextureDescriptor(const gfx::VulkanContext& ctx, uint32_t slot, VkImageView view);

    /// Free slots beyond what the scene loaded: enough for a residency system to keep
    /// transfers in flight, small enough that a scene streaming nothing does not grow the
    /// descriptor array by a page.
    static constexpr uint32_t kTextureSlotHeadroom = 64;
    uint32_t textureSlots = 0;
    uint32_t fallbackSlot = 0;
    std::vector<uint32_t> freeTextureSlots;

    /// Sub-allocation over the two geometry buffers.
    core::RangeAllocator vertexRanges;
    core::RangeAllocator indexRanges;
    uint32_t vertexCapacity = 0;
    uint32_t indexCapacity = 0;
    uint32_t materialCapacity = 0;
    uint32_t materialCount = 0;
    std::vector<LoadedModel> models;
    /// Where the next import's node indices start -- see `LoadedModel::nodeBase`. Never
    /// reclaimed on unload: a reused base lets a stale node match land on a live record, and
    /// 32 bits against a few nodes per load is not a range worth recovering.
    uint32_t nextNodeBase = 0;

    gfx::GpuBuffer vertices;
    gfx::GpuBuffer indices;
    gfx::GpuBuffer materials;
    /// The material table as the CPU holds it, and the only readable copy either side of the
    /// bus.
    std::vector<GpuMaterial> materialCpu;
    uint64_t materialRev = 1;
    std::vector<uint8_t> materialEmissive;

    std::vector<gfx::GpuImage> textures;
    VkSampler sampler = VK_NULL_HANDLE;

    std::vector<gfx::GpuLight> sceneLights;
    std::vector<ParticleEmitter> sceneEmitters;
    std::vector<ColliderDesc> sceneColliders;
    std::vector<AudioSourceDesc> sceneAudio;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayoutBinding> setBindings;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;

    std::vector<Primitive> prims;
    std::vector<Placement> placedPrims;

    AnimationRig sceneRig;
    std::vector<SkinVertex> skinData;
    std::vector<MorphDelta> morphData;
    std::vector<ClothSource> clothSourceList;
    std::vector<uint32_t> indexCopy;

    SceneStats sceneStats;
};

/**
 * @brief Create one instance per placement the scene loaded.
 *
 * Every instance created here is static -- `kInstanceDynamic` selects a more expensive
 * path, so a caller that knows better says so afterwards with `setFlags`.
 */
void addSceneInstances(const GltfScene& scene, InstanceTable& table);

/**
 * @brief The same, over one range of placements.
 *
 * `created` collects the handles when it is not null; without them there is no way to
 * destroy the instances again.
 */
void addPlacementInstances(const GltfScene& scene, InstanceTable& table, uint32_t firstPlacement, uint32_t count,
                           std::vector<InstanceId>* created = nullptr);

} // namespace scene
