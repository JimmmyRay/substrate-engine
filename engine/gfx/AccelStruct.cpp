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

/// Allocate the buffer an acceleration structure lives in and create the structure on
/// it. The two are always created together and always destroyed together.
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

/// Scratch for a build. Freed by the caller once the build has completed, which is why
/// every load-time build here is a blocking immediate submit.
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

/// Fill a triangles descriptor. The five fields that differ between the static and
/// dynamic tiers are arguments; the rest are the same in both and stating them twice
/// is how one of them ends up different by accident.
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
    /**
     * Opaque, including the alpha-masked foliage -- see the header for what that costs
     * and why it is not fixed here.
     *
     * Emissive geometry is the exception, and it is how a light escapes the mesh that
     * represents it. A shadow ray traces without `gl_RayFlagsOpaqueEXT`, so an opaque
     * triangle is confirmed by the implementation and terminates the ray, while a
     * non-opaque one is offered to the shader as a *candidate* -- and rayshadow.glsl
     * never confirms one. Emissive geometry therefore occludes nothing.
     *
     * A reflection ray passes `gl_RayFlagsOpaqueEXT`, which forces every triangle opaque
     * for that ray, so the same geometry is still hit and still seen in reflections. The
     * flag is per ray, which is what lets one mesh be invisible to shadows and visible to
     * everything else without a second acceleration structure or a second TLAS instance.
     */
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
    // A BLAS per primitive plus a TLAS, and it is built twice during load.
    auto zone = core::Profiler::scope("buildSceneAccelStruct");
    // Live slots, split by tier. A geometry index is not an instance slot: the table
    // may hold holes, and a BLAS geometry with no triangles is legal but pointless.
    std::vector<uint32_t> staticSlots;
    std::vector<uint32_t> dynamicSlots;
    std::vector<uint32_t> rigidSlots;
    for (uint32_t s = 0; s < instances.slotCount(); ++s) {
        if ((instances.slot(s).meta.z & scene::kInstanceLive) == 0u) continue;
        if (instances.drawRanges()[s].indexCount == 0) continue;
        // **A blended surface occludes nothing, and the raster path already said so.**
        // `buildCommands` skips `kInstanceBlended` when it fills the shadow cascade, so
        // leaving it in here made one surface cast a shadow under ray queries and none
        // without them -- which is not a quality difference between two shadow techniques,
        // it is the two disagreeing about what is there. A 10 m intersection-highlight
        // sphere around a character is the case that showed it: an opaque black disc under
        // ray queries, nothing at all with `--no-ray-query`.
        //
        // It leaves blended surfaces out of traced *reflections* as well, which is the same
        // trade and the same reason: a hit is shaded by `shadeRayHit` from one opaque
        // surface's material, and a translucent one has no single answer to give it.
        if ((instances.slot(s).meta.z & scene::kInstanceBlended) != 0u) continue;

        // A deformed instance only reaches the dynamic tier when there is somewhere for
        // its vertices to have been written and an index array to rebase. Without those
        // it falls back to the static tier and traces against its bind pose, which is
        // wrong but visible -- and is exactly what 3.9 did for every skinned mesh.
        // `accelTier` argues why that fallback is static rather than rigid.
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

    // ------------------------------------------------------- static tier (3.9)
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
        // One VkTransformMatrixKHR per geometry, in one device-local buffer the build
        // reads by address. A geometry's transform is baked into the BLAS, which is what
        // lets a single BLAS hold a whole flattened node hierarchy.
        transforms.reserve(staticSlots.size());
        for (uint32_t s : staticSlots) transforms.push_back(toVkTransform(instances.slot(s).model));

        // The same thing again, host-side and unflattened, for `staticTierStale`. Kept as
        // the glm matrix rather than reading the row-major 3x4 back, because the comparison
        // is against `instances.slot(s).model` and a transpose in the middle of it is a
        // place to get the sense wrong for no gain.
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
        // Trace-time performance over build time: this is built once at load and then
        // read every frame for the rest of the process.
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

    // ------------------------------------------------------ dynamic tier (S2.5)
    // Indices rebased onto the deformed vertex buffer, once. See the header for why the
    // draw path needs no equivalent and this does.
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
            // No baked transform: the deformed vertices are already in model space, and
            // where the object *is* belongs to the TLAS instance. That is the whole
            // reason a moving object needs its own BLAS.
            dynGeometry[i] = triangleGeometry(out.deformedVertexAddress, out.vertexStride,
                                              out.deformedMaxVertex, out.dynamicIndexAddress, 0);

            dynBuild[i] = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            dynBuild[i].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            // Fast *trace* rather than fast build, plus ALLOW_UPDATE. This used to prefer
            // the build, on the reasoning that a structure refitted every frame pays its
            // build cost every frame and that for one character among a hundred thousand
            // static triangles the build is the side that shows up.
            //
            // **That reasoning had a scene in it, and it does not hold when the deformed
            // geometry is most of what a ray can hit.** An arena authoring its collision
            // separately is ~500 visible triangles, so two skinned characters are 99% of
            // the traced scene rather than a rounding error in it -- and a fast-build tree
            // over 56k triangles costs every shadow ray far more than the per-frame build
            // saves. Measured on `battle_arena`: 38.2 ms a frame down to the numbers in
            // this card's outcome, with the build itself moving by a fraction of that.
            //
            // The flags must match the refit's in `refitAccelStructures`, because an update
            // whose source was built under different flags is undefined. `updateScratchSize`
            // below is queried from *these* flags, so the scratch follows automatically.
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

            // **Build scratch, not update scratch**, because the per-frame pass rebuilds
            // rather than refits -- see `refitAccelStructures`. A build needs the larger
            // of the two, and sizing this from `updateScratchSize` while issuing a build
            // is a buffer overrun the validation layers do not catch.
            maxUpdateScratch = std::max(maxUpdateScratch, sizes.buildScratchSize);

            dynRange[i] = {};
            dynRange[i].primitiveCount = out.dynamicGeometry[i].triangleCount;
            dynRange[i].primitiveOffset = out.dynamicGeometry[i].indexByteOffset;
        }
    }

    // -------------------------------------------------------------- rigid tier
    // Movers that do not deform. Their vertices are the scene's own and never change, so
    // there is no rebasing to do and nothing to refit: what they need is a BLAS with *no*
    // baked transform, so the TLAS instance is free to carry one that changes.
    //
    // One structure per distinct primitive, shared by every mover drawing it. The key is
    // the index range plus the opacity, because two instances of one mesh with different
    // materials can disagree about whether a shadow ray passes through them, and that is
    // a property of the geometry rather than of the TLAS instance. A linear scan over the
    // distinct keys, not a map: the list is one entry per *mesh* a game makes movable,
    // which is tens where the mover count is thousands.
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
            // Indices unrebased, exactly as the static tier leaves them: the build reads
            // them at a byte offset with `firstVertex` 0, so they still address the
            // scene's own vertex buffer and the shader can use them unmodified.
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
            // Fast trace and no ALLOW_UPDATE, unlike the deformed tier: this is built
            // once at load and then read every frame for the rest of the process. Moving
            // it costs a TLAS transform, not a build.
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

    // ------------------------------------------------------------- hit records
    // One record per BLAS geometry, static tier first. See GpuHitRecord for why the
    // shader's lookup is `instanceCustomIndex + geometryIndex` and why that needs the
    // three tiers laid out in exactly this order.
    {
        std::vector<GpuHitRecord> records;
        records.reserve(staticSlots.size() + dynamicSlots.size() + rigidSlots.size());

        for (uint32_t slot : staticSlots) {
            // The static tier's indices were never rebased -- the build reads them at a
            // byte offset and firstVertex is 0 -- so they still index the scene's own
            // vertex buffer, and the shader can use them unmodified.
            records.push_back({instances.drawRanges()[slot].firstIndex, slot, 0u, 0u});
        }
        for (size_t i = 0; i < dynamicSlots.size(); ++i) {
            records.push_back({out.dynamicGeometry[i].indexByteOffset / static_cast<uint32_t>(sizeof(uint32_t)),
                               dynamicSlots[i], 1u, 0u});
        }
        // One per rigid *instance* rather than per rigid BLAS, and that is the whole
        // reason sharing a structure between movers costs nothing downstream: the record
        // is found through `instanceCustomIndex`, so two crates over one cube still
        // resolve to their own slot and their own material.
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

    // ---------------------------------------------------------------------- tlas
    // One instance for the static BLAS -- identity, because its transforms are baked --
    // and one per dynamic BLAS carrying that instance's world transform.
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
        // The record index, not the instance slot. `instanceCustomIndex + geometryIndex`
        // is how a shader finds the hit record, and a dynamic BLAS's geometry index is
        // always 0 -- so this field carries the whole offset past the static tier. It is
        // 24 bits, which caps the scene at 16.7M BLAS geometries.
        inst.instanceCustomIndex = static_cast<uint32_t>(staticSlots.size() + i);
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = out.dynamicBlas[i].address;
        tlasInstances.push_back(inst);
    }
    for (size_t i = 0; i < rigidSlots.size(); ++i) {
        VkAccelerationStructureInstanceKHR inst{};
        // The one line the whole tier exists for. `refitSceneAccelStruct` rewrites it
        // every frame from the same field, so a crate the solver pushes traces where it
        // is drawn rather than where it was loaded.
        inst.transform = toVkTransform(instances.slot(rigidSlots[i]).model);
        inst.instanceCustomIndex = static_cast<uint32_t>(staticSlots.size() + dynamicSlots.size() + i);
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = out.rigidBlas[rigidBlasOf[i]].address;
        tlasInstances.push_back(inst);
    }

    // Host-visible and never staged: the transforms are rewritten every frame by
    // refitSceneAccelStruct, and a staging copy per frame to move 64 bytes an instance
    // would cost more than the write it replaces.
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

    // ------------------------------------------------------------------- record
    // Every BLAS then the TLAS, in one submit, with a barrier between: the TLAS build
    // reads each BLAS's device address, so the bottom level has to be complete first.
    // This is the one ordering constraint in the whole file and it is invisible in the
    // API -- there is no handle dependency for the validation layers to notice.
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

    // ----------------------------------------------- persistent refit scratch
    // Allocated once, at the size the worst frame needs, because allocating inside a
    // frame is exactly what this exists to avoid. Every dynamic BLAS gets its own
    // region so the updates need no barrier between them, and the TLAS rebuild's
    // scratch sits past the end of all of them.
    //
    // Gated on `hasDynamic()` rather than on the dynamic BLASes alone: a scene of crates
    // and no characters refits nothing and still rebuilds its TLAS every frame, and
    // without this it would reach that rebuild with a null scratch buffer.
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
        // A slot the game reused for something else is stale whatever its transform says,
        // and a dead one is stale because the structure is still tracing it.
        if ((live.meta.z & scene::kInstanceLive) == 0u) return true;
        // Deformed instances that fell back to this tier are excluded, and they are the
        // reason this is not simply "did any baked transform change". Their transform moves
        // every frame by design -- `accelTier` says why the fallback is Static -- so
        // including them would rebuild the whole structure once per frame in any scene with
        // a skinned mesh and no deformed buffer yet.
        if ((live.meta.z & scene::kInstanceDeformed) != 0u) continue;
        // **The flag, not only the transform, and this is the case that matters most.**
        // `initPhysics` sets `kInstanceDynamic` on everything it gives a body to, and it
        // runs *after* the game's `setInstances` has already baked those instances into the
        // static tier. They then fall, and their traced copies stay at the spawn -- twelve
        // frozen box shadows in `physics.gltf`, and a sphere with no shadow under it at all.
        // Checking the transform alone would miss a mover that has not moved yet.
        if ((live.meta.z & scene::kInstanceDynamic) != 0u) return true;
        if (scene::movedSinceBake(as.staticBaked[i], live.model)) return true;
    }
    return false;
}

void refitSceneAccelStruct(const VulkanContext& ctx, VkCommandBuffer cmd, const scene::InstanceTable& instances,
                           SceneAccelStruct& as) {
    if (!as.hasDynamic() || as.refitScratch.buffer == VK_NULL_HANDLE) return;

    // The TLAS instance array first, because it is a plain memory write and the build
    // that reads it is recorded after. `staticFirst` is the offset the static BLAS's
    // instance occupies when there is one.
    auto* tlasInstances = static_cast<VkAccelerationStructureInstanceKHR*>(as.instanceBuffer.mapped);
    const size_t staticFirst = as.staticBlas.handle != VK_NULL_HANDLE ? 1u : 0u;
    for (size_t i = 0; i < as.dynamicSlot.size(); ++i) {
        tlasInstances[staticFirst + i].transform = toVkTransform(instances.slot(as.dynamicSlot[i]).model);
    }
    // The rigid tier, in the same array and by the same rule. This is the whole of what
    // moving one costs: no refit, no rebuild, one 3x4 written where the previous frame's
    // was -- and it is why a mover must not be baked into `staticBlas`, which has no such
    // line to write.
    const size_t rigidFirst = staticFirst + as.dynamicSlot.size();
    for (size_t i = 0; i < as.rigidSlot.size(); ++i) {
        tlasInstances[rigidFirst + i].transform = toVkTransform(instances.slot(as.rigidSlot[i]).model);
    }

    // ------------------------------------------------------------ BLAS refits
    std::vector<VkAccelerationStructureGeometryKHR> geometry(as.dynamicBlas.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> build(as.dynamicBlas.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> range(as.dynamicBlas.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR*> rangePtr(as.dynamicBlas.size());

    for (size_t i = 0; i < as.dynamicBlas.size(); ++i) {
        geometry[i] = triangleGeometry(as.deformedVertexAddress, as.vertexStride, as.deformedMaxVertex,
                                       as.dynamicIndexAddress, 0);

        build[i] = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build[i].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        // **The same flags the structure was built with**, and an update whose source was
        // built under different ones is undefined rather than merely slower. If the tier's
        // preference changes in `buildAccelStructures`, it changes here in the same edit.
        build[i].flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                         VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        // **Build, not update, and the difference is the whole cost of this tier.**
        //
        // A refit moves the AABBs and keeps the tree. That is correct output and almost
        // free -- 0.11 ms for two characters -- but the tree it keeps is the one built from
        // the *bind* pose, arms out and legs straight. A skinned character spends every
        // frame somewhere else, so the retained hierarchy is one whose nodes overlap far
        // more than a tree built for the pose on screen, and every ray entering it pays
        // that overlap. Measured on `battle_arena`: refitting cost 22 ms of `Lighting`
        // against 0.23 ms for the same scene with no characters in it -- traversal, not
        // building, and the cheapness of the refit was what hid it.
        //
        // A rebuild is the fix rather than a periodic re-build because the pose differs
        // every frame; there is no interval at which the retained tree is fresh. The build
        // shows up in `AsRefit` and is the trade this line makes on purpose.
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

    // Skipped whole when nothing deforms -- a scene of crates has every BLAS it will ever
    // need. `infoCount` may not be zero, and the barrier below orders BLAS writes against
    // the TLAS read, so with no builds recorded there is nothing for it to order.
    if (!build.empty()) {
        vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<uint32_t>(build.size()), build.data(), rangePtr.data());
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // --------------------------------------------------------- TLAS rebuild
    // Rebuilt rather than refitted. A TLAS over a handful of instances costs less to
    // build than the bookkeeping that would decide whether a refit is still valid, and
    // a refitted TLAS degrades in exactly the case a character walking across a room
    // produces -- large translations relative to the instance's own size.
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

    // And once more, for the shaders that trace against the result.
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace gfx
