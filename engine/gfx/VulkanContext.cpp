#include "gfx/VulkanContext.h"

#include "core/Logger.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace gfx {

const char* vkResultString(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    default: return "VK_ERROR_<unmapped>";
    }
}

void vkCheck(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        core::Logger::critical(core::LogCategory::Vulkan, "%s failed: %s", what, vkResultString(result));
    }
}

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
    if (data == nullptr || data->pMessage == nullptr) return VK_FALSE;

    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        core::Logger::error(core::LogCategory::Vulkan, "%s", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        core::Logger::warn(core::LogCategory::Vulkan, "%s", data->pMessage);
    } else {
        core::Logger::debug(core::LogCategory::Vulkan, "%s", data->pMessage);
    }
    return VK_FALSE;
}

bool hasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

bool hasInstanceExtension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

bool hasDeviceExtension(VkPhysicalDevice dev, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

/**
 * @brief Repair a VK_LAYER_PATH that hides the system layers.
 *
 * VK_LAYER_PATH *replaces* the loader's default explicit-layer search path rather
 * than extending it. A stale `export VK_LAYER_PATH=$VULKAN_SDK/etc/vulkan/...` with
 * VULKAN_SDK unset therefore expands to a nonexistent directory and silently
 * disables all layer discovery — validation included.
 *
 * Appending rather than clearing keeps a deliberately-set path authoritative (it
 * stays first in the list) while restoring the system directories behind it.
 *
 * @return true if the variable was changed and layers are worth re-enumerating.
 */
bool repairLayerPath() {
#ifdef _WIN32
    // Windows discovers explicit layers through the registry key
    // HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers, which VK_LAYER_PATH does not shadow,
    // and there is no standard directory list to append -- everything below is
    // Linux-only by construction.
    return false;
#else
    const char* current = std::getenv("VK_LAYER_PATH");
    if (current == nullptr) return false;

    // The loader's standard explicit-layer locations on Linux.
    static const char* kStandardDirs[] = {
        "/usr/local/etc/vulkan/explicit_layer.d",
        "/usr/local/share/vulkan/explicit_layer.d",
        "/etc/vulkan/explicit_layer.d",
        "/usr/share/vulkan/explicit_layer.d",
    };

    std::vector<std::string> entries;
    std::stringstream existing(current);
    std::string item;
    while (std::getline(existing, item, ':')) {
        if (!item.empty()) entries.push_back(item);
    }

    bool added = false;
    for (const char* dir : kStandardDirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        if (std::find(entries.begin(), entries.end(), dir) != entries.end()) continue;
        entries.emplace_back(dir);
        added = true;
    }

    if (!added) return false;

    std::string joined;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) joined += ':';
        joined += entries[i];
    }

    setenv("VK_LAYER_PATH", joined.c_str(), /*overwrite=*/1);
    // Not a warning: reaching here is the repair succeeding, and `init` already warns when
    // it was not enough. Raising this to `warn` puts an orange line on every debug launch
    // for a condition the engine handles by design.
    core::Logger::status(core::LogCategory::Vulkan, "VK_LAYER_PATH hid the system layers; appended standard paths");
    core::Logger::debug(core::LogCategory::Vulkan, "VK_LAYER_PATH is now %s", joined.c_str());
    return true;
#endif
}

VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    return info;
}

} // namespace

VkSampleCountFlagBits VulkanContext::clampSampleCount(uint32_t requested) const {
    VkSampleCountFlags supported =
        properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts;

    const VkSampleCountFlagBits order[] = {VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT,
                                           VK_SAMPLE_COUNT_1_BIT};
    for (VkSampleCountFlagBits bit : order) {
        if (static_cast<uint32_t>(bit) <= requested && (supported & bit)) return bit;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

void VulkanContext::init(GLFWwindow* window, bool enableValidation, bool allowRayQuery, bool enableSyncValidation) {
    vkCheck(volkInitialize(), "volkInitialize");

    validationEnabled = enableValidation && hasLayer(kValidationLayer);

    // Layer discovery happens on demand, so the search path can still be repaired here --
    // once the instance exists it cannot.
    if (enableValidation && !validationEnabled && repairLayerPath()) {
        validationEnabled = hasLayer(kValidationLayer);
    }

    if (enableValidation && !validationEnabled) {
        core::Logger::warn(core::LogCategory::Vulkan, "Validation requested but %s is not available", kValidationLayer);
        core::Logger::warn(core::LogCategory::Vulkan, "Install vulkan-validationlayers, or check VK_LAYER_PATH");
    }

    // From project(Substrate VERSION ...) in CMakeLists.txt -- the only place the number
    // is written, and the same place the release artifact's filename comes from.
    constexpr uint32_t kSubstrateVkVersion =
        VK_MAKE_VERSION(SUBSTRATE_VERSION_MAJOR, SUBSTRATE_VERSION_MINOR, SUBSTRATE_VERSION_PATCH);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Substrate";
    app.applicationVersion = kSubstrateVkVersion;
    app.pEngineName = "Substrate";
    app.engineVersion = kSubstrateVkVersion;
    app.apiVersion = VK_API_VERSION_1_3;

    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    if (glfwExts == nullptr) {
        core::Logger::critical(core::LogCategory::Vulkan, "GLFW reports no Vulkan surface extensions");
    }

    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);

    // Not gated on validation: pass labels and object names are what make a Release
    // capture legible, and the loader hands back null function pointers where nothing
    // implements the extension.
    //
    // `validationEnabled ||` rather than the probe alone, because the validation layer
    // supplies this extension itself and a layer's extensions do not appear in a global
    // enumeration taken before that layer is enabled. Dropping the disjunction turns the
    // messenger off on any loader that does not export debug_utils on its own.
    debugUtilsEnabled = validationEnabled || hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtilsEnabled) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateInfo instInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instInfo.pApplicationInfo = &app;
    instInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT dbgInfo = debugMessengerInfo();

    // Chained ahead of the messenger rather than instead of it: both must reach
    // `vkCreateInstance`, or errors raised during instance creation arrive nowhere.
    constexpr VkValidationFeatureEnableEXT kSyncFeature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &kSyncFeature;

    syncValidationEnabled = validationEnabled && enableSyncValidation;
    if (enableSyncValidation && !validationEnabled) {
        core::Logger::warn(core::LogCategory::Vulkan, "Synchronization validation requested without the validation layer; ignored");
    }

    if (validationEnabled) {
        instInfo.enabledLayerCount = 1;
        instInfo.ppEnabledLayerNames = &kValidationLayer;
        // Chaining here catches errors during instance creation itself.
        instInfo.pNext = &dbgInfo;
        if (syncValidationEnabled) {
            validationFeatures.pNext = &dbgInfo;
            instInfo.pNext = &validationFeatures;
        }
    }

    vkCheck(vkCreateInstance(&instInfo, nullptr, &instance), "vkCreateInstance");
    volkLoadInstance(instance);

    if (validationEnabled) {
        vkCheck(vkCreateDebugUtilsMessengerEXT(instance, &dbgInfo, nullptr, &debugMessenger),
                "vkCreateDebugUtilsMessengerEXT");
    }

    vkCheck(glfwCreateWindowSurface(instance, window, nullptr, &surface), "glfwCreateWindowSurface");

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) core::Logger::critical(core::LogCategory::Vulkan, "No Vulkan physical devices found");

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    int bestScore = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);

        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        if (!hasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        // Must have a queue family that both renders and presents.
        uint32_t famCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &famCount, nullptr);
        std::vector<VkQueueFamilyProperties> fams(famCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &famCount, fams.data());

        uint32_t gfx = UINT32_MAX;
        uint32_t present = UINT32_MAX;
        uint32_t transfer = UINT32_MAX;
        for (uint32_t i = 0; i < famCount; ++i) {
            if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                if (gfx == UINT32_MAX) gfx = i;
            }
            // Transfer capable and neither graphics nor compute -- on a discrete card the
            // DMA engine. Accepting a family that also draws would put uploads back on the
            // queue they are meant to run beside.
            if ((fams[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                (fams[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0) {
                if (transfer == UINT32_MAX) transfer = i;
            }
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported);
            if (supported && present == UINT32_MAX) present = i;
        }
        if (gfx == UINT32_MAX || present == UINT32_MAX) continue;

        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 : 100;
        score += static_cast<int>(props.limits.maxImageDimension2D / 1024);
        if (gfx == present) score += 50; // avoids cross-family ownership transfers

        if (score > bestScore) {
            bestScore = score;
            physicalDevice = candidate;
            graphicsFamily = gfx;
            presentFamily = present;
            // Falls back to the graphics family; `Uploader` skips the ownership transfer
            // when the two indices are equal, so no caller branches on this.
            transferFamily = transfer == UINT32_MAX ? gfx : transfer;
            properties = props;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        core::Logger::critical(core::LogCategory::Vulkan, "No device supports Vulkan 1.3 with swapchain + present");
    }

    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    timestampPeriod = properties.limits.timestampPeriod;
    maxSampleCount = clampSampleCount(VK_SAMPLE_COUNT_8_BIT);

    core::Logger::status(core::LogCategory::Vulkan, "Device: %s (Vulkan %u.%u.%u, driver %u)", properties.deviceName,
                   VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
                   VK_API_VERSION_PATCH(properties.apiVersion), properties.driverVersion);
    core::Logger::status(core::LogCategory::Vulkan, "  max MSAA: %ux, timestampPeriod: %.1f ns",
                   static_cast<uint32_t>(maxSampleCount), static_cast<double>(timestampPeriod));

    if (properties.limits.timestampComputeAndGraphics == VK_FALSE) {
        core::Logger::warn(core::LogCategory::Vulkan, "timestampComputeAndGraphics is false; GPU zones will be unavailable");
    }

    std::set<uint32_t> uniqueFamilies{graphicsFamily, presentFamily, transferFamily};
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    const float priority = 1.0f;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        queueInfos.push_back(q);
    }

    VkPhysicalDeviceVulkan13Features vk13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    vk13.dynamicRendering = VK_TRUE;
    vk13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features vk12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vk12.timelineSemaphore = VK_TRUE;
    vk12.bufferDeviceAddress = VK_TRUE;
    // The whole scene binds one descriptor set holding every texture in one array, which
    // needs runtime-sized arrays and non-uniform indexing.
    vk12.descriptorIndexing = VK_TRUE;
    vk12.runtimeDescriptorArray = VK_TRUE;
    vk12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    vk12.descriptorBindingPartiallyBound = VK_TRUE;
    vk12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    vk12.scalarBlockLayout = VK_TRUE;
    // The draw count comes out of the buffer the culling dispatch wrote; without this the
    // CPU has to read it back, a round-trip costing more than the culling saved.
    vk12.drawIndirectCount = VK_TRUE;
    vk12.pNext = &vk13;

    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.features.samplerAnisotropy = VK_TRUE;
    features.features.fillModeNonSolid = VK_TRUE;
    // `drawIndirectFirstInstance` is how a vertex shader finds its transform: each draw
    // carries its own instance slot, there being no per-draw push constants.
    features.features.multiDrawIndirect = VK_TRUE;
    features.features.drawIndirectFirstInstance = VK_TRUE;
    // Without this every storage image a fragment shader declares must carry the
    // NonWritable decoration, and the validation layer rejects `shadowmask.frag` outright.
    features.features.fragmentStoresAndAtomics = VK_TRUE;
    features.pNext = &vk12;

    // Checked rather than assumed: a driver missing these turns every draw in the frame
    // into draw zero, which is not a failure mode to discover from a screenshot.
    {
        VkPhysicalDeviceFeatures available{};
        vkGetPhysicalDeviceFeatures(physicalDevice, &available);
        if (available.multiDrawIndirect == VK_FALSE || available.drawIndirectFirstInstance == VK_FALSE) {
            core::Logger::critical(core::LogCategory::Vulkan,
                             "device lacks multiDrawIndirect (%s) or drawIndirectFirstInstance (%s); "
                             "indirect submission is the only draw path",
                             available.multiDrawIndirect ? "yes" : "no",
                             available.drawIndirectFirstInstance ? "yes" : "no");
        }
    }

    std::vector<const char*> deviceExts{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // All three or none: ray query needs acceleration structures, which need deferred host
    // operations even when nothing is deferred. A subset produces a device that cannot
    // trace anything.
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};

    rayQuerySupported = allowRayQuery &&
                        hasDeviceExtension(physicalDevice, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                        hasDeviceExtension(physicalDevice, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                        hasDeviceExtension(physicalDevice, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

    if (rayQuerySupported) {
        deviceExts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        deviceExts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        deviceExts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

        asFeatures.accelerationStructure = VK_TRUE;
        rqFeatures.rayQuery = VK_TRUE;
        rqFeatures.pNext = features.pNext;
        asFeatures.pNext = &rqFeatures;
        features.pNext = &asFeatures;

        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &asProps;
        vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
        asScratchAlignment = asProps.minAccelerationStructureScratchOffsetAlignment;
    }

    // Both halves matter: the extension without CLOCK_MONOTONIC among the host domains
    // yields timestamps on a clock nothing else in this process reads. The null check is
    // the availability test for the entry point -- volk leaves it unresolved where the
    // loader does not expose it.
    if (vkGetPhysicalDeviceCalibrateableTimeDomainsEXT != nullptr &&
        hasDeviceExtension(physicalDevice, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) {
        uint32_t domainCount = 0;
        vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(physicalDevice, &domainCount, nullptr);
        std::vector<VkTimeDomainEXT> domains(domainCount);
        vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(physicalDevice, &domainCount, domains.data());

        bool hasDevice = false;
        bool hasMonotonic = false;
        for (VkTimeDomainEXT d : domains) {
            hasDevice = hasDevice || d == VK_TIME_DOMAIN_DEVICE_EXT;
            hasMonotonic = hasMonotonic || d == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
        }

        if (hasDevice && hasMonotonic) {
            calibratedTimestampsSupported = true;
            calibratedHostDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
            deviceExts.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        }
    }

    VkDeviceCreateInfo devInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    devInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    devInfo.pQueueCreateInfos = queueInfos.data();
    devInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
    devInfo.ppEnabledExtensionNames = deviceExts.data();
    devInfo.pNext = &features;

    vkCheck(vkCreateDevice(physicalDevice, &devInfo, nullptr, &device), "vkCreateDevice");
    volkLoadDevice(device);

    core::Logger::status(core::LogCategory::Vulkan, "Ray query: %s",
                   rayQuerySupported     ? "available"
                   : allowRayQuery       ? "unavailable (RT features compiled out)"
                                         : "disabled by request (RT features compiled out)");

    core::Logger::status(core::LogCategory::Vulkan, "Calibrated timestamps: %s",
                   calibratedTimestampsSupported ? "available (GPU zones on the CPU timeline)"
                                                 : "unavailable (GPU zones placed relative to frame start)");

    vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
    vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);
    vkGetDeviceQueue(device, transferFamily, 0, &transferQueue);
    core::Logger::status(core::LogCategory::Vulkan, "Transfer queue: family %u (%s)", transferFamily,
                   transferFamily == graphicsFamily ? "shared with graphics" : "dedicated");

    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocInfo{};
    allocInfo.physicalDevice = physicalDevice;
    allocInfo.device = device;
    allocInfo.instance = instance;
    allocInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocInfo.pVulkanFunctions = &fns;
    allocInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    vkCheck(vmaCreateAllocator(&allocInfo, &allocator), "vmaCreateAllocator");

    core::Logger::status(core::LogCategory::Vulkan, "Vulkan context ready (validation %s, sync validation %s, debug names %s)",
                   validationEnabled ? "on" : "off", syncValidationEnabled ? "on" : "off",
                   debugUtilsEnabled ? "on" : "off");
}

VulkanContext::MemoryUsage VulkanContext::memoryUsage() const {
    MemoryUsage usage;
    if (allocator == VK_NULL_HANDLE) return usage;

    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator, budgets);

    for (uint32_t heap = 0; heap < memoryProperties.memoryHeapCount; ++heap) {
        // Device-local heaps only, so staging churn does not move the `VRAM [...]` line
        // the MSAA memory baseline is read from.
        if ((memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
        usage.allocatedBytes += budgets[heap].statistics.allocationBytes;
        usage.reservedBytes += budgets[heap].statistics.blockBytes;
        usage.budgetBytes += budgets[heap].budget;
        usage.allocationCount += budgets[heap].statistics.allocationCount;
        usage.blockCount += budgets[heap].statistics.blockCount;
    }
    return usage;
}

void VulkanContext::logMemoryUsage(const char* label) const {
    const MemoryUsage usage = memoryUsage();
    constexpr double kMiB = 1024.0 * 1024.0;
    core::Logger::status(core::LogCategory::Render, "VRAM [%s]: %.1f MiB in %u allocations (%.1f MiB reserved, %.0f MiB budget)",
                   label, static_cast<double>(usage.allocatedBytes) / kMiB, usage.allocationCount,
                   static_cast<double>(usage.reservedBytes) / kMiB, static_cast<double>(usage.budgetBytes) / kMiB);
}

void VulkanContext::shutdown() {
    if (allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator);
        allocator = VK_NULL_HANDLE;
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
    if (debugMessenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
        debugMessenger = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

} // namespace gfx
