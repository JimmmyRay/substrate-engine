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
 * @brief The scene's renderable objects, as flat arrays the GPU can read (4.1).
 *
 * Replaces the flat `DrawItem` list `GltfScene` used to own. That list had no
 * identity, no history, and bounds that lived on `Primitive` in local space, and four
 * separate renderer consumers needed what it lacked: motion vectors want the previous
 * frame's transform per object (3.4), a multi-instance TLAS wants a per-instance
 * transform (3.9), GPU culling wants world bounds and a visibility bit (4.2), and
 * residency wants per-object state (4.6b).
 *
 * **Slot stability is the load-bearing property.** All four consumers key off a slot
 * index, so a slot must not move while an object lives -- which rules out the
 * swap-and-pop compaction an ECS storage would do. Destroying an instance leaves a
 * hole that the free list hands back to the next create(), and the arrays are only
 * ever appended to.
 *
 * The arrays are split by *consumer*, not by field: values read together stay
 * together, values read by different passes live in different arrays. `shading` is
 * what the vertex shaders index by `gl_InstanceIndex`; `bounds` is all a culling
 * compute shader touches, at 32 bytes a slot rather than 128; `prevTransform` and the
 * generation counters are CPU-only. Each array is flat and uploads in one copy, which
 * is exactly the property `entt::basic_storage` cannot offer -- see 4.1b.
 */

/// The tag that makes an instance handle its own type. Declared, never defined -- nothing
/// constructs one; it exists so an `InstanceId` and a `BodyId` cannot be swapped.
struct InstanceTag;

/**
 * @brief Opaque handle to one instance: dense slot plus a generation counter.
 *
 * External code never sees a raw slot index. The generation is what makes a stale
 * handle detectable rather than a silent alias onto whatever object was created in
 * the same slot afterwards -- so `destroy(a); b = create(...);` leaves `a` invalid
 * even though `b` occupies `a`'s slot.
 *
 * **An alias rather than a rename** (C1), so no call site moves: `.index`, `.generation`,
 * `==`, `!=` and `InstanceId{slot, gen}` all still mean what they meant. What it gains is
 * `valid()` and a type distinct from every other handle in the engine. This table was the
 * only subsystem that ever had this lifetime model; `core/Handle.h` is where it became the
 * one every subsystem has.
 */
using InstanceId = core::Handle<InstanceTag>;

/// Bits in `GpuInstance::meta.z`. Shared with the shaders through `instance.glsl`.
enum InstanceFlags : uint32_t {
    /// Slot holds a live object. Cleared on destroy() rather than compacted away, so
    /// every consumer that walks the array has one thing to test.
    kInstanceLive = 1u << 0,
    /// ALPHA_MODE BLEND: the forward pass draws it, the G-buffer cannot represent it.
    kInstanceBlended = 1u << 1,
    /// The transform may change between frames. Keeps the instance out of 3.9's baked
    /// static BLAS -- see `accelTier` below -- and tells 3.4 to write a real velocity
    /// rather than the reproject-from-depth sentinel. Data, not a build flag.
    kInstanceDynamic = 1u << 2,
    /// Survived culling this frame (4.2). Written by the cull dispatch, read by the
    /// draw-command compaction; meaningless when culling is off.
    kInstanceVisible = 1u << 3,
    /// Driven by a skin (4.4). Its vertices come out of the skinned vertex buffer that
    /// `skinning.comp` wrote, not out of the scene's, and it never merges into an
    /// instanced run -- each copy has its own vertices.
    kInstanceSkinned = 1u << 4,
    /// Driven by morph targets (S2.1). Takes the same deformation dispatch and the same
    /// output buffer as a skin, which is why the two are tested together everywhere
    /// through `kInstanceDeformed` -- a morphed mesh with no skin is a face with no
    /// skeleton, and it still needs its vertices written somewhere.
    kInstanceMorphed = 1u << 5,
    /// ALPHA_MODE MASK: the shadow pass must run `shadow.frag` to cut the silhouette out
    /// of the depth it writes. Everything without this bit goes through a pipeline with
    /// no fragment shader at all, which is what restores early-Z and the double-rate
    /// depth-only path for the 87% of Sponza's triangles that are simply opaque.
    kInstanceMasked = 1u << 6,
    /**
     * @brief Solved by a Jolt soft body (C19).
     *
     * The third producer of the deformed vertex buffer, and the first that is a **transfer
     * rather than a dispatch**: `skinning.comp` writes a skin's and a morph's vertices on
     * the GPU, and a cloth's arrive by `vkCmdCopyBuffer` from the CPU solve. Everything
     * downstream of the buffer is the same, which is the whole reason this is a bit beside
     * the other two rather than a second buffer and a second binding rule.
     *
     * OR'd into `kInstanceDeformed` below, so a cloth instance gets a `skinDestBase` range,
     * an infinite culling box, no LOD chain and a dynamic BLAS with no site having to
     * mention cloth. What it must *not* get is a `SkinBatch` -- there is no dispatch to
     * record -- and that is the one place the command build tests this bit by name.
     */
    kInstanceCloth = 1u << 7,
};

/// What sends an instance through `skinning.comp` and makes it draw out of the deformed
/// vertex buffer. One mask rather than two tests at eight call sites, and the reason it
/// is a mask rather than a third flag is that the two causes are independent: a mesh may
/// be skinned, morphed, or both, and the dispatch handles all three the same way.
inline constexpr uint32_t kInstanceDeformed = kInstanceSkinned | kInstanceMorphed | kInstanceCloth;

/**
 * @brief Which tier of the ray-tracing structure an instance belongs in.
 *
 * `Static` is baked: its transform is folded into the shared BLAS at build time, and the
 * TLAS holds that BLAS once, at identity. Nothing about it can ever change again.
 *
 * `Rigid` moves but does not deform. It shares a BLAS with every other instance of the
 * same primitive, in model space, and carries its own TLAS instance -- so moving it is
 * one 3x4 transform written per frame and the TLAS rebuild that was happening anyway.
 *
 * `Deformed` has its vertices rewritten every frame, so it needs a BLAS of its own,
 * built with ALLOW_UPDATE and refitted, *as well as* a TLAS instance.
 */
enum class AccelTier : uint8_t { Static, Rigid, Deformed };

/**
 * @brief Sort one instance's flags into a tier.
 *
 * Here rather than in `gfx/AccelStruct.cpp` because it is a question about these flags
 * and answering it needs no device -- which is also what lets the unit suite hold it.
 *
 * The deformed test comes first and the order is load-bearing: `create()` sets
 * `kInstanceDynamic` on anything deformed, so the two bits are not exclusive and a
 * character sorted into `Rigid` would trace its bind pose forever.
 *
 * `deformedVertices` is whether the renderer has a deformed vertex buffer for this
 * instance to have been written into. Without one there is nothing to refit and it falls
 * back to `Static`, tracing its bind pose -- which is what 3.9 did for every skinned mesh.
 *
 * **`Rigid` would be the tidier fallback and it is deliberately not used.** The two are
 * equivalent here -- both put the bind pose at the instance's current transform, one
 * baked and one on a TLAS instance -- and the case is a load-time transient rather than a
 * steady state: a scene with a skin gets its deformed buffer during the same load, and
 * the structure is rebuilt once it has. Routing that transient through a tier it does not
 * need changed the golden `skin` frame, for nothing anyone can see. It stays where it was.
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
 * A `Static` instance's transform is baked into the static BLAS at build time and there is
 * no per-frame refit that would carry it, so one that moves afterwards leaves a traced
 * copy behind: a shadow and a reflection with nothing casting them.
 *
 * The tolerance is what stops that check firing on nothing. A node writes its instance
 * through `compose(decompose(m))`, which is not bit-exact, and every scene has instances
 * that are pushed a transform equal to the one they already hold -- so an exact comparison
 * would rebuild the whole structure at load in every scene. 1e-4 is four orders above that
 * round trip and four below a placement error anyone could see.
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
 * 128 bytes, which is a cache line pair and keeps the array's stride a power of two.
 * The normal matrix is stored rather than derived: `inverse(transpose(mat3(model)))`
 * is about thirty flops, and paying that per *vertex* to save 48 bytes per *object*
 * is the wrong side of the trade.
 */
struct GpuInstance {
    glm::mat4 model{1.0f};
    /// Rows of the normal matrix, one per vec4 because a std430 mat3 is padded to the
    /// same 48 bytes anyway and this way the padding is visible.
    glm::vec4 normal0{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 normal1{0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 normal2{0.0f, 0.0f, 1.0f, 0.0f};
    /// x primitive, y material, z InstanceFlags, w the animator *character* driving it
    /// (UINT32_MAX for none). A character rather than a skin since S2.3: several copies
    /// of one skinned mesh are several characters over one skin, each with its own pose
    /// and its own block of joint matrices, and it is the block the dispatch needs.
    /// `addSceneInstances` writes the skin index, which is the character the animator
    /// creates for it -- so a plain glTF scene numbers both the same way.
    glm::uvec4 meta{0u, 0u, 0u, kNoNode};
};

static_assert(sizeof(GpuInstance) == 128, "GpuInstance must match instance.glsl");

/**
 * @brief World-space bounds, the only thing a culling dispatch reads.
 *
 * Separate from `GpuInstance` so 4.2's compute pass streams 32 bytes a slot instead
 * of 160. World rather than local: the local bounds on `Primitive` are what the old
 * draw list carried, and every consumer had to re-transform eight corners to use
 * them. Recomputed on setTransform(), which is the only thing that can invalidate it.
 */
struct GpuInstanceBounds {
    /// w is the bounding-sphere radius, which a cheap frustum test can use before
    /// falling back to the box. Packed into the padding a vec3 would waste anyway.
    glm::vec4 worldMin{0.0f};
    glm::vec4 worldMax{0.0f};
};

static_assert(sizeof(GpuInstanceBounds) == 32, "GpuInstanceBounds must match instance.glsl");

/**
 * @brief Whether an instance moves. Not a bool, and it has no default.
 *
 * `InstanceDesc::dynamic` is a bool because it is one field among thirty in a struct a
 * caller fills deliberately. As an *argument* it is the one people get wrong, and it is
 * not cosmetic: it decides whether the instance writes a velocity for TAA and which tier
 * of the acceleration structure it lands in. Getting it wrong is a shadow that stays
 * behind after the thing casting it has been knocked over, which is a bug that looks like
 * a renderer defect and is a call-site typo.
 */
enum class InstanceMotion {
    Static,  ///< Furniture. Placed once and never moved again.
    Dynamic, ///< Moved after creation, by physics, a node, or a game writing a transform.
};

/// What create() needs. A parameter struct rather than eight arguments, so a caller
/// that sets only a transform is not writing five defaults by hand.
struct InstanceDesc {
    uint32_t primitive = 0;
    uint32_t material = 0;
    /// Index range into the scene's shared index buffer, copied from the primitive.
    /// Held per instance rather than looked up through a `Primitive` array because
    /// this is precisely a `VkDrawIndexedIndirectCommand`'s first two fields (0.11),
    /// and an indirect buffer built by walking two arrays is a join for nothing.
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    /// The vertices those indices reach, and where this primitive's skinning
    /// influences start. Indices are what a draw needs; vertices are what a *skinning
    /// dispatch* needs (4.4), and it has no index buffer to derive them from.
    uint32_t baseVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t skinOffset = 0xFFFFFFFFu;
    /// Where the primitive's morph deltas start, and how many targets it has (S2.1).
    /// A count of zero is what says "no morph targets", so there is no second sentinel.
    uint32_t morphOffset = 0;
    uint32_t morphTargets = 0;
    /// Where *this placement's* weights start in the pose's flat weight array. Morph
    /// weights are per node rather than per mesh, so two placements of one morphed mesh
    /// wear different expressions -- which is the whole reason this is not derived from
    /// the primitive beside `morphOffset`.
    uint32_t morphWeightOffset = 0;
    /// Object-space bounds; the table transforms them.
    glm::vec3 localMin{0.0f};
    glm::vec3 localMax{0.0f};
    glm::mat4 transform{1.0f};
    /// Index into the scene's skin list, or UINT32_MAX for rigid geometry. Setting it
    /// is what makes an instance skinned; there is no separate flag to disagree with.
    uint32_t skin = 0xFFFFFFFFu;
    /// The animator character whose pose deforms it, or UINT32_MAX for none. A second
    /// field beside `skin` rather than the same one, because they stopped meaning the
    /// same thing twice over: several characters may share one skin (S2.3), and a
    /// morph-only mesh has a pose and no skeleton at all (S2.1).
    uint32_t character = kNoNode;
    bool blended = false;
    /// ALPHA_MODE MASK. Sets `kInstanceMasked`, which is what partitions the shadow
    /// pass's draws into the two pipelines.
    bool masked = false;
    bool dynamic = false;
    /// A `FABRIC_` primitive, solved as a soft body (C19). A bool rather than an offset
    /// like `skinOffset`, because unlike a skin there is nothing per-instance to carry: the
    /// solved vertices arrive by transfer, addressed by the slot's `skinDestBase`, and the
    /// inverse masses were consumed at load. **A cloth instance's `transform` must be
    /// identity** -- the placement is baked into the vertices, so a second copy of it here
    /// would move the fabric twice.
    bool cloth = false;
};

/**
 * @brief Dense, slot-stable storage for everything the renderer draws.
 *
 * Not a scene graph and not an ECS. There is no hierarchy here -- glTF's node tree is
 * flattened at load and world transforms are what the table stores -- and there are
 * no gameplay fields, which is property (iv) of 4.1b: an entity component system
 * adopted later sits *beside* this, holding an `InstanceId` as a component, rather
 * than trying to own it.
 */
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
    /// The transform of a slot, for the walkers. Every consumer that iterates the table
    /// has a slot rather than an id -- the id is what an *owner* holds -- and rebuilding
    /// one through `idAt` just to index by it is a round trip through a generation
    /// counter that was never in doubt.
    [[nodiscard]] const glm::mat4& transform(uint32_t slot) const { return shading[slot].model; }

    /// The id currently occupying a slot. What a walker needs to call something that
    /// takes an id -- the inspector selects by slot, because a selection has to survive
    /// the object under it being replaced, and an id is designed to stop being valid
    /// exactly then. Returns an id whose generation will not validate if the slot is
    /// dead, which is what `valid()` is for.
    [[nodiscard]] InstanceId idAt(uint32_t slot) const { return InstanceId{slot, generations[slot]}; }
    /// Where the instance was last frame. The sentinel for "has never moved" is
    /// equality with `transform`, which is what endFrame() establishes on creation.
    [[nodiscard]] const glm::mat4& previousTransform(InstanceId id) const { return prevTransforms[id.index]; }

    void setFlags(InstanceId id, uint32_t set, uint32_t clear);
    [[nodiscard]] uint32_t flags(InstanceId id) const { return shading[id.index].meta.z; }

    /**
     * @brief Roll the transforms into the history 3.4's velocity pass reprojects
     *        against. Called once per frame, **before** simulation.
     *
     * Before rather than after, and the difference is the whole of whether the pass
     * works: called after simulation it copies the transforms that frame just wrote, so
     * `previousTransform` equals `transform` for everything and every correction is
     * zero -- a velocity pass that runs, costs its clear and its draws, and reports that
     * nothing moved. Called first, it captures where things were when the last frame
     * drew them, which is what the previous frame's image actually holds.
     *
     * Unconditional over every slot rather than tracked per object: a copy of 64 bytes
     * a slot is cheaper than the bookkeeping that would tell you which ones to skip,
     * and getting that bookkeeping wrong smears exactly the objects that moved. It does
     * not bump the revision -- the history is not something a consumer of the *table*
     * can observe, and bumping it would re-upload the whole shading array every frame.
     */
    void endFrame();

    /// Slots including holes. This is the length of every array below, and the value
    /// `gl_InstanceIndex` is bounded by -- not a count of live objects.
    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(shading.size()); }
    [[nodiscard]] uint32_t liveCount() const { return live; }
    /// Live instances whose material is ALPHA_MODE BLEND, so the forward pass can size
    /// its sort scratch without walking the table first.
    [[nodiscard]] uint32_t blendedCount() const { return blended; }

    /// Bumped by every mutation. The renderer compares it against what each
    /// frame-in-flight buffer last saw, so a static scene uploads once rather than
    /// every frame -- and a moving one costs a memcpy, not a diff.
    [[nodiscard]] uint64_t revision() const { return rev; }

    // ------------------------------------------------------------- flat arrays
    // Indexed by slot, holes included. Contiguous and uploadable in one copy each.

    [[nodiscard]] const GpuInstance* shadingData() const { return shading.data(); }
    [[nodiscard]] const GpuInstanceBounds* boundsData() const { return bounds.data(); }
    /// The history array, for 3.4's motion correction. GPU-visible since the velocity
    /// pass exists; before that it was rolled and read by nothing.
    [[nodiscard]] const glm::mat4* previousData() const { return prevTransforms.data(); }

    /// Byte sizes of the GPU-visible arrays, for sizing and copying.
    [[nodiscard]] uint64_t shadingBytes() const { return shading.size() * sizeof(GpuInstance); }
    [[nodiscard]] uint64_t boundsBytes() const { return bounds.size() * sizeof(GpuInstanceBounds); }
    [[nodiscard]] uint64_t previousBytes() const { return prevTransforms.size() * sizeof(glm::mat4); }

    /// Live, non-blended instances whose transform may change between frames -- what
    /// 3.4's velocity pass draws. Counted rather than derived at the call site so a
    /// scene with nothing dynamic in it can skip the pass without walking the table
    /// every frame to discover that.
    [[nodiscard]] uint32_t dynamicCount() const { return dynamic; }

    /// Which animator character drives slot `i`, or UINT32_MAX. Read straight off the
    /// record rather than kept in a second array: it is one word of the meta the GPU
    /// reads anyway.
    [[nodiscard]] uint32_t characterOf(uint32_t slot) const { return shading[slot].meta.w; }

    /// Point slot `id` at a different animator character (S2.3). What a game calls when
    /// it creates a second copy of a skinned mesh: the copy shares the primitive, the
    /// influences and the skin, and differs in exactly which pose it is deformed by.
    void setCharacter(InstanceId id, uint32_t character);

    /// Index range for slot `i`, as the first two fields of an indirect command want
    /// them. Separate from `shading` because the GPU never reads it: `firstIndex` and
    /// `indexCount` go into the indirect buffer, not into a shader.
    struct DrawRange {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        uint32_t baseVertex = 0;
        uint32_t vertexCount = 0;
        /// Start of this primitive's entries in the scene's skin-influence array, or
        /// UINT32_MAX when the primitive carries none.
        uint32_t skinOffset = 0xFFFFFFFFu;
        /// Start of this primitive's morph deltas, and how many targets it has (S2.1).
        uint32_t morphOffset = 0;
        uint32_t morphTargets = 0;
        /// Where this placement's weights start in the animator's flat weight array,
        /// before the character's own base is added.
        uint32_t morphWeightOffset = 0;
    };
    [[nodiscard]] const std::vector<DrawRange>& drawRanges() const { return ranges; }

    /// Slot `i`'s record, for the record loops. Bounds-checked only in Debug: this is
    /// the inner loop of every pass.
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

    /// Parallel to the arrays above. Incremented on destroy(), so a handle issued
    /// before it can never match again.
    std::vector<uint32_t> generations;
    std::vector<uint32_t> freeSlots;

    uint32_t live = 0;
    uint32_t blended = 0;
    /// Live, non-blended and flagged dynamic. Maintained beside `blended` rather than
    /// counted on demand, and for the same reason: the velocity pass asks every frame
    /// and the answer changes only when the table does.
    uint32_t dynamic = 0;
    uint64_t rev = 1;
};

} // namespace scene
