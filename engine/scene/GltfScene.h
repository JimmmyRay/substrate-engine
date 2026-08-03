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
 * The whole scene shares one vertex buffer and one index buffer; primitives are
 * ranges into them. Materials live in a single storage buffer and reference
 * textures by index into one large descriptor array, so the entire scene binds one
 * descriptor set and draws with nothing but push constants between primitives.
 */
class GltfScene {
  public:
    /// Non-copyable; see the note on `gfx::Uploader` in gfx/Resources.h. This one owns the
    /// geometry buffers, every texture and the descriptor pool behind them.
    GltfScene() = default;
    GltfScene(const GltfScene&) = delete;
    GltfScene& operator=(const GltfScene&) = delete;

    /// False means the file could not be parsed -- the one failure here a caller can
    /// usefully react to, since it knows the path it asked for. It reads a C15 sidecar if
    /// one applies and never writes one: since D9 the writer is `substrate-bake` and is
    /// not linked into a game at all.
    ///
    /// `scale` resizes the scene as it arrives -- see `scaleSceneData` for what that does
    /// and does not touch. 1 loads the file as authored.
    [[nodiscard]] bool load(const gfx::VulkanContext& ctx, gfx::Uploader& uploader,
                            const std::filesystem::path& path, float scale = 1.0f);

    /// The device half of a load: everything `scene::loadSceneCpu` (scene/SceneParse.h)
    /// deliberately left out. `data` and `embedded` are consumed.
    [[nodiscard]] bool upload(const gfx::VulkanContext& ctx, gfx::Uploader& uploader,
                              const std::filesystem::path& path, SceneData& data, EmbeddedImages& embedded);

    /**
     * @brief A scene with no document behind it: no geometry, and every GPU resource a
     *        scene owns.
     *
     * A game that names no scene still has a `GltfScene`, and the renderer binds its
     * descriptor set layout into every pipeline layout it builds -- so a default-constructed
     * one, whose layout is `VK_NULL_HANDLE`, is not "a scene with nothing in it" but a scene
     * that does not exist yet. This is the difference, and it is the same code path a file
     * takes: the buffers are the ones `upload` makes, sized by the headroom it always adds,
     * so `createMesh` and `appendModel` work into an empty scene exactly as they work into a
     * loaded one.
     */
    [[nodiscard]] bool createEmpty(const gfx::VulkanContext& ctx, gfx::Uploader& uploader);

    void destroy(const gfx::VulkanContext& ctx);

    // ------------------------------------------------------- multiple models (C10)

    /// Index into the model table. Not a `Handle`, because a model is never reissued into
    /// a reused slot: `unloadModel` marks it dead and the slot stays dead.
    using ModelId = uint32_t;
    static constexpr ModelId kNoModel = 0xFFFFFFFFu;
    /// An image that got no descriptor slot -- see `modelTextureSlot`.
    static constexpr uint32_t kNoTextureSlot = 0xFFFFFFFFu;
    /// "This import brought no skin", for `LoadedModel::skinBase`. Its own name rather than
    /// `SceneAnimator::kNoSkin` borrowed because the two numbers agree — principles.md
    /// rule 8, and the same argument `kNoClip` was split out under.
    static constexpr uint32_t kNoRig = 0xFFFFFFFFu;

    /**
     * @brief Load a second glTF into *these* buffers and return what to instantiate.
     *
     * There is no second `GltfScene`. The renderer binds one vertex buffer, one index
     * buffer and one descriptor set, and that is the design rather than an accident -- so
     * "load another model" means sub-allocating out of the buffers that already exist,
     * which is what `RangeAllocator` is for.
     *
     * What gets fixed up on the way in: every index value is rebased onto the allocated
     * vertex range, every primitive's `firstIndex`, `baseVertex` and `materialIndex` are
     * offset, and every appended material's texture indices are remapped onto the
     * descriptor slots its images actually landed in.
     *
     * @return `kNoModel` when the file will not parse, or when the buffers have no room.
     *         Each is logged with what it would have needed.
     *
     * **A deforming model is appendable since C22**, and what it took is that the three
     * scene-wide arrays a deforming primitive indexes now grow here: `skinData` and
     * `morphData` are extended and every appended `skinOffset` and `morphOffset` is shifted
     * onto the new region, and cloth is lifted into its own `ClothSource` records exactly as
     * `load` lifts the base scene's. The rig is *not* merged here -- see `takeAppendedRig`
     * for why it cannot be.
     */
    /// `transform` places the import in the world (C21) -- see `scene::placeSceneData` for
    /// what it moves. The identity appends at the document's own coordinates, which is what
    /// every caller before C21 wanted and still gets.
    [[nodiscard]] ModelId appendModel(const gfx::VulkanContext& ctx, gfx::Uploader& uploader,
                                      const std::filesystem::path& path,
                                      const glm::mat4& transform = glm::mat4(1.0f));

    /// Give a model's geometry, materials and texture slots back. Its placements stay in
    /// the array as holes -- every surviving placement keeps its index, which is what the
    /// instances that reference them require.
    void unloadModel(const gfx::VulkanContext& ctx, ModelId id);

    /**
     * @brief Geometry a game built, into the same buffers a file's goes into (G4).
     *
     * Returns a `ModelId` rather than a primitive index, and that is the point rather than
     * a convenience: a mesh made in code is freed by `unloadModel` like any other, out of
     * the same allocator, with the same range coalescing. The alternative -- a second
     * buffer for procedural geometry -- is a second upload path, a second free list and a
     * second thing to bind.
     *
     * `data` is consumed. Its indices are rebased onto whatever range the shared buffer
     * hands out, exactly as an appended file's are.
     *
     * @return `kNoModel` for an empty mesh, or when the buffers cannot grow far enough.
     */
    [[nodiscard]] ModelId createMesh(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, MeshData data);

    // --------------------------------------------------------- mutable materials (G4)

    /**
     * @brief Add a material and return its index.
     *
     * The table is dense and shader-indexed, so an index, not a handle: nothing is ever
     * freed out of the middle of it -- see `unloadModel`, which explains why material
     * slots are the one thing it does not reclaim.
     *
     * @return `UINT32_MAX` when the material buffer cannot grow, which is logged with what
     *         it would have needed.
     */
    uint32_t createMaterial(const GpuMaterial& material);

    [[nodiscard]] const GpuMaterial& material(uint32_t index) const;
    /// Rewrite a material. A call rather than a mutable reference, and for the reason
    /// stated everywhere else in this engine: it bumps the revision, and a caller that
    /// wrote through a reference would leave the GPU copy stale with nothing to say so.
    void setMaterial(uint32_t index, const GpuMaterial& material);

    /**
     * @brief Bumped by every material mutation, exactly as `InstanceTable::revision` is.
     *
     * What the renderer compares against per frame-in-flight buffer, so a scene whose
     * materials never move uploads them once and a scene animating one costs a copy rather
     * than a diff.
     */
    [[nodiscard]] uint64_t materialRevision() const { return materialRev; }
    /// The CPU-side table the renderer re-uploads from. Kept rather than dropped after the
    /// first upload, because a mutable table has to be readable to be written back.
    [[nodiscard]] const GpuMaterial* materialData() const { return materialCpu.data(); }
    [[nodiscard]] uint32_t materialTableCount() const { return materialCount; }

    /**
     * @brief Take the rig an append parsed, to merge into whoever owns the animator (C22).
     *
     * Empty for an import with no skin and no clip, and empty on the second call: this
     * moves. A caller that takes it **must** merge it and then call `rebaseAppendedSkins`
     * with what the merge returned, or every skinned placement of the import names a skin
     * of the base scene.
     */
    [[nodiscard]] AnimationRig takeAppendedRig(ModelId id);

    /**
     * @brief Shift an import's placements onto the skins the animator gave them (C22).
     *
     * `Placement::skin` out of a file is file-local, and the animator decides where the
     * import's skins land. Separate from `appendModel` because only the caller that owns
     * the animator knows the answer, and separate from `takeAppendedRig` because the order
     * is take, merge, rebase and a single call could not express it.
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
         * @brief What the import brought besides geometry (C21), as ranges into the scene's
         *        own containers.
         *
         * Recorded rather than recomputed because removal needs them: a model is retired by
         * undoing exactly what its append did, and "the colliders that came from this file"
         * is not a question `sceneColliders` can answer afterwards.
         */
        uint32_t firstCollider = 0, colliderCount = 0;
        uint32_t firstLight = 0, lightCount = 0;
        uint32_t firstEmitter = 0, emitterCount = 0;
        uint32_t firstAudio = 0, audioCount = 0;
        /// The `ClothSource` records this import produced (C22). A range for the same
        /// reason the colliders are one: `unloadModel` has to undo exactly what the append
        /// did, and "the cloth that came from this file" is not a question the flat list
        /// can answer afterwards.
        uint32_t firstCloth = 0, clothCount = 0;
        /// This import's runs in the scene-wide deform arrays (C22). Recorded so
        /// `unloadModel` can give them back **when they are at the tail**, which is the
        /// whole of what it can safely do: a run freed out of the middle would renumber
        /// every later primitive's `skinOffset` under instances that still hold them.
        uint32_t firstSkinVertex = 0, skinVertexCount = 0;
        uint32_t firstMorphDelta = 0, morphDeltaCount = 0;
        /// Where this import's skins landed in the merged rig, and how many it brought.
        /// `Placement::skin` is file-local and is shifted by this. `kNoRig` when the file
        /// carried no skin at all, which is every import that is not a character.
        uint32_t skinBase = kNoRig;
        uint32_t skinCount = 0;
        /**
         * @brief Each placement's transform **as the document wrote it**, before the caller's
         *        placement was applied -- one entry per placement, in placement order.
         *
         * Kept because it is the offset a scene node wants: `Engine::addModel(NodeId, path)`
         * hangs every instance under the node the import landed on, and the sweep writes
         * `node.worldTransform * offset` back over the instance, so the offset has to be what
         * the *file's* own node hierarchy contributed and nothing else. Recovering it by
         * inverting the placement out of the baked transform works and is a round trip
         * through a matrix inverse for a value the loader had in its hand.
         */
        std::vector<glm::mat4> placementLocals;
        /**
         * @brief The rig this import parsed, until somebody takes it.
         *
         * Held here rather than merged into `sceneRig` because `sceneRig` is **moved out**
         * at load: `Engine::loadScene` hands it to `SceneAnimator::init`, which owns it
         * from then on. Merging into the husk left behind would produce a rig the animator
         * never sees. So the import's rig waits here for `takeAppendedRig`, and the caller
         * that owns the animator does the merge.
         */
        AnimationRig importedRig;
        /**
         * @brief What every node index in this import was shifted by.
         *
         * A glTF's node indices are file-local, and three things match on them: a placement
         * finds its body through `Placement::colliderNode`, an audio source finds its body
         * through `AudioSourceDesc::node`, and both are compared against
         * `ColliderDesc::node`. Two files both numbering from zero would have the second
         * import's crate driven by the first import's collider.
         *
         * Shifting the whole import past everything already loaded keeps every match inside
         * the file that authored it, and costs one addition per record at append time.
         */
        uint32_t nodeBase = 0;
        /// One entry per *image in the file*, holding the descriptor slot it landed in or
        /// `kNoTextureSlot`. Indexed by image rather than packed, because that is what
        /// makes it answer `modelTextureSlot` -- an image that failed to decode or found
        /// no free slot leaves a hole, and a packed list would silently shift every image
        /// after it onto the wrong texture.
        std::vector<uint32_t> textureSlotsUsed;
        bool live = false;
    };
    [[nodiscard]] const LoadedModel& model(ModelId id) const { return models[id]; }
    /**
     * @brief Where an appended model's `image` ended up in the bindless array.
     *
     * What a **code-built** emitter needs to name a sheet an import brought with it:
     * `ParticleEmitter::texture` is a slot, and only the import knows which one. An
     * emitter authored *in* the file is remapped for free by `appendModel`.
     *
     * @return `kNoTextureSlot` for an unknown model or an image that got none.
     */
    [[nodiscard]] uint32_t modelTextureSlot(ModelId id, uint32_t image) const {
        if (id >= models.size() || image >= models[id].textureSlotsUsed.size()) return kNoTextureSlot;
        return models[id].textureSlotsUsed[image];
    }
    [[nodiscard]] uint32_t modelCount() const { return static_cast<uint32_t>(models.size()); }
    /// Free elements left in the two geometry buffers, for a caller deciding whether the
    /// next model will fit. See `RangeAllocator::largestFree` for why this is not capacity
    /// minus used.
    [[nodiscard]] uint32_t freeVertices() const { return vertexRanges.largestFree(); }
    [[nodiscard]] uint32_t freeIndices() const { return indexRanges.largestFree(); }

    /// Every primitive the node hierarchy placed, opaque and blended together. The
    /// split by alpha mode lives on `Primitive::blended` now: a deferred G-buffer
    /// stores one surface per pixel and blending needs several, so those draws go
    /// forward after lighting — but that is a flag on the instance, not a second list.
    const std::vector<Placement>& placements() const { return placedPrims; }
    const std::vector<Primitive>& primitives() const { return prims; }
    const SceneStats& stats() const { return sceneStats; }

    /// Lights parsed out of KHR_lights_punctual, already in world space. Empty for a
    /// file that declares none -- Sponza is one, which is why the sample scene still
    /// places lights from the game.
    const std::vector<gfx::GpuLight>& lights() const { return sceneLights; }

    /// Particle emitters the file authored, already placed by their nodes' world
    /// transforms (S3.1). Empty for every scene in this repository but one -- which is
    /// the whole reason S3's gate said "plus a scene that wants them": an emitter is
    /// content, and Sponza has none to read.
    ///
    /// Moved out rather than copied, because `ParticleSystem::setEmitters` takes
    /// ownership and the scene has no further use for them.
    std::vector<ParticleEmitter>& emitters() { return sceneEmitters; }
    const std::vector<ParticleEmitter>& emitters() const { return sceneEmitters; }

    /// Colliders the file authored, placed by their nodes' world transforms and, where
    /// the shape is a hull or a triangle mesh, already carrying the node's geometry in
    /// node space (S4.2). Empty for every scene in this repository but one -- a collider
    /// is content in exactly the way an emitter is.
    ///
    /// The geometry is copied rather than referenced because the vertex and index arrays
    /// the loader built are uploaded and dropped, and a collider outlives them. Only
    /// collider nodes pay: Sponza's 150k vertices are never touched by this.
    std::vector<ColliderDesc>& colliders() { return sceneColliders; }
    const std::vector<ColliderDesc>& colliders() const { return sceneColliders; }

    /// Sounds the file authored, placed by their nodes' world transforms and with their
    /// `file` already resolved against the scene's own directory (S5.2). Empty for every
    /// scene in this repository but one -- a sound is content in exactly the way an
    /// emitter and a collider are.
    ///
    /// The path is resolved here rather than in the mixer because this is the only place
    /// that knows where the .gltf came from, and a source authored as `"file": "hum.wav"`
    /// means the one next to the scene.
    std::vector<AudioSourceDesc>& audioSources() { return sceneAudio; }
    const std::vector<AudioSourceDesc>& audioSources() const { return sceneAudio; }

    // ------------------------------------------------------- animation (4.4, S2.1)
    /// The retained node hierarchy, skins, clips and default morph weights, in the
    /// form `SceneAnimator::init` wants. Handed over once, by move.
    AnimationRig& rig() { return sceneRig; }
    const AnimationRig& rig() const { return sceneRig; }

    /// Per-vertex joint indices and weights for skinned primitives only. Empty for a
    /// file that declares no skins, which is every scene in this repository but one.
    const std::vector<SkinVertex>& skinVertices() const { return skinData; }

    /// Per-target, per-vertex displacements for morphed primitives only, target-major.
    /// Empty for a file that declares no morph targets (S2.1).
    const std::vector<MorphDelta>& morphDeltas() const { return morphData; }

    /**
     * @brief One `FABRIC_` primitive's rest pose, complete and self-contained (C19).
     *
     * **This is the answer to how a per-vertex array added to `Primitive` avoids becoming
     * `indexData()`.** G11 found that copy stale because it was a snapshot of one array
     * indexed by an offset in another, and `createMesh` grew one of them; C17's LOD chains
     * answered the same hazard by travelling *inside* the record they describe. This is
     * that answer for per-vertex data, which cannot literally live inline: the flat
     * `SceneData::clothVertices` array and `Primitive::clothOffset` are the **file
     * format** -- one more `podVector` in the sidecar, like every other POD run -- and they
     * are read exactly once, here, in the statement that builds these records. Nothing
     * reads them again. From that point a cloth carries its own vertices, its own indices
     * and its own inverse masses, so there is no pair of arrays left to get out of step and
     * no offset left to be wrong.
     *
     * The rest pose is kept because the GPU vertex buffer is not readable and a soft body
     * needs somewhere to start; it is the vertices of the fabric only, so a scene with no
     * cloth holds nothing at all.
     */
    struct ClothSource {
        /// Index into `primitives()`, for the log line and for matching an instance.
        uint32_t primitive = 0;
        /// Rest pose in the primitive's object space. The placement's transform is applied
        /// when the soft body is built, not here.
        std::vector<Vertex> vertices;
        /// **Zero-based** into `vertices`, rebased off the scene's absolute indices, so
        /// this record needs nothing outside itself to be read.
        std::vector<uint32_t> indices;
        /// One per vertex. Never shorter, because it is sized from `vertexCount`.
        std::vector<ClothVertex> masses;
    };

    /// Every `FABRIC_` primitive the file declared. Empty for every scene in this
    /// repository that does not author cloth, which is all of them but `cloth.gltf`.
    const std::vector<ClothSource>& clothSources() const { return clothSourceList; }

    /// The scene's indices, retained on the CPU **only** when something in the scene
    /// deforms (S2.5). A dynamic BLAS is built over the deformed vertex buffer and
    /// needs its indices rebased onto that buffer's ranges; the draw path gets the same
    /// rebasing for free from an indirect command's signed `vertexOffset`, and an
    /// acceleration-structure build has no signed equivalent. Empty for Sponza, which
    /// is the case where holding 3 MB of indices for nobody would be the cost.
    ///
    /// **The invariant is positional, not "these are the indices".** Element `i` is
    /// element `i` of the device index buffer wherever it is written at all, so a reader
    /// may index it by a primitive's `firstIndex` -- and the gaps a partly-filled copy
    /// leaves are ranges no deformed primitive names. `createMesh` extends it for a mesh
    /// carrying morph targets and nothing else does, because the dynamic tier is the only
    /// reader and a deformed primitive is the only thing it asks about.
    const std::vector<uint32_t>& indexData() const { return indexCopy; }

    VkBuffer vertexBuffer() const { return vertices.buffer; }
    VkBuffer indexBuffer() const { return indices.buffer; }
    VkBuffer materialBuffer() const { return materials.buffer; }
    /// One byte per material: does it emit? Kept on the CPU when the rest of the
    /// material table is uploaded and dropped, because the acceleration-structure build
    /// needs it to decide which geometry may occlude a shadow ray. See
    /// buildSceneAccelStruct.
    const std::vector<uint8_t>& emissiveMaterials() const { return materialEmissive; }
    /// Images the scene actually loaded. Not the size of the bindless array, which is
    /// larger by the fallback plus the free slots -- see textureCapacity().
    uint32_t textureCount() const { return sceneStats.textures; }
    /// Vertices in the shared buffer. The acceleration-structure build needs it for
    /// `maxVertex`, which bounds what the builder will read.
    uint32_t vertexCount() const { return static_cast<uint32_t>(sceneStats.vertexCount); }

    // ----------------------------------------------------- texture slots (4.6b)
    // The four properties the roadmap's residency delegation names, made checkable:
    //
    //   (i)   textures are indexed bindlessly, so swapping a slot's contents touches no
    //         pipeline -- the one descriptor array below, unchanged since v0.
    //   (ii)  slots are stable and recycled through a free list -- acquire/release here.
    //   (iii) uploads run on a transfer queue with a fence -- `Uploader::endBatchAsync`.
    //   (iv)  an unresident slot samples a defined image -- `fallbackTextureSlot()`.
    //
    // What is *not* here, deliberately, is a policy: what to evict, when to prefetch and
    // how much to keep resident are a game's decisions about its own content, and the
    // engine that guessed at them would be wrong for every game that disagreed.

    /// Slots the bindless array can address, including the fallback. A stated budget:
    /// acquireTextureSlot() reports exhaustion rather than overrunning it.
    uint32_t textureCapacity() const { return textureSlots; }

    /// Take a free slot, or UINT32_MAX when there are none left -- logged, once per
    /// call, because a residency system that quietly stops streaming looks exactly like
    /// one that has nothing left to stream.
    uint32_t acquireTextureSlot();

    /// Hand a slot back. Destroys whatever image it held and repoints its descriptor at
    /// the fallback *before* returning it to the free list, so there is no window in
    /// which a shader could sample a destroyed view.
    void releaseTextureSlot(const gfx::VulkanContext& ctx, uint32_t slot);

    /// Put `image` in `slot` and update its descriptor. Takes ownership: the scene
    /// destroys it on shutdown or on the next release.
    void bindTexture(const gfx::VulkanContext& ctx, uint32_t slot, const gfx::GpuImage& image);

    /// The reserved slot every free one points at. 1x1 opaque white, which is the
    /// neutral value through every material slot the shaders sample.
    uint32_t fallbackTextureSlot() const { return fallbackSlot; }

    VkDescriptorSetLayout descriptorSetLayout() const { return setLayout; }
    VkDescriptorSet descriptorSet() const { return set; }

    /// What `setLayout` was created from. Vulkan offers no way to read a layout back,
    /// and the Debug reflection check in Renderer needs something to compare a
    /// reflected binding against -- including this set, which is the only one carrying
    /// a variable-count descriptor array.
    const std::vector<VkDescriptorSetLayoutBinding>& descriptorBindings() const { return setBindings; }

    /// Axis-aligned bounds of the whole scene, for framing the camera. **In the scale the
    /// scene was loaded at**, which is what `sceneScale` below is for.
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    /// Whether anything has contributed to the two above. **A flag rather than an inverted
    /// sentinel pair**: they default to `{0,0,0}`, so "empty" and "a point at the origin" are
    /// the same reading, and every caller reads the vectors directly. `appendModel` needs to
    /// tell the two apart to know whether to seed or to union (C41).
    bool boundsSet = false;

    /**
     * @brief What `load` resized the scene by, and 1 for a scene loaded as authored.
     *
     * Read by a game that derives placements from the bounds above, because a fraction of
     * the extent scales with the building and an absolute offset in metres does not: a
     * crate is 0.62 m in a cathedral of any size, but "the floor is one metre above the
     * bounding box" is a measurement *of* the cathedral and scales with it. Getting that
     * distinction wrong puts the props a metre into the ground at 2x.
     */
    float sceneScale = 1.0f;

  private:
    void buildDescriptors(const gfx::VulkanContext& ctx, gfx::Uploader& uploader);

    /// The pool, layout and set alone -- everything in `buildDescriptors` that can be
    /// re-created against a different `textureSlots`. The sampler and the fallback image
    /// cannot: they already exist and the slots below index them.
    void createSceneDescriptors(const gfx::VulkanContext& ctx);

    /// Make room for `atLeast` more textures, growing the bindless array if the free list
    /// is short. **Replaces `setLayout` and `set`**, so a caller has to rebuild anything
    /// holding either -- `Engine::addModel` does it through `Renderer::setScene`.
    bool reserveTextureSlots(const gfx::VulkanContext& ctx, uint32_t atLeast);

    /**
     * @brief Replace one of the three buffers with a bigger one, keeping what it holds.
     *
     * One helper for three callers rather than three near-identical bodies, which is what
     * the Rule of Threes asks for at exactly this point: the vertex, index and material
     * buffers differ in their usage flags, their stride and their name, and in nothing
     * else. `Uploader::copyBuffer` is the primitive, and its own comment already says this
     * is what it is for.
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
    /// report that they cannot. Doubling, floored at what is actually needed: growth that
    /// only just fits is growth that happens again on the next append.
    bool reserveGeometry(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, uint32_t needVertices,
                         uint32_t needIndices);
    bool reserveMaterials(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, uint32_t need);
    /// Point the scene set's binding 0 at the material buffer. Its own function because
    /// growth replaces the buffer, and `buildDescriptors` cannot be re-run -- it creates
    /// the sampler and the fallback image, which already exist.
    void writeMaterialDescriptor(const gfx::VulkanContext& ctx);
    /// Point slot `slot`'s descriptor at `view`. The one place a bindless slot changes.
    void writeTextureDescriptor(const gfx::VulkanContext& ctx, uint32_t slot, VkImageView view);

    /// Free slots beyond what the scene loaded. Sixty-four is a stated budget rather
    /// than a guess at a working set: it is enough for a residency system to have a
    /// meaningful number of transfers in flight, and small enough that the descriptor
    /// array does not grow by a page for a scene that streams nothing.
    static constexpr uint32_t kTextureSlotHeadroom = 64;
    uint32_t textureSlots = 0;
    uint32_t fallbackSlot = 0;
    std::vector<uint32_t> freeTextureSlots;

    /// Sub-allocation over the two geometry buffers (C10). The scene's own load is the
    /// first allocation; `appendModel` takes the rest and `unloadModel` gives it back.
    core::RangeAllocator vertexRanges;
    core::RangeAllocator indexRanges;
    uint32_t vertexCapacity = 0;
    uint32_t indexCapacity = 0;
    uint32_t materialCapacity = 0;
    uint32_t materialCount = 0;
    std::vector<LoadedModel> models;
    /// Where the next import's node indices start. Seeded from the base scene's node count
    /// and advanced by every append, so no two loaded documents ever share one -- see
    /// `LoadedModel::nodeBase`. Never reclaimed on unload: reusing a base would let a stale
    /// match land on a live record, and the counter is 32 bits against a few nodes a load.
    uint32_t nextNodeBase = 0;

    gfx::GpuBuffer vertices;
    gfx::GpuBuffer indices;
    gfx::GpuBuffer materials;
    /// The material table as the CPU holds it. G4 made it mutable, and a table that can
    /// be written has to be readable -- so this is kept rather than dropped after the
    /// upload, which is 96 bytes per material and the only copy either side of the bus.
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
 * A free function taking both, rather than a method on either: the scene has no
 * business knowing what a table is, and the table has no business parsing glTF. It is
 * also the whole of property (ii) in 4.1b — instance creation is an explicit call a
 * caller makes, so a game that wants to place the same mesh forty times, or none at
 * all, is not fighting load().
 *
 * Every instance created here is static: nothing in a glTF scene moves until 4.4
 * animates it, and `kInstanceDynamic` is what selects the more expensive path in 3.4
 * and 3.9. A caller that knows better says so afterwards with `setFlags`.
 */
void addSceneInstances(const GltfScene& scene, InstanceTable& table);

/**
 * @brief The same, over one range of placements (C10).
 *
 * What instantiating an appended model needs: `appendModel` reports where its placements
 * landed, and this turns exactly those into instances. `created` collects the handles when
 * it is not null, which is what a caller needs to be able to destroy them again.
 */
void addPlacementInstances(const GltfScene& scene, InstanceTable& table, uint32_t firstPlacement, uint32_t count,
                           std::vector<InstanceId>* created = nullptr);

} // namespace scene
