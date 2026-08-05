#pragma once

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace gfx {

struct VulkanContext;

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

/**
 * @brief Attach a `VK_EXT_debug_utils` name to any Vulkan handle.
 *
 * A no-op when the extension is absent, so no call site needs a guard.
 *
 * `scripts/rdoc/images.py` writes its output files under these names, so a name must
 * match the corresponding `Renderer` member exactly rather than read as prose.
 */
void setObjectName(const VulkanContext& ctx, uint64_t handle, VkObjectType type, const char* name);

GpuBuffer createBuffer(const VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                       VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags flags = 0,
                       const char* name = nullptr);
void destroyBuffer(const VulkanContext& ctx, GpuBuffer& buf);

/// `arrayLayers > 1` produces a 2D_ARRAY view, which is how the shadow cascades live
/// in one image.
GpuImage createImage(const VulkanContext& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                     uint32_t mipLevels = 1, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, uint32_t arrayLayers = 1,
                     const char* name = nullptr);

/// A view of one array layer, for rendering into a single cascade.
VkImageView createLayerView(const VulkanContext& ctx, const GpuImage& img, uint32_t layer,
                            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

/// Six-layer cube-compatible image. `img.view` is a CUBE view for sampling; write to
/// it through a storage view from createStorageView(), because a compute shader
/// cannot write a cube directly.
GpuImage createCubeImage(const VulkanContext& ctx, uint32_t size, VkFormat format, VkImageUsageFlags usage,
                         uint32_t mipLevels = 1, const char* name = nullptr);

/// A 2D_ARRAY view of one mip, which is the shape `imageStore` needs.
VkImageView createStorageView(const VulkanContext& ctx, const GpuImage& img, uint32_t mipLevel);
void destroyImage(const VulkanContext& ctx, GpuImage& img);

uint32_t mipLevelsFor(VkExtent2D extent);

/// synchronization2 image layout transition.
void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                     VkAccessFlags2 dstAccess, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                     uint32_t baseMip = 0, uint32_t mipCount = VK_REMAINING_MIP_LEVELS);

/// Wide enough for every current caller (two) with room to spare. Exceeding it aborts
/// rather than truncating: a barrier silently covering fewer buffers than asked for is a
/// race that reproduces on one driver in ten.
inline constexpr size_t kMaxBufferBarriers = 8;

/// The buffer counterpart of `transitionImage`, with the same argument order.
///
/// Always `VK_QUEUE_FAMILY_IGNORED`: a caller needing a queue-ownership transfer must
/// write the struct out rather than grow this. Up to `kMaxBufferBarriers` buffers land in
/// one dependency; the range, when given, applies to every buffer in the list.
void bufferBarrier(VkCommandBuffer cmd, std::initializer_list<VkBuffer> buffers, VkPipelineStageFlags2 srcStage,
                   VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                   VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);


/**
 * @brief Immediate-submit helper for staging uploads.
 *
 * One command buffer and one fence, reused. Every submit blocks until complete —
 * fine for load time, never for the frame loop.
 */
class Uploader {
  public:
    /// No destructor releases a Vulkan handle here -- teardown is an explicit `shutdown()`
    /// -- so a copy would duplicate every handle and two `shutdown()` calls would be a
    /// double free with no destructor anywhere near it.
    Uploader() = default;
    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    /// Aborts on failure, as VulkanContext::init does and for the same reason.
    void init(const VulkanContext& ctx);
    void shutdown(const VulkanContext& ctx);

    /// Copy host data into a device-local buffer via a staging buffer.
    void uploadBuffer(const VulkanContext& ctx, GpuBuffer& dst, const void* data, VkDeviceSize size);
    /// The same, at a byte offset into `dst`.
    void uploadBufferAt(const VulkanContext& ctx, GpuBuffer& dst, VkDeviceSize offset, const void* data,
                        VkDeviceSize size);
    /// Device to device, from the front of both.
    void copyBuffer(const VulkanContext& ctx, GpuBuffer& dst, const GpuBuffer& src, VkDeviceSize size);

    /// Copy pixels into mip 0, generate the mip chain, leave in SHADER_READ_ONLY_OPTIMAL.
    void uploadImageWithMips(const VulkanContext& ctx, GpuImage& dst, const void* pixels, VkDeviceSize size);

    /// Records every upload into one command buffer so the round-trip is paid once. The
    /// staging buffers must outlive the batch, so `endBatch` is what frees them.
    void beginBatch(const VulkanContext& ctx);
    void addImageWithMips(const VulkanContext& ctx, GpuImage& dst, const void* pixels, VkDeviceSize size);

    /**
     * @brief Copy a mip chain that already exists, level by level.
     *
     * The route a block-compressed format must take: `addImageWithMips` generates its
     * chain with `vkCmdBlitImage`, which cannot filter BC7.
     *
     * `levels` are (byte offset into `data`, byte length), base level first.
     */
    void addImageLevels(const VulkanContext& ctx, GpuImage& dst, const void* data, VkDeviceSize size,
                        const std::vector<std::pair<uint64_t, uint64_t>>& levels);

    /**
     * @brief Submit the batch on the transfer queue and block until it lands.
     *
     * The copies go to `ctx.transferQueue`. Where that family is distinct from the
     * graphics family the images are *released* by the transfer submit and *acquired* by a
     * second graphics submit waiting on a semaphore: Vulkan requires both halves, and
     * skipping the acquire is undefined behaviour the validation layers do not always
     * catch.
     */
    void endBatch(const VulkanContext& ctx);

    /**
     * @brief The same submission without the wait.
     *
     * Returns false if there was nothing to submit. Poll `batchComplete()` and call
     * `reclaimBatch()` once it returns true; until then the staging buffers are alive and
     * the destination images must not be sampled.
     */
    [[nodiscard]] bool endBatchAsync(const VulkanContext& ctx);
    [[nodiscard]] bool batchComplete(const VulkanContext& ctx) const;
    void reclaimBatch(const VulkanContext& ctx);

    /// Record arbitrary commands and block until they finish. Load-time only.
    VkCommandBuffer beginImmediate(const VulkanContext& ctx);
    void endImmediate(const VulkanContext& ctx);

  private:
    void begin(const VulkanContext& ctx);
    void submitAndWait(const VulkanContext& ctx);
    /// End the transfer command buffer, submit it, and -- across families -- record and
    /// submit the acquire half on the graphics queue. Leaves `fence` unsignalled.
    void submitBatch(const VulkanContext& ctx);
    void recordImageWithMips(GpuImage& dst, VkBuffer staging);
    /// Record the release half of a queue-family ownership transfer for an image the
    /// batch just wrote, and remember it so the acquire half can name it. A no-op when
    /// the two families are the same.
    void releaseToGraphics(const VulkanContext& ctx, const GpuImage& dst);
    void recordImageLevels(GpuImage& dst, VkBuffer staging,
                           const std::vector<std::pair<uint64_t, uint64_t>>& levels);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    bool batching = false;
    std::vector<GpuBuffer> batchStaging;
    /// Images this batch wrote, so the acquire barriers know what to name. Cleared with
    /// the staging buffers.
    std::vector<VkImage> batchImages;
    bool batchInFlight = false;

    /// Separate pool and command buffer on the transfer family, allocated even when the
    /// family is shared -- a command pool belongs to exactly one family.
    VkCommandPool transferPool = VK_NULL_HANDLE;
    VkCommandBuffer transferCmd = VK_NULL_HANDLE;
    /// Signalled by the transfer submit, waited on by the acquire submit. Unused where
    /// the families are the same, because then there is only one submit.
    VkSemaphore transferDone = VK_NULL_HANDLE;
};

} // namespace gfx
