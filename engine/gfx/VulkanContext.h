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
 * Targets the Vulkan 1.3 feature set — dynamic rendering and synchronization2 are core,
 * so there are no VkRenderPass or VkFramebuffer objects anywhere in Substrate.
 */
struct VulkanContext {
    /// Non-copyable; see `Uploader` in Resources.h for what a copied handle owner costs.
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

    /// @brief Dedicated transfer queue where the device has one, the graphics family
    ///        otherwise. Equality with `graphicsFamily` is a supported case, not a
    ///        degraded one: it means `Uploader` needs no queue-ownership transfer.
    uint32_t transferFamily = UINT32_MAX;
    VkQueue transferQueue = VK_NULL_HANDLE;

    VmaAllocator allocator = VK_NULL_HANDLE;

    bool validationEnabled = false;
    /// Whether the layer is additionally tracking every access against every barrier.
    /// Far more expensive than `validationEnabled`, so it is asked for by name -- see
    /// `Config::Render::syncValidation`.
    bool syncValidationEnabled = false;

    /// Whether VK_EXT_debug_utils made it onto the instance, independently of validation.
    /// False makes every `setObjectName` call and every `GpuScope` label a no-op.
    bool debugUtilsEnabled = false;

    /// Nanoseconds per timestamp tick, from VkPhysicalDeviceLimits.
    float timestampPeriod = 1.0f;

    /**
     * @brief Whether `VK_EXT_calibrated_timestamps` can put GPU zones on the CPU's clock.
     *
     * Detected, never required. False keeps the uncalibrated placement, where a zone's
     * duration is exact but its position is only relative to the frame's first GPU
     * timestamp -- so a pass can appear to begin before the CPU submitted it.
     */
    bool calibratedTimestampsSupported = false;
    /// The host time domain to calibrate against, valid when the flag above is set. Only
    /// `VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT` is accepted: it is what
    /// `std::chrono::steady_clock` reads on Linux, and every CPU scope in the trace is
    /// already stamped with that clock.
    VkTimeDomainEXT calibratedHostDomain = VK_TIME_DOMAIN_DEVICE_EXT;

    /// Highest sample count supported by both colour and depth framebuffers.
    VkSampleCountFlagBits maxSampleCount = VK_SAMPLE_COUNT_1_BIT;

    /// @brief Whether this device can build acceleration structures and trace ray
    ///        queries. Detected, never required -- a device without it compiles the
    ///        ray-traced features out through their specialisation constants.
    bool rayQuerySupported = false;
    /// Scratch alignment for acceleration-structure builds. Queried, not assumed: the
    /// spec guarantees nothing, and the default here is not what this device reports.
    uint32_t asScratchAlignment = 256;

    /// Aborts on failure rather than reporting it: every Vulkan call inside goes through
    /// `vkCheck`, so this never returns without a usable device.
    /// @param allowRayQuery Request the acceleration-structure and ray-query extensions
    ///        where the device offers them. False forces `rayQuerySupported` off, which is
    ///        what a sanitizer build needs -- the proprietary NVIDIA driver fails
    ///        `vkCreateDevice` outright when those extensions are requested under ASan.
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
