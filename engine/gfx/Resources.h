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
 * Images, buffers and pipelines all need one, which is the third occurrence and so
 * the point at which this stops being duplication and becomes a function. A global
 * one because the callers span modules -- `Resources.cpp` names what it creates,
 * `Renderer.cpp` names its pipelines -- and it holds no state.
 *
 * A no-op when the extension is absent, so no call site needs a guard. That matters:
 * the alternative is `if (ctx.debugUtilsEnabled)` at thirty creation sites, which is
 * thirty chances to forget.
 *
 * These names are what a RenderDoc capture shows instead of `Image 47`, and they are
 * the filenames `scripts/rdoc/images.py` writes -- so they should match the member
 * name in `Renderer` exactly rather than read as prose.
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
/// A buffer has no layout, so where the image version takes old and new it takes nothing:
/// what is left is the src/dst stage-and-access pair, which is the whole of what a buffer
/// barrier says. `offset` and `size` are trailing defaults for the same reason `baseMip`
/// and `mipCount` are on the image version -- four of the six callers want the whole
/// buffer and should not have to say so, and the two that barrier a sub-range of the
/// instance buffer would otherwise have to keep writing the struct out by hand.
///
/// Always `VK_QUEUE_FAMILY_IGNORED`: nothing in this engine transfers queue ownership of a
/// buffer, and a caller that needs to should write the struct out rather than grow this.
///
/// Takes a list because the particle passes barrier the pool and the key buffer together,
/// as one dependency rather than two. Up to `kMaxBufferBarriers` at once; the range, when
/// given, applies to every buffer in the list.
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
    /**
     * Non-copyable, and every other handle-owning type here says the same.
     *
     * The ownership model in `engine/gfx/` is rule-of-zero plus an explicit `shutdown()` or
     * `destroy()`: no destructor releases a Vulkan handle, so nothing is released twice by
     * accident and the teardown order stays written down where it can be read. What that
     * model does not give you is any protection from a copy -- the implicit copy constructor
     * duplicates every handle, and two objects each reaching their own `shutdown()` is a
     * double free with no destructor anywhere near it.
     *
     * The copy is never wanted, so it is deleted rather than defined. `GpuBuffer` and
     * `GpuImage` stay copyable: they are value records returned from `createBuffer` and
     * `createImage` and stored in vectors, and it is the manager that owns them, not them.
     */
    Uploader() = default;
    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    /// Aborts on failure, as VulkanContext::init does and for the same reason.
    void init(const VulkanContext& ctx);
    void shutdown(const VulkanContext& ctx);

    /// Copy host data into a device-local buffer via a staging buffer.
    void uploadBuffer(const VulkanContext& ctx, GpuBuffer& dst, const void* data, VkDeviceSize size);
    /// The same, at a byte offset. What appending a second model into a shared geometry
    /// buffer needs, and the reason the offset is a parameter rather than a second buffer.
    void uploadBufferAt(const VulkanContext& ctx, GpuBuffer& dst, VkDeviceSize offset, const void* data,
                        VkDeviceSize size);
    /// Device to device, from the front of both. What growing a buffer is made of: make a
    /// bigger one, copy the old contents in, drop the old one.
    void copyBuffer(const VulkanContext& ctx, GpuBuffer& dst, const GpuBuffer& src, VkDeviceSize size);

    /// Copy pixels into mip 0, generate the mip chain, leave in SHADER_READ_ONLY_OPTIMAL.
    void uploadImageWithMips(const VulkanContext& ctx, GpuImage& dst, const void* pixels, VkDeviceSize size);

    // ------------------------------------------------------------------ batching
    // One blocking submit per image costs a full GPU round-trip each time. Batching
    // records every upload into a single command buffer and pays that cost once.
    // Staging buffers must stay alive until the batch completes, so they are held
    // and freed together in endBatch().

    void beginBatch(const VulkanContext& ctx);
    void addImageWithMips(const VulkanContext& ctx, GpuImage& dst, const void* pixels, VkDeviceSize size);

    /**
     * @brief Copy a mip chain that already exists, level by level (4.6a).
     *
     * The counterpart to addImageWithMips, and the difference is the whole point of a
     * texture cache: that one uploads level 0 and *generates* the rest with blits,
     * which is both load-time work and impossible for a block-compressed format --
     * vkCmdBlitImage cannot filter BC7. This one copies bytes the offline tool already
     * produced.
     *
     * `levels` are (byte offset into `data`, byte length), base level first.
     */
    void addImageLevels(const VulkanContext& ctx, GpuImage& dst, const void* data, VkDeviceSize size,
                        const std::vector<std::pair<uint64_t, uint64_t>>& levels);

    /**
     * @brief Submit the batch on the transfer queue and block until it lands.
     *
     * Property (iii) of 4.6b: the copies go to `ctx.transferQueue` with a fence, not to
     * the graphics queue with a wait-idle. Where the transfer family is distinct from
     * the graphics family the images are *released* by the transfer submit and
     * *acquired* by a second, tiny graphics submit that waits on a semaphore -- Vulkan
     * requires both halves of that handshake, and skipping the acquire is undefined
     * behaviour the validation layers will not always catch.
     */
    void endBatch(const VulkanContext& ctx);

    /**
     * @brief The same submission without the wait. For streaming (4.6b).
     *
     * Returns false if there was nothing to submit. Poll `batchComplete()` and call
     * `reclaimBatch()` once it returns true; until then the staging buffers are alive
     * and the destination images must not be sampled.
     *
     * This is the call a residency system makes and the load path does not: at load
     * there is nothing else to do while the copy runs, and a blocking submit is the
     * simpler correct thing. Mid-frame there is a whole frame to get on with.
     */
    [[nodiscard]] bool endBatchAsync(const VulkanContext& ctx);
    [[nodiscard]] bool batchComplete(const VulkanContext& ctx) const;
    void reclaimBatch(const VulkanContext& ctx);

    /// Record arbitrary commands and block until they finish. For one-off load-time
    /// GPU work that is neither a buffer nor an image upload -- the IBL precompute is
    /// the only caller, and it needs dispatches and blits rather than copies.
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
    /// Set by any addImageWithMips() in the batch: blits and fragment-stage barriers
    /// are graphics work, so the whole batch goes to the graphics queue.

    /// Separate pool and command buffer on the transfer family. Distinct objects even
    /// when the family is shared: a command pool belongs to exactly one family, and
    /// branching on that at every use would be worse than allocating a second pool.
    VkCommandPool transferPool = VK_NULL_HANDLE;
    VkCommandBuffer transferCmd = VK_NULL_HANDLE;
    /// Signalled by the transfer submit, waited on by the acquire submit. Unused where
    /// the families are the same, because then there is only one submit.
    VkSemaphore transferDone = VK_NULL_HANDLE;
};

} // namespace gfx
