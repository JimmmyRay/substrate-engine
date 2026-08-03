#pragma once

#include "gfx/Resources.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace scene {
class InstanceTable;
}

namespace gfx {

struct VulkanContext;
class Uploader;

/**
 * @brief One acceleration structure and the buffer it lives in.
 *
 * Beside `GpuBuffer` and `GpuImage` in spirit, and built by free functions that take a
 * `VulkanContext&` exactly as those are. Tethered's `AccelerationStructureVk` is 1265
 * lines and is written against a `Device`/`Command` RHI with a `friend class Device`;
 * what travels from it is the *shape of the build* -- scratch sizing, the two-phase
 * size query, the barrier between BLAS and TLAS -- and none of the plumbing.
 */
struct GpuAccelStruct {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    GpuBuffer buffer;
    VkDeviceAddress address = 0;
};

/**
 * @brief The scene's whole ray-tracing structure: baked geometry, rigid movers, and
 *        deformed copies.
 *
 * ## The three tiers, and why the split is by *how* a thing moves
 *
 * 3.9 shipped one BLAS holding every primitive as a separate geometry with its own
 * baked 3x4 transform, and a TLAS with a single identity instance. That is exactly
 * right for geometry that never moves, and it stays exactly right -- `staticBlas` is
 * still built once at load and never touched again.
 *
 * What S2 ends is the premise that *nothing* moves. A skinned or morphed instance has
 * its vertices rewritten by `skinning.comp` every frame, so its geometry cannot be
 * baked into a shared structure: it gets a BLAS of its own, built with
 * `ALLOW_UPDATE`, and **refitted** rather than rebuilt each frame.
 *
 * Between those two sits the case both of them miss, and missing it is what this file
 * got wrong for two arcs: an instance that **moves without deforming**. A crate a
 * physics solver pushes has the same vertices it always had, so it needs no refit --
 * but its transform changes, and a baked transform cannot change. Sorting it into the
 * static tier gives an object that draws in its new place and traces in its old one,
 * which shows up as a shadow that stays behind after the thing casting it is knocked
 * over. It belongs in `rigidBlas`: model-space geometry, no baked transform, and a TLAS
 * instance whose 3x4 is rewritten every frame beside the deformed ones.
 *
 * The TLAS therefore holds one instance for the static BLAS, one per deformed instance
 * and one per rigid mover, and is rebuilt every frame -- which is cheap, because a TLAS
 * build over a handful of instances is dominated by its own launch overhead.
 *
 * The roadmap predicted exactly this shape and called the TLAS holding N instances
 * "the whole architectural change". It was, and the selector is the instance's flags:
 * data, not a build flag. `scene::accelTier` is that selector, and it is in the scene
 * layer because it is a question about flags and needs no device to answer.
 *
 * ## One BLAS per primitive, not per mover
 *
 * A deformed instance owns its vertices, so it owns its BLAS. A rigid one does not: six
 * crates are six transforms over one cube. `rigidBlas` therefore holds one structure per
 * distinct primitive among the movers and `rigidSlot` is the per-*instance* list beside
 * it. Nothing downstream has to know: a shader finds its hit record through
 * `instanceCustomIndex`, which belongs to the TLAS instance rather than to the BLAS, so
 * two instances sharing a structure still resolve to their own slot and material.
 *
 * ## Refit, not rebuild
 *
 * `VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR` keeps the tree topology and moves
 * its bounds, which is correct only while the vertices stay roughly where they were --
 * the case a skeleton animating a mesh is. A refitted structure degrades if the pose
 * wanders far from the one it was built in; the honest statement is that this trades
 * trace quality for build time and that a character that turns inside out would want
 * a periodic full rebuild. Nothing here does that yet, and the reason is that nothing
 * has measured a case where it matters.
 *
 * ## Indices are rebased once, at load
 *
 * A deformed instance's vertices live in their own range of the deformed vertex buffer,
 * while the scene's index buffer holds indices into the *scene's* vertex buffer. The
 * draw path fixes that for free with an indirect command's `vertexOffset`, which is a
 * signed field. An acceleration structure build has no signed equivalent -- only a
 * `firstVertex` that is added -- so the indices are rewritten once into
 * `dynamicIndices` and the per-frame path has nothing to fix up.
 */
/**
 * @brief What a shader needs to look up the triangle a ray hit.
 *
 * A ray query reports `instanceCustomIndex`, `geometryIndex` and `primitiveIndex`, and
 * none of those is a material or a vertex range. This table closes that gap: one record
 * per BLAS geometry, and `instanceCustomIndex + geometryIndex` is its index.
 *
 * That addition is the whole indexing scheme, and it works because of how the two tiers
 * are laid out. The static BLAS holds one geometry per static instance, so its geometry
 * indices are already 0..N-1 and its TLAS instance carries a custom index of 0. Each
 * dynamic BLAS holds exactly one geometry, so its geometry index is always 0 and the
 * custom index carries the whole offset. One expression covers both, with no sentinel to
 * test and no branch.
 *
 * `firstIndex` is an *element* offset into whichever index buffer `deformed` selects --
 * the scene's, or the rebased `dynamicIndices`. Keeping it in elements rather than bytes
 * is deliberate: the build wants bytes and the shader wants elements, and converting at
 * the one call site that needs bytes is better than storing the unit the shader would
 * have to divide back out on every hit.
 */
struct GpuHitRecord {
    uint32_t firstIndex;
    /// Into `InstanceTable`, for the normal matrix and `meta.y`'s material index. The
    /// instance SSBO is already bound to every tracing shader, so nothing is duplicated
    /// here that the shader could read for itself.
    uint32_t instanceSlot;
    /// 0 static, 1 deformed. Selects the vertex *and* index buffer as a pair; they are
    /// never mixed, which is why this is one flag rather than two.
    uint32_t deformed;
    uint32_t pad;
};

struct SceneAccelStruct {
    GpuAccelStruct staticBlas;
    /// One per deformed instance, in `dynamicSlot` order. Built with ALLOW_UPDATE.
    std::vector<GpuAccelStruct> dynamicBlas;
    /// Which instance slot each dynamic BLAS belongs to, so the TLAS instance can take
    /// that slot's world transform without a second lookup.
    std::vector<uint32_t> dynamicSlot;
    /// One per *primitive* that some rigid mover draws, shared by every mover drawing it.
    /// Model space, no baked transform, built once and never updated -- the geometry does
    /// not change, only where the TLAS puts it.
    std::vector<GpuAccelStruct> rigidBlas;
    /// Which instance slot each rigid TLAS instance belongs to, in TLAS order after the
    /// deformed ones. Longer than `rigidBlas` whenever movers share a primitive, which is
    /// the usual case and the reason the two lists are not one.
    std::vector<uint32_t> rigidSlot;
    GpuAccelStruct tlas;

    /// What the per-frame refit needs to rebuild one dynamic BLAS's geometry
    /// description: everything else about it is the same for all of them and lives in
    /// the four addresses below. Kept as numbers rather than as a stored
    /// `VkAccelerationStructureGeometryKHR` because that struct holds a pointer chain,
    /// and a cached one whose `pGeometries` outlived its vector is the classic way this
    /// goes wrong.
    struct DynamicGeometry {
        uint32_t triangleCount = 0;
        uint32_t indexByteOffset = 0;
    };
    std::vector<DynamicGeometry> dynamicGeometry;
    VkDeviceAddress deformedVertexAddress = 0;
    VkDeviceAddress dynamicIndexAddress = 0;
    VkDeviceSize vertexStride = 0;
    uint32_t deformedMaxVertex = 0;

    /// Indices for the dynamic BLASes, rebased onto the deformed vertex buffer.
    GpuBuffer dynamicIndices;
    /// One `GpuHitRecord` per BLAS geometry: the static tier first, then one per dynamic
    /// BLAS. Read by device address from the shaders that shade a hit, which is why it
    /// carries SHADER_DEVICE_ADDRESS rather than being bound as a descriptor.
    GpuBuffer hitRecords;
    VkDeviceAddress hitRecordAddress = 0;
    /// The scene's own vertex and index buffers, by address, so a shader can reach the
    /// static tier's geometry. Held here rather than re-queried because the addresses are
    /// already computed during the build.
    VkDeviceAddress sceneVertexAddress = 0;
    VkDeviceAddress sceneIndexAddress = 0;
    /// Per-geometry transforms baked into `staticBlas`. Kept only because the build
    /// reads it by address and the structure must outlive nothing else.
    GpuBuffer staticTransforms;
    /// Which instance slot each static geometry was baked from, and the transform that was
    /// baked. Host-side, and the only reason it exists is `staticTierStale` -- a static
    /// instance that moves after the build has no refit to carry it, so the structure has
    /// to be asked whether it still describes the world.
    std::vector<uint32_t> staticSlot;
    std::vector<glm::mat4> staticBaked;
    /// `1 + dynamicBlas.size() + rigidSlot.size()` TLAS instances, host-visible and
    /// rewritten per frame.
    GpuBuffer instanceBuffer;
    /// Scratch for the per-frame refit and TLAS rebuild. Allocated once at the size the
    /// worst frame needs, because allocating inside a frame is the thing this avoids.
    GpuBuffer refitScratch;
    VkDeviceAddress refitScratchAddress = 0;
    VkDeviceSize blasUpdateScratchStride = 0;
    VkDeviceAddress tlasScratchAddress = 0;

    [[nodiscard]] bool valid() const { return tlas.handle != VK_NULL_HANDLE; }
    /// Whether anything in the structure can move, and so whether the per-frame refit has
    /// work to do. A scene of rigid movers and no characters refits no BLAS at all and
    /// still needs its TLAS rebuilt, which is why this is not `!dynamicBlas.empty()`.
    [[nodiscard]] bool hasDynamic() const { return !dynamicBlas.empty() || !rigidSlot.empty(); }
};

/**
 * @brief Build the whole structure for `instances`, once, at load.
 *
 * `deformedVertices` and `deformedBase` describe the buffer `skinning.comp` writes and
 * where each instance slot's vertices start in it; pass a null buffer and an empty
 * vector for a scene with no rig, and only the static tier is built.
 *
 * ## Everything is opaque
 *
 * Alpha-masked foliage is marked opaque here, so a ray-traced shadow from a leaf is the
 * shadow of its whole quad. The cascade path does better, because `shadow.frag` samples
 * the base-colour texture and discards; matching it needs non-opaque geometry and a
 * candidate-intersection loop in every shader that traces. That is a real difference and
 * it is why 3.10 complements the cascades rather than replacing them.
 *
 * Blocking: builds through `Uploader::beginImmediate`, which is a load-time facility.
 */
void buildSceneAccelStruct(const VulkanContext& ctx, Uploader& uploader, VkBuffer vertexBuffer,
                           VkDeviceSize vertexStride, uint32_t vertexCount, VkBuffer indexBuffer,
                           const std::vector<uint32_t>& sceneIndices, VkBuffer deformedVertices,
                           uint32_t deformedVertexCount, const std::vector<uint32_t>& deformedBase,
                           const scene::InstanceTable& instances, const std::vector<uint8_t>& emissiveMaterials,
                           SceneAccelStruct& out);

/**
 * @brief Move every mover: refit the deformed BLASes, rewrite the TLAS transforms of
 *        both tiers that have them, and rebuild the TLAS, into `cmd` (S2.5).
 *
 * Must be recorded *after* the deformation dispatch and its barrier, and before
 * anything traces. Records nothing at all when the scene has no dynamic geometry,
 * which is every scene this engine had before S2.
 */
void refitSceneAccelStruct(const VulkanContext& ctx, VkCommandBuffer cmd, const scene::InstanceTable& instances,
                           SceneAccelStruct& as);

/**
 * @brief Whether any instance baked into the static tier has moved since the build.
 *
 * The static tier has no refit: its geometry's transform is inside the BLAS. An instance
 * that moves after the build therefore traces from where it *was*, which reads as a shadow
 * and a reflection with nothing casting them.
 *
 * The case this exists for is ordering rather than motion. A game creates an instance,
 * attaches it to a scene node and calls `Renderer::setInstances` -- and the node's world
 * transform only reaches the instance on the first `Scene::update`, which is after the
 * structure was built from whatever transform `create` was handed.
 */
[[nodiscard]] bool staticTierStale(const SceneAccelStruct& as, const scene::InstanceTable& instances);

void destroySceneAccelStruct(const VulkanContext& ctx, SceneAccelStruct& as);
void destroyAccelStruct(const VulkanContext& ctx, GpuAccelStruct& as);

/// Device address of a buffer. Needs `bufferDeviceAddress`, which the context enables
/// unconditionally -- VMA wants it anyway.
VkDeviceAddress bufferAddress(const VulkanContext& ctx, VkBuffer buffer);

} // namespace gfx
