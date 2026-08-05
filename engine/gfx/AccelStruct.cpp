#include "gfx/AccelStruct.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "gfx/VulkanContext.h"
#include "scene/InstanceTable.h"

#include <cstring>
#include <vector>

namespace gfx {

namespace {

/// glm is column-major and VkTransformMatrixKHR is row-major 3x4, so this is a
/// transpose-and-drop-the-last-row, not a memcpy. Getting it wrong produces a scene
/// that traces against a sheared copy of itself, which looks like a bias problem.
VkTransformMatrixKHR toVkTransform(const glm::mat4& m) {
    VkTransformMatrixKHR out{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t col = 0; col < 4; ++col) out.matrix[row][col] = m[col][row];
    }
    return out;
}

void createStructure(const VulkanContext& ctx, GpuAccelStruct& as, VkDeviceSize size,
                     VkAccelerationStructureTypeKHR type) {
    as.buffer = createBuffer(ctx, size,
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

    VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    info.buffer = as.buffer.buffer;
    info.size = size;
    info.type = type;
    vkCheck(vkCreateAccelerationStructureKHR(ctx.device, &info, nullptr, &as.handle),
            "vkCreateAccelerationStructureKHR");

    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addrInfo.accelerationStructure = as.handle;
    as.address = vkGetAccelerationStructureDeviceAddressKHR(ctx.device, &addrInfo);
}

/// Scratch for a build. The caller frees it, so a build using it must have completed --
/// which is why every load-time build here goes through a blocking immediate submit.
GpuBuffer createScratch(const VulkanContext& ctx, VkDeviceSize size, VkDeviceAddress& address,
                        const char* name = nullptr) {
    GpuBuffer scratch = createBuffer(
        ctx, size + ctx.asScratchAlignment,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, name);

    // The address the build reads from must be aligned, and the *allocation* is only
    // aligned to whatever VMA chose -- hence the padding above and the round-up here.
    const VkDeviceAddress base = bufferAddress(ctx, scratch.buffer);
    const VkDeviceAddress align = ctx.asScratchAlignment;
    address = (base + align - 1) & ~(align - 1);
    return scratch;
}

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) & ~(a - 1); }

VkAccelerationStructureGeometryKHR triangleGeometry(VkDeviceAddress vertices, VkDeviceSize stride,
                                                    uint32_t maxVertex, VkDeviceAddress indices,
                                                    VkDeviceAddress transforms, bool opaque = true) {
    VkAccelerationStructureGeometryTrianglesDataKHR tri{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = vertices;
    tri.vertexStride = stride;
    tri.maxVertex = maxVertex;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = indices;
    tri.transformData.deviceAddress = transforms;

    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.geometry.triangles = tri;
    // Clearing the bit is what lets a light escape the emissive mesh representing it:
    // `rayshadow.glsl` traces without `gl_RayFlagsOpaqueEXT` and never confirms a
    // candidate, so a non-opaque triangle occludes nothing, while reflection rays force
    // opacity per ray and still see it. Marking emissive geometry opaque here puts every
    // emissive mesh back in its own shadow.
    geometry.flags = opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
    return geometry;
}

} // namespace

VkDeviceAddress bufferAddress(const VulkanContext& ctx, VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(ctx.device, &info);
}

void destroyAccelStruct(const VulkanContext& ctx, GpuAccelStruct& as) {
    if (as.handle != VK_NULL_HANDLE) vkDestroyAccelerationStructureKHR(ctx.device, as.handle, nullptr);
    as.handle = VK_NULL_HANDLE;
    as.address = 0;
    destroyBuffer(ctx, as.buffer);
}

void destroySceneAccelStruct(const VulkanContext& ctx, SceneAccelStruct& as) {
    destroyAccelStruct(ctx, as.tlas);
    for (GpuAccelStruct& b : as.dynamicBlas) destroyAccelStruct(ctx, b);
    as.dynamicBlas.clear();
    as.dynamicSlot.clear();
    for (GpuAccelStruct& b : as.rigidBlas) destroyAccelStruct(ctx, b);
    as.rigidBlas.clear();
    as.rigidSlot.clear();
    destroyAccelStruct(ctx, as.staticBlas);
    destroyBuffer(ctx, as.dynamicIndices);
    destroyBuffer(ctx, as.hitRecords);
    as.hitRecordAddress = 0;
    as.sceneVertexAddress = 0;
    as.sceneIndexAddress = 0;
    destroyBuffer(ctx, as.staticTransforms);
    as.staticSlot.clear();
    as.staticBaked.clear();
    destroyBuffer(ctx, as.instanceBuffer);
    destroyBuffer(ctx, as.refitScratch);
    as.refitScratchAddress = 0;
    as.tlasScratchAddress = 0;
    as.blasUpdateScratchStride = 0;
    as.dynamicGeometry.clear();
}

void buildSceneAccelStruct(const VulkanContext& ctx, Uploader& uploader, VkBuffer vertexBuffer,
                           VkDeviceSize vertexStride, uint32_t vertexCount, VkBuffer indexBuffer,
                           const std::vector<uint32_t>& sceneIndices, VkBuffer deformedVertices,
                           uint32_t deformedVertexCount, const std::vector<uint32_t>& deformedBase,
                           const scene::InstanceTable& instances, const std::vector<uint8_t>& emissiveMaterials,
                           SceneAccelStruct& out) {
    auto zone = core::Profiler::scope("buildSceneAccelStruct");
    // A geometry index is not an instance slot -- the table holds holes -- which is why
    // every consumer downstream goes through these lists rather than indexing `instances`.
    std::vector<uint32_t> staticSlots;
    std::vector<uint32_t> dynamicSlots;
    std::vector<uint32_t> rigidSlots;
    for (uint32_t s = 0; s < instances.slotCount(); ++s) {
        if ((instances.slot(s).meta.z & scene::kInstanceLive) == 0u) continue;
        if (instances.drawRanges()[s].indexCount == 0) continue;
        // Must match `buildCommands`, which skips `kInstanceBlended` when it fills the
        // shadow cascade. Admitting blended surfaces here makes them cast a shadow under
        // ray queries and none with `--no-ray-query` -- the two paths disagreeing about
        // what is in the scene, not about how it is shaded.
        if ((instances.slot(s).meta.z & scene::kInstanceBlended) != 0u) continue;

        // Without a deformed vertex range and an index array to rebase there is nothing
        // for the dynamic tier to trace, so the instance falls back to static and traces
        // against its bind pose.
        const bool deformable = deformedVertices != VK_NULL_HANDLE && !sceneIndices.empty() &&
                                s < deformedBase.size() && deformedBase[s] != UINT32_MAX;
        switch (scene::accelTier(instances.slot(s).meta.z, deformable)) {
        case scene::AccelTier::Deformed: dynamicSlots.push_back(s); break;
        case scene::AccelTier::Rigid:    rigidSlots.push_back(s);   break;
        case scene::AccelTier::Static:   staticSlots.push_back(s);  break;
        }
    }
    if (staticSlots.empty() && dynamicSlots.empty() && rigidSlots.empty()) return;

    const VkDeviceAddress vertexAddress = bufferAddress(ctx, vertexBuffer);
    const VkDeviceAddress indexAddress = bufferAddress(ctx, indexBuffer);

    VkAccelerationStructureBuildGeometryInfoKHR staticBuild{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<uint32_t> triangleCounts;
    std::vector<VkTransformMatrixKHR> transforms;
    GpuBuffer staticScratch;
    VkDeviceAddress staticScratchAddress = 0;
    VkAccelerationStructureBuildSizesInfoKHR staticSizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};

    if (!staticSlots.empty()) {
        transforms.reserve(staticSlots.size());
        for (uint32_t s : staticSlots) transforms.push_back(toVkTransform(instances.slot(s).model));

        // The same transforms again for `staticTierStale`, kept as glm rather than read
        // back from the row-major 3x4: the comparison is against `slot(s).model`, and a
        // transpose in the middle of it is a place to get the sense wrong for no gain.
        out.staticSlot = staticSlots;
        out.staticBaked.clear();
        out.staticBaked.reserve(staticSlots.size());
        for (uint32_t s : staticSlots) out.staticBaked.push_back(instances.slot(s).model);

        out.staticTransforms =
            createBuffer(ctx, transforms.size() * sizeof(VkTransformMatrixKHR),
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "asStaticTransforms");
        uploader.uploadBuffer(ctx, out.staticTransforms, transforms.data(),
                              transforms.size() * sizeof(VkTransformMatrixKHR));
        const VkDeviceAddress transformAddress = bufferAddress(ctx, out.staticTransforms.buffer);

        geometries.resize(staticSlots.size());
        ranges.resize(staticSlots.size());
        triangleCounts.resize(staticSlots.size());

        for (size_t i = 0; i < staticSlots.size(); ++i) {
            const scene::InstanceTable::DrawRange& prim = instances.drawRanges()[staticSlots[i]];
            const uint32_t material = instances.slot(staticSlots[i]).meta.y;
            const bool emissive = material < emissiveMaterials.size() && emissiveMaterials[material] != 0u;
            geometries[i] = triangleGeometry(vertexAddress, vertexStride, vertexCount - 1, indexAddress,
                                             transformAddress, !emissive);

            triangleCounts[i] = prim.indexCount / 3;
            ranges[i].primitiveCount = triangleCounts[i];
            // Byte offset for indices, element offset for the transform. The two units
            // differ and the validation layers will not catch a mix-up, because both are
            // legal numbers.
            ranges[i].primitiveOffset = prim.firstIndex * static_cast<uint32_t>(sizeof(uint32_t));
            ranges[i].firstVertex = 0;
            ranges[i].transformOffset = static_cast<uint32_t>(i * sizeof(VkTransformMatrixKHR));
        }

        staticBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        staticBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        staticBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        staticBuild.geometryCount = static_cast<uint32_t>(geometries.size());
        staticBuild.pGeometries = geometries.data();

        vkGetAccelerationStructureBuildSizesKHR(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &staticBuild, triangleCounts.data(), &staticSizes);
        createStructure(ctx, out.staticBlas, staticSizes.accelerationStructureSize,
                        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        staticScratch = createScratch(ctx, staticSizes.buildScratchSize, staticScratchAddress);
        staticBuild.dstAccelerationStructure = out.staticBlas.handle;
        staticBuild.scratchData.deviceAddress = staticScratchAddress;
    }

    // A build range has only an added `firstVertex`, never the signed `vertexOffset` the
    // indirect draw path uses, so indices onto the deformed vertex buffer have to be
    // rebased once here rather than corrected per frame.
    std::vector<VkAccelerationStructureGeometryKHR> dynGeometry(dynamicSlots.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> dynBuild(dynamicSlots.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> dynRange(dynamicSlots.size());
    std::vector<GpuBuffer> dynScratch;
    VkDeviceSize maxUpdateScratch = 0;

    out.dynamicGeometry.assign(dynamicSlots.size(), SceneAccelStruct::DynamicGeometry{});

    if (!dynamicSlots.empty()) {
        std::vector<uint32_t> rebased;
        rebased.reserve(dynamicSlots.size() * 1024);
        for (size_t i = 0; i < dynamicSlots.size(); ++i) {
            const uint32_t slot = dynamicSlots[i];
            const scene::InstanceTable::DrawRange& prim = instances.drawRanges()[slot];
            const int64_t shift =
                static_cast<int64_t>(deformedBase[slot]) - static_cast<int64_t>(prim.baseVertex);

            out.dynamicGeometry[i].indexByteOffset = static_cast<uint32_t>(rebased.size() * sizeof(uint32_t));
            out.dynamicGeometry[i].triangleCount = prim.indexCount / 3;
            for (uint32_t k = 0; k < prim.indexCount; ++k) {
                const uint32_t source = sceneIndices[prim.firstIndex + k];
                rebased.push_back(static_cast<uint32_t>(static_cast<int64_t>(source) + shift));
            }
        }

        out.dynamicIndices = createBuffer(ctx, rebased.size() * sizeof(uint32_t),
                                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "asDynamicIndices");
        uploader.uploadBuffer(ctx, out.dynamicIndices, rebased.data(), rebased.size() * sizeof(uint32_t));

        out.deformedVertexAddress = bufferAddress(ctx, deformedVertices);
        out.dynamicIndexAddress = bufferAddress(ctx, out.dynamicIndices.buffer);
        out.vertexStride = vertexStride;
        out.deformedMaxVertex = deformedVertexCount - 1;

        out.dynamicBlas.resize(dynamicSlots.size());
        out.dynamicSlot = dynamicSlots;
        dynScratch.resize(dynamicSlots.size());

        for (size_t i = 0; i < dynamicSlots.size(); ++i) {
            // No baked transform: the deformed vertices are model space and the TLAS
            // instance carries where the object is. Baking one here freezes it in place.
            dynGeometry[i] = triangleGeometry(out.deformedVertexAddress, out.vertexStride,
                                              out.deformedMaxVertex, out.dynamicIndexAddress, 0);

            dynBuild[i] = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            dynBuild[i].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            // Must match the flags `refitSceneAccelStruct` builds with: a structure
            // rebuilt under flags other than the ones it was created with is undefined,
            // not merely slower. Preferring fast build here costs every shadow ray that
            // enters a character far more than the per-frame build saves.
            dynBuild[i].flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                                VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            dynBuild[i].mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            dynBuild[i].geometryCount = 1;
            dynBuild[i].pGeometries = &dynGeometry[i];

            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                    &dynBuild[i], &out.dynamicGeometry[i].triangleCount, &sizes);

            createStructure(ctx, out.dynamicBlas[i], sizes.accelerationStructureSize,
                            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);

            VkDeviceAddress scratchAddress = 0;
            dynScratch[i] = createScratch(ctx, sizes.buildScratchSize, scratchAddress);
            dynBuild[i].dstAccelerationStructure = out.dynamicBlas[i].handle;
            dynBuild[i].scratchData.deviceAddress = scratchAddress;

            // Build scratch, not update scratch: the per-frame pass rebuilds. Sizing this
            // from `updateScratchSize` while issuing a build is a buffer overrun the
            // validation layers do not catch.
            maxUpdateScratch = std::max(maxUpdateScratch, sizes.buildScratchSize);

            dynRange[i] = {};
            dynRange[i].primitiveCount = out.dynamicGeometry[i].triangleCount;
            dynRange[i].primitiveOffset = out.dynamicGeometry[i].indexByteOffset;
        }
    }

    // One structure per distinct primitive, shared by every mover drawing it. Opacity is
    // part of the key because it is baked into the geometry rather than the TLAS
    // instance, so two movers of one mesh with different materials cannot share a BLAS.
    struct RigidKey {
        uint32_t firstIndex;
        uint32_t indexCount;
        bool opaque;
    };
    std::vector<RigidKey> rigidKeys;
    std::vector<uint32_t> rigidBlasOf(rigidSlots.size(), 0);
    std::vector<VkAccelerationStructureGeometryKHR> rigidGeometry;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> rigidBuild;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> rigidRange;
    std::vector<GpuBuffer> rigidScratch;

    for (size_t i = 0; i < rigidSlots.size(); ++i) {
        const scene::InstanceTable::DrawRange& prim = instances.drawRanges()[rigidSlots[i]];
        const uint32_t material = instances.slot(rigidSlots[i]).meta.y;
        const bool emissive = material < emissiveMaterials.size() && emissiveMaterials[material] != 0u;
        const RigidKey key{prim.firstIndex, prim.indexCount, !emissive};

        size_t found = 0;
        for (; found < rigidKeys.size(); ++found) {
            if (rigidKeys[found].firstIndex == key.firstIndex && rigidKeys[found].indexCount == key.indexCount &&
                rigidKeys[found].opaque == key.opaque) {
                break;
            }
        }
        if (found == rigidKeys.size()) {
            rigidKeys.push_back(key);
            // Unrebased, with `firstVertex` 0, so the indices still address the scene's
            // own vertex buffer and `GpuHitRecord::deformed` stays 0 for this tier.
            rigidGeometry.push_back(
                triangleGeometry(vertexAddress, vertexStride, vertexCount - 1, indexAddress, 0, key.opaque));
            rigidBuild.push_back({VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR});
            VkAccelerationStructureBuildRangeInfoKHR range{};
            range.primitiveCount = key.indexCount / 3;
            range.primitiveOffset = key.firstIndex * static_cast<uint32_t>(sizeof(uint32_t));
            rigidRange.push_back(range);
        }
        rigidBlasOf[i] = static_cast<uint32_t>(found);
    }

    if (!rigidKeys.empty()) {
        out.rigidBlas.resize(rigidKeys.size());
        rigidScratch.resize(rigidKeys.size());

        for (size_t i = 0; i < rigidKeys.size(); ++i) {
            rigidBuild[i].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            rigidBuild[i].flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            rigidBuild[i].mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            rigidBuild[i].geometryCount = 1;
            rigidBuild[i].pGeometries = &rigidGeometry[i];

            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                    &rigidBuild[i], &rigidRange[i].primitiveCount, &sizes);

            createStructure(ctx, out.rigidBlas[i], sizes.accelerationStructureSize,
                            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);

            VkDeviceAddress scratchAddress = 0;
            rigidScratch[i] = createScratch(ctx, sizes.buildScratchSize, scratchAddress);
            rigidBuild[i].dstAccelerationStructure = out.rigidBlas[i].handle;
            rigidBuild[i].scratchData.deviceAddress = scratchAddress;
        }
    }
    out.rigidSlot = rigidSlots;

    // Static, then deformed, then rigid. `GpuHitRecord`'s indexing scheme is this order;
    // emitting the tiers in any other shifts every shader's lookup.
    {
        std::vector<GpuHitRecord> records;
        records.reserve(staticSlots.size() + dynamicSlots.size() + rigidSlots.size());

        for (uint32_t slot : staticSlots) {
            records.push_back({instances.drawRanges()[slot].firstIndex, slot, 0u, 0u});
        }
        for (size_t i = 0; i < dynamicSlots.size(); ++i) {
            records.push_back({out.dynamicGeometry[i].indexByteOffset / static_cast<uint32_t>(sizeof(uint32_t)),
                               dynamicSlots[i], 1u, 0u});
        }
        // One per rigid *instance*, not per rigid BLAS: the record is reached through
        // `instanceCustomIndex`, so movers sharing a structure still resolve to their own
        // slot and material.
        for (uint32_t slot : rigidSlots) {
            records.push_back({instances.drawRanges()[slot].firstIndex, slot, 0u, 0u});
        }

        if (!records.empty()) {
            out.hitRecords = createBuffer(ctx, records.size() * sizeof(GpuHitRecord),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "asHitRecords");
            uploader.uploadBuffer(ctx, out.hitRecords, records.data(), records.size() * sizeof(GpuHitRecord));
            out.hitRecordAddress = bufferAddress(ctx, out.hitRecords.buffer);
        }

        out.sceneVertexAddress = vertexAddress;
        out.sceneIndexAddress = indexAddress;
    }

    // The static BLAS's instance is identity because its transforms are already baked in;
    // giving it one here applies the transform twice.
    std::vector<VkAccelerationStructureInstanceKHR> tlasInstances;
    if (out.staticBlas.handle != VK_NULL_HANDLE) {
        VkAccelerationStructureInstanceKHR inst{};
        inst.transform = toVkTransform(glm::mat4(1.0f));
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = out.staticBlas.address;
        tlasInstances.push_back(inst);
    }
    for (size_t i = 0; i < out.dynamicBlas.size(); ++i) {
        VkAccelerationStructureInstanceKHR inst{};
        inst.transform = toVkTransform(instances.slot(out.dynamicSlot[i]).model);
        // The record index, not the instance slot -- a dynamic BLAS's geometry index is
        // always 0, so this carries the whole offset past the static tier. 24 bits, which
        // caps the scene at 16.7M BLAS geometries.
        inst.instanceCustomIndex = static_cast<uint32_t>(staticSlots.size() + i);
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = out.dynamicBlas[i].address;
        tlasInstances.push_back(inst);
    }
    for (size_t i = 0; i < rigidSlots.size(); ++i) {
        VkAccelerationStructureInstanceKHR inst{};
        inst.transform = toVkTransform(instances.slot(rigidSlots[i]).model);
        inst.instanceCustomIndex = static_cast<uint32_t>(staticSlots.size() + dynamicSlots.size() + i);
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = out.rigidBlas[rigidBlasOf[i]].address;
        tlasInstances.push_back(inst);
    }

    // Host-visible and never staged: `refitSceneAccelStruct` writes straight through
    // `mapped` every frame, so making this device-local breaks the refit path.
    out.instanceBuffer =
        createBuffer(ctx, tlasInstances.size() * sizeof(VkAccelerationStructureInstanceKHR),
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VMA_MEMORY_USAGE_AUTO,
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                     "asInstances");
    std::memcpy(out.instanceBuffer.mapped, tlasInstances.data(),
                tlasInstances.size() * sizeof(VkAccelerationStructureInstanceKHR));

    VkAccelerationStructureGeometryKHR tlasGeometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    tlasGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
    tlasGeometry.geometry.instances.data.deviceAddress = bufferAddress(ctx, out.instanceBuffer.buffer);
    tlasGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuild.geometryCount = 1;
    tlasBuild.pGeometries = &tlasGeometry;

    const uint32_t instanceCount = static_cast<uint32_t>(tlasInstances.size());
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild,
                                            &instanceCount, &tlasSizes);

    createStructure(ctx, out.tlas, tlasSizes.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);

    VkDeviceAddress tlasScratchAddress = 0;
    GpuBuffer tlasScratch = createScratch(ctx, tlasSizes.buildScratchSize, tlasScratchAddress);
    tlasBuild.dstAccelerationStructure = out.tlas.handle;
    tlasBuild.scratchData.deviceAddress = tlasScratchAddress;

    // Every BLAS, then a barrier, then the TLAS. The TLAS build reads each BLAS by device
    // address, so there is no handle dependency for the validation layers to catch if the
    // barrier is dropped or the order swapped.
    VkCommandBuffer cmd = uploader.beginImmediate(ctx);

    if (out.staticBlas.handle != VK_NULL_HANDLE) {
        VkAccelerationStructureBuildRangeInfoKHR* r = ranges.data();
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &staticBuild, &r);
    }
    for (size_t i = 0; i < dynBuild.size(); ++i) {
        VkAccelerationStructureBuildRangeInfoKHR* r = &dynRange[i];
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &dynBuild[i], &r);
    }
    for (size_t i = 0; i < out.rigidBlas.size(); ++i) {
        VkAccelerationStructureBuildRangeInfoKHR* r = &rigidRange[i];
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &rigidBuild[i], &r);
    }

    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    tlasRange.primitiveCount = instanceCount;
    VkAccelerationStructureBuildRangeInfoKHR* tlasRanges = &tlasRange;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasBuild, &tlasRanges);

    uploader.endImmediate(ctx);

    destroyBuffer(ctx, staticScratch);
    for (GpuBuffer& b : dynScratch) destroyBuffer(ctx, b);
    for (GpuBuffer& b : rigidScratch) destroyBuffer(ctx, b);
    destroyBuffer(ctx, tlasScratch);

    // Allocated once, at the size the worst frame needs. Every dynamic BLAS gets its own
    // region so the per-frame builds need no barrier between them; the TLAS scratch sits
    // past the end of all of them. Gated on `hasDynamic()`, not on the dynamic BLASes:
    // a scene of crates alone still rebuilds its TLAS and would find a null scratch here.
    if (out.hasDynamic()) {
        const VkDeviceSize align = std::max<VkDeviceSize>(ctx.asScratchAlignment, 1);
        out.blasUpdateScratchStride = alignUp(std::max<VkDeviceSize>(maxUpdateScratch, 1), align);
        const VkDeviceSize blasTotal = out.blasUpdateScratchStride * out.dynamicBlas.size();
        const VkDeviceSize total = blasTotal + alignUp(tlasSizes.buildScratchSize, align);

        out.refitScratch = createScratch(ctx, total, out.refitScratchAddress, "asRefitScratch");
        out.tlasScratchAddress = out.refitScratchAddress + blasTotal;
    }

    VkDeviceSize dynamicBytes = 0;
    for (const GpuAccelStruct& b : out.dynamicBlas) dynamicBytes += b.buffer.size;
    VkDeviceSize rigidBytes = 0;
    for (const GpuAccelStruct& b : out.rigidBlas) rigidBytes += b.buffer.size;

    core::Logger::status(core::LogCategory::Render,
                   "Acceleration structures: %zu static geometries (%.1f MiB) + %zu refitted (%.1f MiB) + "
                   "%zu moved over %zu shared structures (%.1f MiB), TLAS %.1f KiB over %u instances",
                   staticSlots.size(),
                   static_cast<double>(staticSizes.accelerationStructureSize) / (1024.0 * 1024.0),
                   dynamicSlots.size(), static_cast<double>(dynamicBytes) / (1024.0 * 1024.0),
                   rigidSlots.size(), out.rigidBlas.size(), static_cast<double>(rigidBytes) / (1024.0 * 1024.0),
                   static_cast<double>(tlasSizes.accelerationStructureSize) / 1024.0, instanceCount);
}

bool staticTierStale(const SceneAccelStruct& as, const scene::InstanceTable& instances) {
    for (size_t i = 0; i < as.staticSlot.size(); ++i) {
        const uint32_t s = as.staticSlot[i];
        if (s >= instances.slotCount()) return true;
        const scene::GpuInstance& live = instances.slot(s);
        if ((live.meta.z & scene::kInstanceLive) == 0u) return true;
        // Deformed fallbacks move every frame by design; including them rebuilds the whole
        // structure once per frame in any scene with a skinned mesh and no deformed buffer.
        if ((live.meta.z & scene::kInstanceDeformed) != 0u) continue;
        // The flag, not only the transform: `initPhysics` sets `kInstanceDynamic` after
        // `setInstances` has already baked those instances in, so a mover that has not
        // fallen yet still has to invalidate the structure.
        if ((live.meta.z & scene::kInstanceDynamic) != 0u) return true;
        if (scene::movedSinceBake(as.staticBaked[i], live.model)) return true;
    }
    return false;
}

void refitSceneAccelStruct(const VulkanContext& ctx, VkCommandBuffer cmd, const scene::InstanceTable& instances,
                           SceneAccelStruct& as) {
    if (!as.hasDynamic() || as.refitScratch.buffer == VK_NULL_HANDLE) return;

    // The instance array is written host-side, so it must be finished before the build
    // that reads it is recorded below.
    auto* tlasInstances = static_cast<VkAccelerationStructureInstanceKHR*>(as.instanceBuffer.mapped);
    const size_t staticFirst = as.staticBlas.handle != VK_NULL_HANDLE ? 1u : 0u;
    for (size_t i = 0; i < as.dynamicSlot.size(); ++i) {
        tlasInstances[staticFirst + i].transform = toVkTransform(instances.slot(as.dynamicSlot[i]).model);
    }
    // The rigid tier follows the deformed one in the same array; the two loops' order
    // here has to match the order `buildSceneAccelStruct` appended them in.
    const size_t rigidFirst = staticFirst + as.dynamicSlot.size();
    for (size_t i = 0; i < as.rigidSlot.size(); ++i) {
        tlasInstances[rigidFirst + i].transform = toVkTransform(instances.slot(as.rigidSlot[i]).model);
    }

    std::vector<VkAccelerationStructureGeometryKHR> geometry(as.dynamicBlas.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> build(as.dynamicBlas.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> range(as.dynamicBlas.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR*> rangePtr(as.dynamicBlas.size());

    for (size_t i = 0; i < as.dynamicBlas.size(); ++i) {
        geometry[i] = triangleGeometry(as.deformedVertexAddress, as.vertexStride, as.deformedMaxVertex,
                                       as.dynamicIndexAddress, 0);

        build[i] = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build[i].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        // The same flags `buildSceneAccelStruct` created these structures with; rebuilding
        // under different ones is undefined, not merely slower.
        build[i].flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                         VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        // Build, not update. A refit keeps the tree built from the *bind* pose, whose
        // nodes overlap far more than one built for the pose on screen -- cheap to record
        // and paid back by every ray that enters the character, which is why the cost
        // lands in `Lighting` rather than in `AsRefit` where it would be noticed.
        build[i].mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build[i].srcAccelerationStructure = VK_NULL_HANDLE;
        build[i].dstAccelerationStructure = as.dynamicBlas[i].handle;
        build[i].geometryCount = 1;
        build[i].pGeometries = &geometry[i];
        build[i].scratchData.deviceAddress = as.refitScratchAddress + as.blasUpdateScratchStride * i;

        range[i] = {};
        range[i].primitiveCount = as.dynamicGeometry[i].triangleCount;
        range[i].primitiveOffset = as.dynamicGeometry[i].indexByteOffset;
        rangePtr[i] = &range[i];
    }

    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;

    // `infoCount` may not be zero, so a scene with no deformed geometry must skip the call
    // rather than pass an empty array.
    if (!build.empty()) {
        vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<uint32_t>(build.size()), build.data(), rangePtr.data());
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Rebuilt, not refitted: a refitted TLAS degrades under exactly the large translations
    // a character crossing a room produces.
    VkAccelerationStructureGeometryKHR tlasGeometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    tlasGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
    tlasGeometry.geometry.instances.data.deviceAddress = bufferAddress(ctx, as.instanceBuffer.buffer);
    tlasGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuild.geometryCount = 1;
    tlasBuild.pGeometries = &tlasGeometry;
    tlasBuild.dstAccelerationStructure = as.tlas.handle;
    tlasBuild.scratchData.deviceAddress = as.tlasScratchAddress;

    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    tlasRange.primitiveCount = static_cast<uint32_t>(rigidFirst + as.rigidSlot.size());
    VkAccelerationStructureBuildRangeInfoKHR* tlasRanges = &tlasRange;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasBuild, &tlasRanges);

    // Reused: the same `dep` now orders the TLAS write against every shader that traces.
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace gfx
