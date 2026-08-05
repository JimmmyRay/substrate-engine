#pragma once

#include <volk.h>

#include <cstdint>
#include <vector>

struct GLFWwindow;

namespace gfx {

struct VulkanContext;

/**
 * @brief Whether rendering can continue.
 *
 * Not an error code: the only non-Continue value means the user closed the window while
 * the swapchain was being rebuilt, which is the normal way this program ends. Genuine
 * Vulkan failures never travel this way -- they abort at the failing call through vkCheck.
 */
enum class FrameResult {
    Continue,
    WindowClosed,
};

/**
 * @brief Swapchain plus its per-image views.
 *
 * No framebuffers: dynamic rendering targets image views directly.
 */
struct Swapchain {
    /// Non-copyable; see the note on `Uploader` in Resources.h.
    Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    /// Whether the surface allowed TRANSFER_SRC on its images, which is what frame capture
    /// copies from. False means a capture request must be refused, not attempted.
    bool captureSupported = false;

    std::vector<VkImage> images;
    std::vector<VkImageView> views;

    /// Aborts on failure rather than reporting it: every call inside goes through
    /// vkCheck, so there is no path where this returns without a usable swapchain.
    void create(const VulkanContext& ctx, GLFWwindow* window, bool vsync);
    void destroy(const VulkanContext& ctx);

    /// Tear down and rebuild at the window's current size. Blocks while minimised,
    /// which is why it can report that the window went away in the meantime.
    [[nodiscard]] FrameResult recreate(const VulkanContext& ctx, GLFWwindow* window, bool vsync);

    uint32_t imageCount() const { return static_cast<uint32_t>(images.size()); }
};

} // namespace gfx
