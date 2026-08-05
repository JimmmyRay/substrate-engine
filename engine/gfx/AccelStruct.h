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

/// @brief One acceleration structure and the buffer it lives in.
struct GpuAccelStruct {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    GpuBuffer buffer;
    VkDeviceAddress address = 0;
};

/**
 * @brief What a shader needs to look up the triangle a ray hit: one record per BLAS
 *        geometry, indexed by `instanceCustomIndex + geometryIndex`.
 *
 * That addition is the indexing scheme every tracing shader relies on, and it only works
 * while the static BLAS is the TLAS's first instance with custom index 0 and every
 * dynamic BLAS holds exactly one geometry. Adding a second geometry to a dynamic BLAS,
 * or emitting the tiers in another order, silently shifts every record.
 *
 * `firstIndex` is an *element* offset into whichever index buffer `deformed` selects; the
 * build wants bytes and converts at its own call site.
 */
struct GpuHitRecord {
    uint32_t firstIndex;
    /// Into `InstanceTable`, for the normal matrix and `meta.y`'s material index.
    uint32_t instanceSlot;
    /// 0 static, 1 deformed. Selects the vertex *and* index buffer as a pair; they are
    /// never mixed, which is why this is one flag rather than two.
    uint32_t deformed;
    uint32_t pad;
};

/**
 * @brief The scene's whole ray-tracing structure: baked geometry, rigid movers, and
 *        deformed copies, sorted by `scene::accelTier`.
 *
 * `staticBlas` bakes each geometry's transform into the structure, so anything sorted
 * into it that later moves draws in its new place and traces in its old one -- a shadow
 * left behind by a knocked-over crate. Anything whose transform can change belongs in
 * `rigidBlas` (model space, no baked transform) or `dynamicBlas` (vertices rewritten by
 * `skinning.comp`), both of which are placed by a TLAS instance instead.
 */
struct SceneAccelStruct {
    GpuAccelStruct staticBlas;
    /// One per deformed instance, in `dynamicSlot` order. Built with ALLOW_UPDATE.
    std::vector<GpuAccelStruct> dynamicBlas;
    /// Which instance slot each dynamic BLAS belongs to.
    std::vector<uint32_t> dynamicSlot;
    /// One per *primitive* that some rigid mover draws, shared by every mover drawing it.
    std::vector<GpuAccelStruct> rigidBlas;
    /// Which instance slot each rigid TLAS instance belongs to, in TLAS order after the
    /// deformed ones. Longer than `rigidBlas` whenever movers share a primitive, which is
    /// why the two lists cannot be indexed by each other.
    std::vector<uint32_t> rigidSlot;
    GpuAccelStruct tlas;

    /// What the per-frame refit needs to rebuild one dynamic BLAS's geometry description.
    /// Numbers rather than a cached `VkAccelerationStructureGeometryKHR`: that struct
    /// holds a pointer chain, and a stored one whose `pGeometries` outlives its vector is
    /// how this goes wrong.
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
    /// One `GpuHitRecord` per BLAS geometry, static tier first. Read by device address
    /// rather than bound as a descriptor, so it carries SHADER_DEVICE_ADDRESS.
    GpuBuffer hitRecords;
    VkDeviceAddress hitRecordAddress = 0;
    /// The scene's own vertex and index buffers, by address, so a shader can reach the
    /// static tier's geometry.
    VkDeviceAddress sceneVertexAddress = 0;
    VkDeviceAddress sceneIndexAddress = 0;
    /// Per-geometry transforms baked into `staticBlas`; the build reads it by address, so
    /// it must outlive the build.
    GpuBuffer staticTransforms;
    /// Which instance slot each static geometry was baked from, and the transform baked
    /// for it. Host-side, and read only by `staticTierStale`.
    std::vector<uint32_t> staticSlot;
    std::vector<glm::mat4> staticBaked;
    /// `1 + dynamicBlas.size() + rigidSlot.size()` TLAS instances, host-visible and
    /// rewritten per frame.
    GpuBuffer instanceBuffer;
    /// Scratch for the per-frame refit and TLAS rebuild, allocated once at the size the
    /// worst frame needs -- allocating inside a frame is what this exists to avoid.
    GpuBuffer refitScratch;
    VkDeviceAddress refitScratchAddress = 0;
    VkDeviceSize blasUpdateScratchStride = 0;
    VkDeviceAddress tlasScratchAddress = 0;

    [[nodiscard]] bool valid() const { return tlas.handle != VK_NULL_HANDLE; }
    /// Whether the per-frame refit has work to do. Not `!dynamicBlas.empty()`: a scene of
    /// rigid movers refits no BLAS and still needs its TLAS rebuilt.
    [[nodiscard]] bool hasDynamic() const { return !dynamicBlas.empty() || !rigidSlot.empty(); }
};

/**
 * @brief Build the whole structure for `instances`, once, at load.
 *
 * `deformedVertices` and `deformedBase` describe the buffer `skinning.comp` writes and
 * where each instance slot's vertices start in it; a null buffer and an empty vector
 * build the static tier alone.
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
 *        both tiers that have them, and rebuild the TLAS, into `cmd`.
 *
 * Must be recorded *after* the deformation dispatch and its barrier, and before
 * anything traces.
 */
void refitSceneAccelStruct(const VulkanContext& ctx, VkCommandBuffer cmd, const scene::InstanceTable& instances,
                           SceneAccelStruct& as);

/**
 * @brief Whether any instance baked into the static tier has moved since the build.
 *
 * The static tier has no refit, so a baked instance that moves traces from where it *was*
 * -- a shadow and a reflection with nothing casting them. The usual trigger is ordering,
 * not motion: a node's world transform reaches its instance on the first `Scene::update`,
 * which is after `setInstances` has already built the structure.
 */
[[nodiscard]] bool staticTierStale(const SceneAccelStruct& as, const scene::InstanceTable& instances);

void destroySceneAccelStruct(const VulkanContext& ctx, SceneAccelStruct& as);
void destroyAccelStruct(const VulkanContext& ctx, GpuAccelStruct& as);

/// Device address of a buffer. Needs `bufferDeviceAddress`, which the context enables
/// unconditionally -- VMA wants it anyway.
VkDeviceAddress bufferAddress(const VulkanContext& ctx, VkBuffer buffer);

} // namespace gfx
