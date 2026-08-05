#include "gfx/Resources.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "gfx/VulkanContext.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace gfx {

uint32_t mipLevelsFor(VkExtent2D extent) {
    const uint32_t largest = std::max(extent.width, extent.height);
    return static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(largest)))) + 1;
}

void setObjectName(const VulkanContext& ctx, uint64_t handle, VkObjectType type, const char* name) {
    // volk leaves the pointer unresolved when VK_EXT_debug_utils is not on the instance,
    // which is what lets every caller pass a name unconditionally.
    if (name == nullptr || vkSetDebugUtilsObjectNameEXT == nullptr) return;

    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(ctx.device, &info);
}

GpuBuffer createBuffer(const VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                       VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags flags, const char* name) {
    GpuBuffer buf;
    buf.size = size;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = memoryUsage;
    alloc.flags = flags;

    VmaAllocationInfo allocated{};
    vkCheck(vmaCreateBuffer(ctx.allocator, &info, &alloc, &buf.buffer, &buf.allocation, &allocated),
            "vmaCreateBuffer");
    buf.mapped = allocated.pMappedData;
    setObjectName(ctx, reinterpret_cast<uint64_t>(buf.buffer), VK_OBJECT_TYPE_BUFFER, name);
    return buf;
}

void destroyBuffer(const VulkanContext& ctx, GpuBuffer& buf) {
    if (buf.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx.allocator, buf.buffer, buf.allocation);
        buf = GpuBuffer{};
    }
}

GpuImage createImage(const VulkanContext& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                     uint32_t mipLevels, VkSampleCountFlagBits samples, VkImageAspectFlags aspect,
                     uint32_t arrayLayers, const char* name) {
    GpuImage img;
    img.format = format;
    img.extent = extent;
    img.mipLevels = mipLevels;
    img.arrayLayers = arrayLayers;
    img.samples = samples;

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = mipLevels;
    info.arrayLayers = arrayLayers;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    // Large render targets are worth a dedicated allocation rather than a suballocation.
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        alloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }

    vkCheck(vmaCreateImage(ctx.allocator, &info, &alloc, &img.image, &img.allocation, nullptr), "vmaCreateImage");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = img.image;
    viewInfo.viewType = arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = arrayLayers;

    vkCheck(vkCreateImageView(ctx.device, &viewInfo, nullptr, &img.view), "vkCreateImageView");
    setObjectName(ctx, reinterpret_cast<uint64_t>(img.image), VK_OBJECT_TYPE_IMAGE, name);
    return img;
}

GpuImage createCubeImage(const VulkanContext& ctx, uint32_t size, VkFormat format, VkImageUsageFlags usage,
                         uint32_t mipLevels, const char* name) {
    GpuImage img;
    img.format = format;
    img.extent = {size, size};
    img.mipLevels = mipLevels;
    img.arrayLayers = 6;

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {size, size, 1};
    info.mipLevels = mipLevels;
    info.arrayLayers = 6;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    vkCheck(vmaCreateImage(ctx.allocator, &info, &alloc, &img.image, &img.allocation, nullptr),
            "vmaCreateImage(cube)");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = img.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 6;
    vkCheck(vkCreateImageView(ctx.device, &viewInfo, nullptr, &img.view), "vkCreateImageView(cube)");
    setObjectName(ctx, reinterpret_cast<uint64_t>(img.image), VK_OBJECT_TYPE_IMAGE, name);
    return img;
}

VkImageView createStorageView(const VulkanContext& ctx, const GpuImage& img, uint32_t mipLevel) {
    // 2D_ARRAY even for a cube: `imageStore` addresses a cube's faces as array layers,
    // and a CUBE view is not a legal storage image.
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = img.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    info.format = img.format;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.baseMipLevel = mipLevel;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = img.arrayLayers;

    VkImageView view = VK_NULL_HANDLE;
    vkCheck(vkCreateImageView(ctx.device, &info, nullptr, &view), "vkCreateImageView(storage)");
    return view;
}

VkImageView createLayerView(const VulkanContext& ctx, const GpuImage& img, uint32_t layer,
                            VkImageAspectFlags aspect) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = img.image;
    // 2D, not 2D_ARRAY: a rendering attachment must be a single-layer view.
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = img.format;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.levelCount = img.mipLevels;
    info.subresourceRange.baseArrayLayer = layer;
    info.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    vkCheck(vkCreateImageView(ctx.device, &info, nullptr, &view), "vkCreateImageView(layer)");
    return view;
}

void destroyImage(const VulkanContext& ctx, GpuImage& img) {
    if (img.view != VK_NULL_HANDLE) vkDestroyImageView(ctx.device, img.view, nullptr);
    if (img.image != VK_NULL_HANDLE) vmaDestroyImage(ctx.allocator, img.image, img.allocation);
    img = GpuImage{};
}

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                     VkAccessFlags2 dstAccess, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = baseMip;
    barrier.subresourceRange.levelCount = mipCount;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void bufferBarrier(VkCommandBuffer cmd, std::initializer_list<VkBuffer> buffers, VkPipelineStageFlags2 srcStage,
                   VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                   VkDeviceSize offset, VkDeviceSize size) {
    // Aborts rather than clamping: a barrier covering four of five buffers still
    // validates, still runs, and is wrong only under load on someone else's driver.
    if (buffers.size() > kMaxBufferBarriers) {
        core::Logger::critical(core::LogCategory::Render, "bufferBarrier: %zu buffers, limit is %zu", buffers.size(),
                               kMaxBufferBarriers);
    }

    std::array<VkBufferMemoryBarrier2, kMaxBufferBarriers> barriers{};
    uint32_t count = 0;
    for (VkBuffer buffer : buffers) {
        VkBufferMemoryBarrier2& b = barriers[count++];
        b = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = buffer;
        b.offset = offset;
        b.size = size;
    }

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = count;
    dep.pBufferMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Uploader::init(const VulkanContext& ctx) {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = ctx.graphicsFamily;
    vkCheck(vkCreateCommandPool(ctx.device, &poolInfo, nullptr, &pool), "vkCreateCommandPool(upload)");

    VkCommandBufferAllocateInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(ctx.device, &cbInfo, &cmd), "vkAllocateCommandBuffers(upload)");

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCheck(vkCreateFence(ctx.device, &fenceInfo, nullptr, &fence), "vkCreateFence(upload)");

    poolInfo.queueFamilyIndex = ctx.transferFamily;
    vkCheck(vkCreateCommandPool(ctx.device, &poolInfo, nullptr, &transferPool), "vkCreateCommandPool(transfer)");
    cbInfo.commandPool = transferPool;
    vkCheck(vkAllocateCommandBuffers(ctx.device, &cbInfo, &transferCmd), "vkAllocateCommandBuffers(transfer)");

    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCheck(vkCreateSemaphore(ctx.device, &semInfo, nullptr, &transferDone), "vkCreateSemaphore(transfer)");
}

void Uploader::shutdown(const VulkanContext& ctx) {
    // An `endBatchAsync` nobody reclaimed still owns staging buffers and a fence with work
    // outstanding; destroying them below without this wait frees memory the device is
    // reading.
    if (batchInFlight) {
        vkCheck(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(transfer, shutdown)");
        reclaimBatch(ctx);
    }

    if (transferDone != VK_NULL_HANDLE) vkDestroySemaphore(ctx.device, transferDone, nullptr);
    if (transferPool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx.device, transferPool, nullptr);
    if (fence != VK_NULL_HANDLE) vkDestroyFence(ctx.device, fence, nullptr);
    if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx.device, pool, nullptr);
    fence = VK_NULL_HANDLE;
    pool = VK_NULL_HANDLE;
    cmd = VK_NULL_HANDLE;
    transferDone = VK_NULL_HANDLE;
    transferPool = VK_NULL_HANDLE;
    transferCmd = VK_NULL_HANDLE;
}

void Uploader::begin(const VulkanContext& ctx) {
    vkCheck(vkResetFences(ctx.device, 1, &fence), "vkResetFences(upload)");
    vkCheck(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer(upload)");

    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(cmd, &info), "vkBeginCommandBuffer(upload)");
}

void Uploader::submitAndWait(const VulkanContext& ctx) {
    // The zone covers the submit *and* the fence wait: this is where the CPU stops during
    // a load, and splitting them would attribute the stall to neither.
    auto zone = core::Profiler::scope("Uploader::submitAndWait");
    vkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(upload)");

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;

    vkCheck(vkQueueSubmit2(ctx.graphicsQueue, 1, &submit, fence), "vkQueueSubmit2(upload)");
    vkCheck(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(upload)");
}

void Uploader::uploadBuffer(const VulkanContext& ctx, GpuBuffer& dst, const void* data, VkDeviceSize size) {
    uploadBufferAt(ctx, dst, 0, data, size);
}

void Uploader::uploadBufferAt(const VulkanContext& ctx, GpuBuffer& dst, VkDeviceSize offset, const void* data,
                              VkDeviceSize size) {
    if (size == 0) return;

    GpuBuffer staging = createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                         VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(staging.mapped, data, size);

    begin(ctx);
    VkBufferCopy copy{0, offset, size};
    vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &copy);
    submitAndWait(ctx);

    destroyBuffer(ctx, staging);
}

void Uploader::copyBuffer(const VulkanContext& ctx, GpuBuffer& dst, const GpuBuffer& src, VkDeviceSize size) {
    if (size == 0) return;
    begin(ctx);
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, src.buffer, dst.buffer, 1, &copy);
    submitAndWait(ctx);
}

void Uploader::uploadImageWithMips(const VulkanContext& ctx, GpuImage& dst, const void* pixels, VkDeviceSize size) {
    GpuBuffer staging = createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                         VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(staging.mapped, pixels, size);

    begin(ctx);
    recordImageWithMips(dst, staging.buffer);
    submitAndWait(ctx);

    destroyBuffer(ctx, staging);
}

void Uploader::beginBatch(const VulkanContext& ctx) {
    // Both buffers are begun, because a command buffer may only be submitted to a queue of
    // the family its *pool* was created for: `addImageWithMips` needs `vkCmdBlitImage`,
    // which a transfer-only family cannot record, so it goes on the graphics buffer and no
    // amount of routing can send the transfer one to the graphics queue instead.
    begin(ctx); // the graphics buffer, and the fence both submits share

    vkCheck(vkResetCommandBuffer(transferCmd, 0), "vkResetCommandBuffer(transfer)");
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(transferCmd, &info), "vkBeginCommandBuffer(transfer)");

    // Everything recordImage* emits goes to the transfer buffer for the duration.
    std::swap(cmd, transferCmd);
    batching = true;
}

void Uploader::submitBatch(const VulkanContext& ctx) {
    std::swap(cmd, transferCmd); // restore; `transferCmd` is the one holding the batch
    vkCheck(vkEndCommandBuffer(transferCmd), "vkEndCommandBuffer(transfer)");

    VkCommandBufferSubmitInfo transferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    transferInfo.commandBuffer = transferCmd;

    VkSubmitInfo2 transferSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    transferSubmit.commandBufferInfoCount = 1;
    transferSubmit.pCommandBufferInfos = &transferInfo;

    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = transferDone;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    transferSubmit.signalSemaphoreInfoCount = 1;
    transferSubmit.pSignalSemaphoreInfos = &signal;

    vkCheck(vkQueueSubmit2(ctx.transferQueue, 1, &transferSubmit, VK_NULL_HANDLE), "vkQueueSubmit2(transfer)");

    // The graphics half runs either way: across families these barriers are the acquire,
    // and their family indices *and* layouts must mirror `releaseToGraphics` exactly --
    // one barrier recorded twice on two queues, where a mismatch is undefined behaviour
    // rather than a validation error. Within one family what is left is the visibility
    // barrier the transfer queue could not record.
    const bool crossFamily = ctx.transferFamily != ctx.graphicsFamily;
    std::vector<VkImageMemoryBarrier2> acquires;
    acquires.reserve(batchImages.size());
    for (VkImage image : batchImages) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = crossFamily ? ctx.transferFamily : VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = crossFamily ? ctx.graphicsFamily : VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                    VK_REMAINING_ARRAY_LAYERS};
        acquires.push_back(barrier);
    }
    if (!acquires.empty()) {
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(acquires.size());
        dep.pImageMemoryBarriers = acquires.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    vkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(acquire)");

    VkCommandBufferSubmitInfo acquireInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    acquireInfo.commandBuffer = cmd;

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = transferDone;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 acquireSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    acquireSubmit.waitSemaphoreInfoCount = 1;
    acquireSubmit.pWaitSemaphoreInfos = &wait;
    acquireSubmit.commandBufferInfoCount = 1;
    acquireSubmit.pCommandBufferInfos = &acquireInfo;
    vkCheck(vkQueueSubmit2(ctx.graphicsQueue, 1, &acquireSubmit, fence), "vkQueueSubmit2(acquire)");
}

void Uploader::addImageWithMips(const VulkanContext& ctx, GpuImage& dst, const void* pixels, VkDeviceSize size) {
    GpuBuffer staging = createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                         VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(staging.mapped, pixels, size);

    // Onto the graphics buffer, not the batch's: during a batch `cmd` is the transfer
    // buffer, and a transfer-only queue can record neither `vkCmdBlitImage` nor the
    // fragment-stage barrier that follows it. Hence the swap around the record.
    //
    // Not pushed to `batchImages`: that list drives the acquire barriers, and an acquire
    // naming the transfer family would claim a release this image never made.
    if (batching) std::swap(cmd, transferCmd);
    recordImageWithMips(dst, staging.buffer);
    if (batching) std::swap(cmd, transferCmd);
    batchStaging.push_back(staging);
}

void Uploader::addImageLevels(const VulkanContext& ctx, GpuImage& dst, const void* data, VkDeviceSize size,
                              const std::vector<std::pair<uint64_t, uint64_t>>& levels) {
    GpuBuffer staging = createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                         VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(staging.mapped, data, size);
    recordImageLevels(dst, staging.buffer, levels);
    releaseToGraphics(ctx, dst);
    batchStaging.push_back(staging);
    batchImages.push_back(dst.image);
}

void Uploader::endBatch(const VulkanContext& ctx) {
    if (!endBatchAsync(ctx)) return;
    vkCheck(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(transfer)");
    reclaimBatch(ctx);
}

bool Uploader::endBatchAsync(const VulkanContext& ctx) {
    if (!batching) return false;
    batching = false;
    submitBatch(ctx);
    batchInFlight = true;
    return true;
}

bool Uploader::batchComplete(const VulkanContext& ctx) const {
    if (!batchInFlight) return true;
    return vkGetFenceStatus(ctx.device, fence) == VK_SUCCESS;
}

void Uploader::reclaimBatch(const VulkanContext& ctx) {
    if (!batchInFlight) return;
    batchInFlight = false;
    for (auto& staging : batchStaging) destroyBuffer(ctx, staging);
    batchStaging.clear();
    batchImages.clear();
}

VkCommandBuffer Uploader::beginImmediate(const VulkanContext& ctx) {
    begin(ctx);
    return cmd;
}

void Uploader::endImmediate(const VulkanContext& ctx) { submitAndWait(ctx); }

void Uploader::releaseToGraphics(const VulkanContext& ctx, const GpuImage& dst) {
    if (ctx.transferFamily == ctx.graphicsFamily) return;

    // dstStageMask NONE and dstAccessMask 0 by rule: the acquire is what makes the data
    // visible, and naming a destination stage the transfer queue does not support here is
    // invalid.
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = ctx.transferFamily;
    barrier.dstQueueFamilyIndex = ctx.graphicsFamily;
    barrier.image = dst.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                VK_REMAINING_ARRAY_LAYERS};

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Uploader::recordImageLevels(GpuImage& dst, VkBuffer staging,
                                 const std::vector<std::pair<uint64_t, uint64_t>>& levels) {
    transitionImage(cmd, dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);

    std::vector<VkBufferImageCopy> regions;
    regions.reserve(levels.size());
    for (uint32_t level = 0; level < levels.size() && level < dst.mipLevels; ++level) {
        VkBufferImageCopy region{};
        region.bufferOffset = levels[level].first;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        // Texels, not blocks, even for a compressed format -- Vulkan does the block
        // arithmetic. Block counts here upload a quarter of each level and read past the
        // end of the staging buffer for the rest.
        region.imageExtent = {std::max(1u, dst.extent.width >> level), std::max(1u, dst.extent.height >> level), 1};
        regions.push_back(region);
    }
    if (!regions.empty()) {
        vkCmdCopyBufferToImage(cmd, staging, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());
    }

    // dstStage NONE, not FRAGMENT_SHADER: this may be recorded on a transfer-only queue,
    // where naming a graphics stage is invalid. `submitBatch`'s acquire barrier is what
    // makes the write visible to the fragment stage.
    transitionImage(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
}

void Uploader::recordImageWithMips(GpuImage& dst, VkBuffer staging) {
    // COPY *and* BLIT: this covers every level, but only level 0 is written by the copy
    // below -- the rest are first written by `vkCmdBlitImage`. COPY alone leaves every
    // blit destination unsynchronised against the transition that made it writable, which
    // sync validation reports as WRITE_AFTER_WRITE with `SYNC_COPY_TRANSFER_WRITE`.
    transitionImage(cmd, dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {dst.extent.width, dst.extent.height, 1};
    vkCmdCopyBufferToImage(cmd, staging, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Each level must be read-ready before it is the next one's source, so the barriers
    // walk down the chain one level at a time rather than covering the range at once.
    int32_t mipWidth = static_cast<int32_t>(dst.extent.width);
    int32_t mipHeight = static_cast<int32_t>(dst.extent.height);

    for (uint32_t level = 1; level < dst.mipLevels; ++level) {
        // Both stages on the source side too: level 0 arrived by copy and every level
        // after it by blit, so naming COPY alone waits, from level two down, on something
        // that never happened.
        transitionImage(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                        level - 1, 1);

        const int32_t nextWidth = mipWidth > 1 ? mipWidth / 2 : 1;
        const int32_t nextHeight = mipHeight > 1 ? mipHeight / 2 : 1;

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {nextWidth, nextHeight, 1};

        vkCmdBlitImage(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        transitionImage(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1);

        mipWidth = nextWidth;
        mipHeight = nextHeight;
    }

    // The last level was only ever a blit destination.
    transitionImage(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, dst.mipLevels - 1, 1);
}

} // namespace gfx
