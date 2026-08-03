#pragma once

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

struct GLFWwindow;

namespace gfx {

/**
 * @brief Instance, device, queues and allocator.
 *
 * Deliberately a plain aggregate of Vulkan handles rather than an abstraction over
 * them: everything downstream talks to Vulkan directly.
 *
 * Targets the Vulkan 1.3 feature set — dynamic rendering and synchronization2 are
 * core, so there are no VkRenderPass or VkFramebuffer objects anywhere in Substrate.
 */
struct VulkanContext {
    /// Non-copyable; see the note on `Uploader` in Resources.h for why every handle owner
    /// in this directory says this.
    VulkanContext() = default;
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};

    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t presentFamily = UINT32_MAX;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    /**
     * @brief Dedicated transfer queue where the device has one, the graphics family
     *        otherwise (4.6b).
     *
     * Property (iii) of the residency delegation: an upload must be able to run on a
     * queue that is not the one drawing, with a fence rather than a `vkQueueWaitIdle`,
     * or streaming a texture in mid-frame means stalling the frame.
     *
     * Equality with `graphicsFamily` is the *supported* case, not a degraded one -- it
     * simply means no queue-family ownership transfer is needed, and `Uploader` tests
     * exactly that.
     */
    uint32_t transferFamily = UINT32_MAX;
    VkQueue transferQueue = VK_NULL_HANDLE;

    VmaAllocator allocator = VK_NULL_HANDLE;

    bool validationEnabled = false;
    /// Whether the layer is additionally tracking every access against every barrier.
    /// Separate from `validationEnabled` because it is far more expensive and is asked
    /// for by name -- see `Config::Render::syncValidation`.
    bool syncValidationEnabled = false;

    /// Whether VK_EXT_debug_utils made it onto the instance. Enabled independently of
    /// validation, because pass labels and object names are what turn a RenderDoc
    /// capture from a list of anonymous draws into a readable frame -- and the frame
    /// worth reading is usually a Release one. False means every naming call in
    /// Resources.cpp and every label in GpuScope is a no-op.
    bool debugUtilsEnabled = false;

    /// Nanoseconds per timestamp tick, from VkPhysicalDeviceLimits.
    float timestampPeriod = 1.0f;

    /**
     * @brief Whether `VK_EXT_calibrated_timestamps` can put GPU zones on the CPU's
     *        clock (5.4).
     *
     * Without it a GPU zone's *duration* is exact but its *position* is not: the two
     * clocks are unrelated, so the profiler can only place a zone relative to the
     * frame's first GPU timestamp and pretend that coincides with the CPU frame start.
     * It does not -- the GPU is executing a frame the CPU finished recording some
     * milliseconds ago -- and the consequence is a trace where a pass appears to begin
     * before the CPU submitted it.
     *
     * Detected, never required, like ray query: false keeps the uncalibrated placement,
     * which is what every measurement before 5.4 was made against.
     */
    bool calibratedTimestampsSupported = false;
    /// The host time domain to calibrate against, valid when the flag above is set.
    /// `VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT` is the one that matters: it is the clock
    /// `std::chrono::steady_clock` reads on Linux, which is the clock every CPU scope in
    /// the trace is already stamped with. Any other domain would need a second
    /// conversion and is treated as unsupported.
    VkTimeDomainEXT calibratedHostDomain = VK_TIME_DOMAIN_DEVICE_EXT;

    /// Highest sample count supported by both colour and depth framebuffers.
    VkSampleCountFlagBits maxSampleCount = VK_SAMPLE_COUNT_1_BIT;

    /**
     * @brief Whether this device can build acceleration structures and trace ray
     *        queries (3.9).
     *
     * Detected, never required. A device without it gets a working build with the
     * ray-traced features compiled out through their specialisation constants, which
     * is exactly what 2.7 exists to make possible -- the alternative being no build at
     * all on hardware that can still draw the scene perfectly well.
     */
    bool rayQuerySupported = false;
    /// Scratch alignment for acceleration-structure builds. Queried, not assumed: it
    /// is 128 on this device and the spec guarantees nothing.
    uint32_t asScratchAlignment = 256;

    /// Aborts on failure rather than reporting it. Every Vulkan call inside goes
    /// through vkCheck, which logs the failing call and the result code and does not
    /// return -- so there is no state in which this finishes without a usable device,
    /// and a bool would only have advertised a failure the caller could never see.
    /// @param allowRayQuery Request the acceleration-structure and ray-query
    ///        extensions where the device offers them. False forces
    ///        `rayQuerySupported` off, which is the only way to run a sanitizer build:
    ///        the proprietary NVIDIA driver fails `vkCreateDevice` outright when those
    ///        extensions are requested under ASan, exactly as it segfaults under TSan.
    ///        Losing ray tracing is a far better outcome than losing the sanitizer.
    void init(GLFWwindow* window, bool enableValidation, bool allowRayQuery = true, bool enableSyncValidation = false);
    void shutdown();

    /// Clamp a requested sample count to what this device actually supports.
    VkSampleCountFlagBits clampSampleCount(uint32_t requested) const;

    struct MemoryUsage {
        uint64_t allocatedBytes = 0; ///< Sum of live allocations
        uint64_t reservedBytes = 0;  ///< Sum of VMA blocks backing them
        uint64_t budgetBytes = 0;    ///< What the driver says is available
        uint32_t allocationCount = 0;
        uint32_t blockCount = 0;
    };

    /// Live VMA statistics, for the profiler and the MSAA memory baseline.
    MemoryUsage memoryUsage() const;
    void logMemoryUsage(const char* label) const;
};

/// Aborts via Logger::critical when `result` is not VK_SUCCESS.
void vkCheck(VkResult result, const char* what);

const char* vkResultString(VkResult result);

} // namespace gfx
