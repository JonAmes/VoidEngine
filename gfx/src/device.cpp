#include <ve/gfx/device.h>

#include <ve/core/log.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>
#include <vector>

namespace ve::gfx {

namespace {

std::string to_lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool name_contains(const VkPhysicalDeviceProperties& props, const std::string& needle_lower) {
    return to_lower(props.deviceName).find(needle_lower) != std::string::npos;
}

} // namespace

std::expected<device, std::string> device::create(const instance& inst) {
    const auto devices = inst.enumerate_physical_devices();
    if (devices.empty()) {
        return std::unexpected("no Vulkan physical devices visible");
    }

    // Read the env override per call (OS-neutral std::getenv; #38).
    std::string needle_raw;
    if (const char* env = std::getenv("VE_GPU"); env != nullptr) {
        needle_raw = env;
    }
    const std::string needle_lower = to_lower(needle_raw);

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    if (!needle_raw.empty()) {
        for (const VkPhysicalDevice phys : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(phys, &props);
            if (name_contains(props, needle_lower)) {
                chosen = phys;
                break;
            }
        }
        if (chosen == VK_NULL_HANDLE) {
            return std::unexpected("VE_GPU='" + needle_raw + "' matches no Vulkan device");
        }
    } else {
        // Default policy: prefer discrete, else first available.
        for (const VkPhysicalDevice phys : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(phys, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                chosen = phys;
                break;
            }
        }
        if (chosen == VK_NULL_HANDLE) {
            chosen = devices[0];
        }
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(chosen, &props);

    // Queue families: graphics implies transfer; present joins later with WSI (2.8).
    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(chosen, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(chosen, &family_count, families.data());

    std::uint32_t graphics_family = VK_QUEUE_FAMILY_IGNORED;
    for (std::uint32_t i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
            graphics_family = i;
            break;
        }
    }
    if (graphics_family == VK_QUEUE_FAMILY_IGNORED) {
        return std::unexpected("device '" + std::string(props.deviceName) + "' has no graphics queue");
    }

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_ci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = graphics_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    // Vulkan 1.3 *feature*, not just core version: dynamicRendering must be
    // explicitly enabled even with apiVersion 1.3 (decision #37's premise —
    // every pipeline uses dynamic rendering, so the engine enables it at
    // device creation and never falls back to VkRenderPass).
    VkPhysicalDeviceVulkan13Features feats13{}; // value-init: the struct
    feats13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES; // has ~24
    feats13.dynamicRendering = VK_TRUE; // members; don't fight the warning wall
    const VkDeviceCreateInfo dev_ci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &feats13,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_ci,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
        .pEnabledFeatures = nullptr, // 1.0 baseline stays default; 1.3 chain above
    };

    VkDevice dev = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateDevice(chosen, &dev_ci, nullptr, &dev); r != VK_SUCCESS) {
        return std::unexpected("vkCreateDevice failed: VkResult(" + std::to_string(static_cast<int>(r)) + ")");
    }
    volkLoadDevice(dev);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, graphics_family, 0, &queue);

    device out;
    out.physical_ = chosen;
    out.device_ = dev;
    out.graphics_queue_ = queue;
    out.graphics_family_ = graphics_family;

    VE_LOG_INFO("gfx: logical device '%s' (family %u graphics queue)", props.deviceName, graphics_family);
    return out;
}

device::~device() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
    }
}

device::device(device&& other) noexcept
    : physical_(std::exchange(other.physical_, VK_NULL_HANDLE)),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      graphics_queue_(std::exchange(other.graphics_queue_, VK_NULL_HANDLE)),
      graphics_family_(std::exchange(other.graphics_family_, 0)) {}

device& device::operator=(device&& other) noexcept {
    if (this != &other) {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            vkDestroyDevice(device_, nullptr);
        }
        physical_ = std::exchange(other.physical_, VK_NULL_HANDLE);
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        graphics_queue_ = std::exchange(other.graphics_queue_, VK_NULL_HANDLE);
        graphics_family_ = std::exchange(other.graphics_family_, 0);
    }
    return *this;
}

} // namespace ve::gfx
