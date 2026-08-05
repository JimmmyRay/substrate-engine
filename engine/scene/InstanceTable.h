#pragma once

#include "core/Handle.h"
#include "scene/Node.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace scene {

/**
 * @file InstanceTable.h
 * @brief The scene's renderable objects, as flat arrays the GPU can read.
 *
 * **Slot stability is the load-bearing property.** Motion vectors, the TLAS, GPU culling
 * and residency all key off a slot index, so a slot must not move while an object lives
 * -- which rules out swap-and-pop compaction. Destroying an instance leaves a hole the
 * free list hands back to the next create(), and the arrays are only ever appended to.
 *
 * The arrays are split by *consumer*, not by field: `shading` is what the vertex shaders
 * index by `gl_InstanceIndex`, `bounds` is all a culling compute shader touches at 32
 * bytes a slot rather than 128, and `prevTransform` and the generations are CPU-only.
 * Each is flat and uploads in one copy.
 */

/// Declared, never defined; it exists so an `InstanceId` and a `BodyId` cannot be swapped.
struct InstanceTag;

/**
 * @brief Opaque handle to one instance: dense slot plus a generation counter.
 *
 * The generation is what makes a stale handle detectable rather than a silent alias onto
 * whatever object took the slot afterwards -- `destroy(a); b = create(...);` leaves `a`
 * invalid even though `b` occupies `a`'s slot.
 */
using InstanceId = core::Handle<InstanceTag>;

/// Bits in `GpuInstance::meta.z`. Shared with the shaders through `instance.glsl`.
enum InstanceFlags : uint32_t {
    /// Slot holds a live object. Cleared on destroy() rather than compacted away, so
    /// every consumer that walks the array has one thing to test.
    kInstanceLive = 1u << 0,
    /// ALPHA_MODE BLEND: the forward pass draws it, the G-buffer cannot represent it.
    kInstanceBlended = 1u << 1,
    /// The transform may change between frames. Keeps the instance out of the baked
    /// static BLAS -- see `accelTier` below -- and makes the velocity pass write a real
    /// velocity rather than the reproject-from-depth sentinel.
    kInstanceDynamic = 1u << 2,
    /// Survived culling this frame. Written by the cull dispatch, read by the draw-command
    /// compaction; meaningless when culling is off.
    kInstanceVisible = 1u << 3,
    /// Driven by a skin. Its vertices come out of the deformed vertex buffer that
    /// `skinning.comp` wrote, not out of the scene's, and it never merges into an
    /// instanced run -- each copy has its own vertices.
    kInstanceSkinned = 1u << 4,
    /// Driven by morph targets. Takes the same deformation dispatch and output buffer as a
    /// skin, which is why the two are only ever tested together through `kInstanceDeformed`.
    kInstanceMorphed = 1u << 5,
    /// ALPHA_MODE MASK: the shadow pass must run `shadow.frag` to cut the silhouette out
    /// of the depth it writes. Everything without this bit takes a pipeline with no
    /// fragment shader at all, which is what keeps early-Z and the double-rate depth-only
    /// path for the 87% of Sponza's triangles that are simply opaque.
    kInstanceMasked = 1u << 6,
    /**
     * @brief Solved by a Jolt soft body.
     *
     * The third producer of the deformed vertex buffer, and the only one that is a
     * transfer rather than a dispatch: a cloth's vertices arrive by `vkCmdCopyBuffer` from
     * the CPU solve. Being in `kInstanceDeformed` is what gets it a `skinDestBase` range,
     * an infinite culling box, no LOD chain and a dynamic BLAS with no site mentioning
     * cloth; what it must *not* get is a `SkinBatch`, there being no dispatch to record,
     * and the command build is the one place that tests this bit by name.
     */
    kInstanceCloth = 1u << 7,
};

/// What sends an instance through `skinning.comp` and makes it draw out of the deformed
/// vertex buffer. A mask rather than a third flag because the causes are independent: a
/// mesh may be skinned, morphed, or both, and the dispatch handles all three the same way.
inline constexpr uint32_t kInstanceDeformed = kInstanceSkinned | kInstanceMorphed | kInstanceCloth;

/**
 * @brief Which tier of the ray-tracing structure an instance belongs in.
 *
 * `Static` is baked: its transform is folded into the shared BLAS at build time and the
 * TLAS holds that BLAS once, at identity, so nothing about it can change again. `Rigid`
 * shares a model-space BLAS and carries its own TLAS instance, so moving it costs one 3x4
 * transform a frame. `Deformed` has its vertices rewritten every frame, so it needs a BLAS
 * of its own, built with ALLOW_UPDATE and refitted, *as well as* a TLAS instance.
 */
enum class AccelTier : uint8_t { Static, Rigid, Deformed };

/**
 * @brief Sort one instance's flags into a tier.
 *
 * The deformed test comes first and the order is load-bearing: `create()` sets
 * `kInstanceDynamic` on anything deformed, so the two bits are not exclusive and a
 * character sorted into `Rigid` would trace its bind pose forever.
 *
 * `deformedVertices` is whether the renderer has a deformed vertex buffer for this
 * instance to have been written into; without one there is nothing to refit, so it falls
 * back to `Static`. Falling back to `Rigid` instead is equivalent in what it traces and
 * changes the golden `skin` frame, for a case that is a load-time transient either way.
 */
[[nodiscard]] constexpr AccelTier accelTier(uint32_t flags, bool deformedVertices) {
    if ((flags & kInstanceDeformed) != 0u) return deformedVertices ? AccelTier::Deformed : AccelTier::Static;
    if ((flags & kInstanceDynamic) != 0u) return AccelTier::Rigid;
    return AccelTier::Static;
}

/**
 * @brief Whether a static instance has left the transform an acceleration structure baked
 *        into it, by enough to be worth rebuilding for.
 *
 * A `Static` instance that moves afterwards leaves a traced copy behind -- a shadow and a
 * reflection with nothing casting them -- because nothing refits it per frame.
 *
 * 1e-4, not an exact comparison: a node writes its instance through
 * `compose(decompose(m))`, which is not bit-exact, so an exact test rebuilds the whole
 * structure at load in every scene. The tolerance is four orders above that round trip and
 * four below a placement error anyone could see.
 */
[[nodiscard]] inline bool movedSinceBake(const glm::mat4& baked, const glm::mat4& model) {
    constexpr float kTolerance = 1e-4f;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            if (std::abs(baked[col][row] - model[col][row]) > kTolerance) return true;
        }
    }
    return false;
}

/**
 * @brief Per-instance data the vertex and fragment stages read. Must match the
 *        `Instance` struct in `engine/shaders/instance.glsl` exactly.
 *
 * 128 bytes, which is a cache line pair and keeps the array's stride a power of two. The
 * normal matrix is stored rather than derived, because deriving it costs about thirty
 * flops per *vertex* to save 48 bytes per *object*.
 */
struct GpuInstance {
    glm::mat4 model{1.0f};
    /// Rows of the normal matrix, one per vec4 because a std430 mat3 is padded to the
    /// same 48 bytes anyway and this way the padding is visible.
    glm::vec4 normal0{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 normal1{0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 normal2{0.0f, 0.0f, 1.0f, 0.0f};
    /// x primitive, y material, z InstanceFlags, w the animator *character* driving it
    /// (UINT32_MAX for none). A character and not a skin: several copies of one skinned
    /// mesh are several characters over one skin, each with its own block of joint
    /// matrices, and it is the block the dispatch needs.
    glm::uvec4 meta{0u, 0u, 0u, kNoNode};
};

static_assert(sizeof(GpuInstance) == 128, "GpuInstance must match instance.glsl");

/**
 * @brief World-space bounds, the only thing a culling dispatch reads. Must match
 *        instance.glsl.
 *
 * Separate from `GpuInstance` so the cull pass streams 32 bytes a slot instead of 160.
 * Recomputed by setTransform(), which is the only thing that can invalidate it.
 */
struct GpuInstanceBounds {
    /// w is the bounding-sphere radius, which a cheap frustum test can use before falling
    /// back to the box. Packed into the padding a vec3 would waste anyway.
    glm::vec4 worldMin{0.0f};
    glm::vec4 worldMax{0.0f};
};

static_assert(sizeof(GpuInstanceBounds) == 32, "GpuInstanceBounds must match instance.glsl");

/**
 * @brief Whether an instance moves. An enum with no default, because as a bare `bool`
 *        argument it is the one callers get wrong.
 *
 * It decides whether the instance writes a velocity for TAA and which tier of the
 * acceleration structure it lands in, so getting it wrong is a shadow that stays behind
 * after the thing casting it has been knocked over -- a call-site typo that reads as a
 * renderer defect.
 */
enum class InstanceMotion {
    Static,  ///< Furniture. Placed once and never moved again.
    Dynamic, ///< Moved after creation, by physics, a node, or a game writing a transform.
};

/// What create() needs.
struct InstanceDesc {
    uint32_t primitive = 0;
    uint32_t material = 0;
    /// Index range into the scene's shared index buffer, copied from the primitive. Held
    /// per instance because this is precisely a `VkDrawIndexedIndirectCommand`'s first two
    /// fields, and an indirect buffer built by walking two arrays is a join for nothing.
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    /// The vertices those indices reach, and where this primitive's skinning influences
    /// start. Indices are what a draw needs; vertices are what the skinning dispatch
    /// needs, and it has no index buffer to derive them from.
    uint32_t baseVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t skinOffset = 0xFFFFFFFFu;
    /// Where the primitive's morph deltas start, and how many targets it has.
    /// A count of zero is what says "no morph targets", so there is no second sentinel.
    uint32_t morphOffset = 0;
    uint32_t morphTargets = 0;
    /// Where *this placement's* weights start in the pose's flat weight array. Morph
    /// weights are per node, not per mesh, so deriving this from the primitive beside
    /// `morphOffset` would give two placements of one mesh the same expression.
    uint32_t morphWeightOffset = 0;
    /// Object-space bounds; the table transforms them.
    glm::vec3 localMin{0.0f};
    glm::vec3 localMax{0.0f};
    glm::mat4 transform{1.0f};
    /// Index into the scene's skin list, or UINT32_MAX for rigid geometry. Setting it is
    /// what makes an instance skinned; there is no separate flag to disagree with.
    uint32_t skin = 0xFFFFFFFFu;
    /// The animator character whose pose deforms it, or UINT32_MAX for none. Distinct from
    /// `skin`: several characters may share one skin, and a morph-only mesh has a pose and
    /// no skeleton at all.
    uint32_t character = kNoNode;
    bool blended = false;
    /// ALPHA_MODE MASK. Sets `kInstanceMasked`, which partitions the shadow pass's draws
    /// into the two pipelines.
    bool masked = false;
    bool dynamic = false;
    /// A `FABRIC_` primitive, solved as a soft body. **A cloth instance's `transform` must
    /// be identity** -- the placement is baked into the solved vertices, so a second copy
    /// of it here moves the fabric twice.
    bool cloth = false;
};

/// Dense, slot-stable storage for everything the renderer draws. World transforms only;
/// glTF's node tree is flattened at load.
class InstanceTable {
  public:
    /// Reserve storage for `n` instances. Purely an allocation hint; create() grows.
    void reserve(uint32_t n);

    /// Add an instance and return its handle. Never invalidates an existing handle.
    InstanceId create(const InstanceDesc& desc);

    /// Free the slot. A second destroy() of the same handle is a no-op, not a
    /// double-free: the generation has already moved past it.
    void destroy(InstanceId id);

    [[nodiscard]] bool valid(InstanceId id) const {
        return id.index < generations.size() && generations[id.index] == id.generation &&
               (shading[id.index].meta.z & kInstanceLive) != 0u;
    }

    /// Move an instance. Refreshes the normal matrix and the world bounds, which is
    /// why this is a call rather than a mutable reference handed out -- a caller that
    /// wrote `table.transform(id) = m` would leave both stale.
    void setTransform(InstanceId id, const glm::mat4& m);

    [[nodiscard]] const glm::mat4& transform(InstanceId id) const { return shading[id.index].model; }
    /// The transform of a slot, for the walkers -- an id is what an owner holds, and
    /// rebuilding one through `idAt` to index by it round-trips a generation counter that
    /// was never in doubt.
    [[nodiscard]] const glm::mat4& transform(uint32_t slot) const { return shading[slot].model; }

    /// The id currently occupying a slot, for a walker that has to call something taking
    /// an id. The generation will not validate if the slot is dead, so the result still
    /// has to go through `valid()`.
    [[nodiscard]] InstanceId idAt(uint32_t slot) const { return InstanceId{slot, generations[slot]}; }
    /// Where the instance was last frame. "Has never moved" is equality with `transform`,
    /// which is what create() establishes.
    [[nodiscard]] const glm::mat4& previousTransform(InstanceId id) const { return prevTransforms[id.index]; }

    void setFlags(InstanceId id, uint32_t set, uint32_t clear);
    [[nodiscard]] uint32_t flags(InstanceId id) const { return shading[id.index].meta.z; }

    /**
     * @brief Roll the transforms into the history the velocity pass reprojects against.
     *        Called once per frame, **before** simulation.
     *
     * Called after simulation instead, it copies the transforms that frame just wrote, so
     * `previousTransform` equals `transform` for everything and every correction is zero
     * -- a velocity pass that runs, costs its clear and its draws, and reports that
     * nothing moved.
     *
     * It does not bump the revision: the history is not observable through the table, and
     * bumping it would re-upload the whole shading array every frame.
     */
    void endFrame();

    /// Slots including holes. The length of every array below, and the value
    /// `gl_InstanceIndex` is bounded by -- not a count of live objects.
    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(shading.size()); }
    [[nodiscard]] uint32_t liveCount() const { return live; }
    /// Live instances whose material is ALPHA_MODE BLEND, so the forward pass can size its
    /// sort scratch without walking the table first.
    [[nodiscard]] uint32_t blendedCount() const { return blended; }

    /// Bumped by every mutation. The renderer compares it against what each
    /// frame-in-flight buffer last saw, so a static scene uploads once rather than every
    /// frame.
    [[nodiscard]] uint64_t revision() const { return rev; }

    // Indexed by slot, holes included. Contiguous and uploadable in one copy each.

    [[nodiscard]] const GpuInstance* shadingData() const { return shading.data(); }
    [[nodiscard]] const GpuInstanceBounds* boundsData() const { return bounds.data(); }
    [[nodiscard]] const glm::mat4* previousData() const { return prevTransforms.data(); }

    /// Byte sizes of the GPU-visible arrays, for sizing and copying.
    [[nodiscard]] uint64_t shadingBytes() const { return shading.size() * sizeof(GpuInstance); }
    [[nodiscard]] uint64_t boundsBytes() const { return bounds.size() * sizeof(GpuInstanceBounds); }
    [[nodiscard]] uint64_t previousBytes() const { return prevTransforms.size() * sizeof(glm::mat4); }

    /// Live, non-blended instances whose transform may change between frames -- what the
    /// velocity pass draws. Counted rather than derived, so a scene with nothing dynamic
    /// in it can skip the pass without walking the table to discover that.
    [[nodiscard]] uint32_t dynamicCount() const { return dynamic; }

    /// Which animator character drives a slot, or UINT32_MAX.
    [[nodiscard]] uint32_t characterOf(uint32_t slot) const { return shading[slot].meta.w; }

    /// Point an instance at a different animator character -- what a second copy of a
    /// skinned mesh needs, sharing the primitive, the influences and the skin and
    /// differing only in the pose that deforms it.
    void setCharacter(InstanceId id, uint32_t character);

    /// Index range for one slot. Separate from `shading` because the GPU never reads it:
    /// these go into the indirect buffer, not into a shader.
    struct DrawRange {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        uint32_t baseVertex = 0;
        uint32_t vertexCount = 0;
        /// Start of this primitive's entries in the scene's skin-influence array, or
        /// UINT32_MAX when the primitive carries none.
        uint32_t skinOffset = 0xFFFFFFFFu;
        /// Start of this primitive's morph deltas, and how many targets it has.
        uint32_t morphOffset = 0;
        uint32_t morphTargets = 0;
        /// Where this placement's weights start in the animator's flat weight array,
        /// before the character's own base is added.
        uint32_t morphWeightOffset = 0;
    };
    [[nodiscard]] const std::vector<DrawRange>& drawRanges() const { return ranges; }

    /// One slot's record. Bounds-checked only in Debug: this is the inner loop of every
    /// pass.
    [[nodiscard]] const GpuInstance& slot(uint32_t index) const { return shading[index]; }
    [[nodiscard]] const GpuInstanceBounds& slotBounds(uint32_t index) const { return bounds[index]; }

    void clear();

  private:
    /// Transform `desc`'s object-space AABB by `m` and store the world box plus its
    /// bounding-sphere radius. Eight corners, because transforming min and max alone
    /// is wrong the moment the matrix contains a rotation.
    void refreshBounds(uint32_t slot, const glm::mat4& m);

    std::vector<GpuInstance> shading;
    std::vector<GpuInstanceBounds> bounds;
    std::vector<glm::mat4> prevTransforms;
    std::vector<DrawRange> ranges;

    /// Object-space bounds, kept so setTransform() can rebuild the world box without
    /// reaching back into the scene the instance came from. CPU-only.
    std::vector<glm::vec3> localMin;
    std::vector<glm::vec3> localMax;

    /// Parallel to the arrays above. Incremented on destroy(), so a handle issued before
    /// it can never match again.
    std::vector<uint32_t> generations;
    std::vector<uint32_t> freeSlots;

    uint32_t live = 0;
    uint32_t blended = 0;
    /// Live, non-blended and flagged dynamic. Maintained rather than counted on demand:
    /// the velocity pass asks every frame and the answer changes only when the table does.
    uint32_t dynamic = 0;
    uint64_t rev = 1;
};

} // namespace scene
