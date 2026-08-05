#include "gfx/Swapchain.h"

#include "core/Logger.h"
#include "gfx/VulkanContext.h"

#include <GLFW/glfw3.h>

#include <algorithm>

namespace gfx {

namespace {

VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& available) {
    // An _SRGB swapchain means the hardware does the linear->sRGB encode on write,
    // so the tonemap shader can output linear and stay correct.
    for (const auto& f : available) {
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return available.front();
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& available, bool vsync) {
    if (vsync) return VK_PRESENT_MODE_FIFO_KHR; // always supported

    for (VkPresentModeKHR m : available) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    }
    for (VkPresentModeKHR m : available) {
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& caps, GLFWwindow* window) {
    if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;

    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(window, &w, &h);

    VkExtent2D extent{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return extent;
}

} // namespace

void Swapchain::create(const VulkanContext& ctx, GLFWwindow* window, bool vsync) {
    VkSurfaceCapabilitiesKHR caps{};
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice, ctx.surface, &caps),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, ctx.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, ctx.surface, &formatCount, formats.data());

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, ctx.surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, ctx.surface, &modeCount, modes.data());

    if (formats.empty() || modes.empty()) {
        core::Logger::critical(core::LogCategory::Vulkan, "Surface reports no formats or present modes");
    }

    const VkSurfaceFormatKHR surfaceFormat = chooseFormat(formats);
    format = surfaceFormat.format;
    presentMode = choosePresentMode(modes, vsync);
    extent = chooseExtent(caps, window);

    uint32_t desired = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && desired > caps.maxImageCount) desired = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = ctx.surface;
    info.minImageCount = desired;
    info.imageFormat = format;
    info.imageColorSpace = surfaceFormat.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    // TRANSFER_SRC is frame capture's, and surfaces are not required to allow it --
    // requesting an unsupported usage fails swapchain creation outright.
    captureSupported = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (captureSupported) info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;

    const uint32_t families[] = {ctx.graphicsFamily, ctx.presentFamily};
    if (ctx.graphicsFamily != ctx.presentFamily) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    vkCheck(vkCreateSwapchainKHR(ctx.device, &info, nullptr, &handle), "vkCreateSwapchainKHR");

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(ctx.device, handle, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(ctx.device, handle, &count, images.data());

    views.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = images[i];
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = format;
        v.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        v.subresourceRange.levelCount = 1;
        v.subresourceRange.layerCount = 1;
        vkCheck(vkCreateImageView(ctx.device, &v, nullptr, &views[i]), "vkCreateImageView(swapchain)");
    }

    core::Logger::status(core::LogCategory::Vulkan, "Swapchain %ux%u, %u images, %s", extent.width, extent.height, count,
                   presentMode == VK_PRESENT_MODE_MAILBOX_KHR    ? "MAILBOX"
                   : presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE"
                                                                  : "FIFO");
}

void Swapchain::destroy(const VulkanContext& ctx) {
    for (VkImageView v : views) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(ctx.device, v, nullptr);
    }
    views.clear();
    images.clear();

    if (handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx.device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
}

FrameResult Swapchain::recreate(const VulkanContext& ctx, GLFWwindow* window, bool vsync) {
    // A minimised window has a zero-sized framebuffer, which is not a legal extent.
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    while (w == 0 || h == 0) {
        glfwWaitEvents();
        glfwGetFramebufferSize(window, &w, &h);
        if (glfwWindowShouldClose(window)) return FrameResult::WindowClosed;
    }

    vkDeviceWaitIdle(ctx.device);
    destroy(ctx);
    create(ctx, window, vsync);
    return FrameResult::Continue;
}

} // namespace gfx
