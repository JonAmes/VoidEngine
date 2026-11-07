#include <ve/gfx/instance.h>

#include <ve/core/log.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace ve::gfx {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool validation_layer_available() {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& l) {
        return std::string(l.layerName) == kValidationLayer;
    });
}

std::string result_text(VkResult r) {
    // Only the results create paths can realistically hit; numeric otherwise
    // so unknown codes never hide behind a wrong name.
    switch (r) {
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    default: return "VkResult(" + std::to_string(static_cast<int>(r)) + ")";
    }
}

} // namespace

std::expected<instance, std::string> instance::create(const char* app_name) {
    // Loads instance-independent commands (layer/extension enumeration,
    // vkCreateInstance) through the platform loader. Idempotent by design.
    volkInitialize();

    if (!validation_layer_available()) {
        return std::unexpected(
            "Vulkan validation layer not installed — apt vulkan-validationlayers "
            "or the Vulkan SDK (engine policy: every instance validates)");
    }

    const VkApplicationInfo app{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = app_name,
        // Version fields stay 0 here: the VERSION file is the single version
        // source and reaches binaries via packaging, not via this struct.
        .applicationVersion = VK_MAKE_VERSION(0, 0, 0),
        .pEngineName = "VoidEngine",
        .engineVersion = VK_MAKE_VERSION(0, 0, 0),
        .apiVersion = VK_API_VERSION_1_3, // decision #37 floor
    };
    const VkInstanceCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app,
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = &kValidationLayer,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
    };

    VkInstance handle = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateInstance(&ci, nullptr, &handle); r != VK_SUCCESS) {
        return std::unexpected("vkCreateInstance failed: " + result_text(r));
    }

    instance out;
    out.handle_ = handle;
    volkLoadInstance(handle); // instance+physical-device commands from now on

    // Log what we are actually running on: the #35 policy (lavapipe in CI vs
    // VE_GPU dev runs) must be obvious in every test log.
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(handle, &n, nullptr);
    VE_LOG_INFO("gfx: VkInstance @API 1.3 created on %u device(s), validation ON", n);
    return out;
}

instance::~instance() {
    if (handle_ != VK_NULL_HANDLE) {
        vkDestroyInstance(handle_, nullptr);
    }
}

instance::instance(instance&& other) noexcept : handle_(std::exchange(other.handle_, VK_NULL_HANDLE)) {}

instance& instance::operator=(instance&& other) noexcept {
    if (this != &other) {
        if (handle_ != VK_NULL_HANDLE) {
            vkDestroyInstance(handle_, nullptr);
        }
        handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
    }
    return *this;
}

std::vector<VkPhysicalDevice> instance::enumerate_physical_devices() const {
    if (handle_ == VK_NULL_HANDLE) {
        return {};
    }
    std::uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(handle_, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(handle_, &count, devices.data()) != VK_SUCCESS) {
        return {};
    }
    return devices;
}

} // namespace ve::gfx
