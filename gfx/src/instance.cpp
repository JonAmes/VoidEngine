#include <ve/gfx/instance.h>

#include <ve/core/log.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace ve::gfx {

// 2.7: strict messenger state. ERROR/WARNING land here; tests fail on them.
struct detail::validation_state {
    std::mutex mutex;
    std::vector<std::string> problems;
};

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kDebugUtilsExt = "VK_EXT_debug_utils"; // literal: header macro absent in some SDK revisions

// Returns VK_FALSE: the layer's own logging still happens (useful in CI
// logs) — the *failure* comes from tests asserting validation_problems()
// empty, which keeps the policy inside the test contract, not in a callback
// that could be silently removed.
VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user_data) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) == 0) {
        return VK_FALSE; // INFO/VERBOSE filtered by policy
    }
    auto* state = static_cast<detail::validation_state*>(user_data);
    std::string msg = data->pMessage != nullptr ? data->pMessage : "(no message)";
    if (data->pMessageIdName != nullptr) {
        msg = std::string(data->pMessageIdName) + ": " + msg;
    }
    std::lock_guard lock(state->mutex);
    state->problems.push_back(std::move(msg));
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT messenger_create_info(detail::validation_state* state) {
    // Value-init + assign: extension structs drift across SDK revisions.
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = validation_callback;
    ci.pUserData = state;
    return ci;
}

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

std::expected<instance, std::string> instance::create(
    const char* app_name, const std::vector<std::string>& extra_extensions) {
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
    auto validation = std::make_unique<detail::validation_state>();
    // Chaining the messenger into vkCreateInstance catches instance-creation
    // errors themselves (a bad layer/extension combo would otherwise print
    // nowhere). VK_EXT_debug_utils comes bundled with the validation layer —
    // since the layer is required (2.2 policy), the extension is too.
    const VkDebugUtilsMessengerCreateInfoEXT messenger_ci =
        messenger_create_info(validation.get());
    // debug_utils is unconditional (validation is policy); extras are the
    // caller's WSI set (2.8: names from GLFW, never hardcoded in gfx/).
    std::vector<const char*> extensions{
        kDebugUtilsExt,
    };
    for (const auto& e : extra_extensions) {
        extensions.push_back(e.c_str()); // ci must outlive vkCreateInstance: it does
    }
    const VkInstanceCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = &messenger_ci,
        .flags = 0,
        .pApplicationInfo = &app,
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = &kValidationLayer,
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    VkInstance handle = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateInstance(&ci, nullptr, &handle); r != VK_SUCCESS) {
        return std::unexpected("vkCreateInstance failed: " + result_text(r));
    }

    instance out;
    out.handle_ = handle;
    out.validation_ = std::move(validation);
    volkLoadInstance(handle); // instance+physical-device commands from now on

    // Persistent messenger for everything after instance creation. volk
    // resolves vkCreateDebugUtilsMessengerEXT at runtime (no static link to
    // the layer; #38 OS-neutral).
    if (const VkResult r = vkCreateDebugUtilsMessengerEXT(handle, &messenger_ci, nullptr,
                                                          &out.messenger_);
        r != VK_SUCCESS) {
        vkDestroyInstance(handle, nullptr);
        return std::unexpected("vkCreateDebugUtilsMessengerEXT failed: " + result_text(r) +
                               " — strict validation is engine policy");
    }

    // Log what we are actually running on: the #35 policy (lavapipe in CI vs
    // VE_GPU dev runs) must be obvious in every test log.
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(handle, &n, nullptr);
    VE_LOG_INFO("gfx: VkInstance @API 1.3 created on %u device(s), validation ON", n);
    return out;
}

instance::~instance() {
    // Messenger must die before the instance it belongs to.
    if (messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(handle_, messenger_, nullptr);
    }
    if (handle_ != VK_NULL_HANDLE) {
        vkDestroyInstance(handle_, nullptr);
    }
}

instance::instance(instance&& other) noexcept
    : handle_(std::exchange(other.handle_, VK_NULL_HANDLE)),
      messenger_(std::exchange(other.messenger_, VK_NULL_HANDLE)),
      validation_(std::move(other.validation_)) {}

instance& instance::operator=(instance&& other) noexcept {
    if (this != &other) {
        if (messenger_ != VK_NULL_HANDLE) {
            vkDestroyDebugUtilsMessengerEXT(handle_, messenger_, nullptr);
        }
        if (handle_ != VK_NULL_HANDLE) {
            vkDestroyInstance(handle_, nullptr);
        }
        handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
        messenger_ = std::exchange(other.messenger_, VK_NULL_HANDLE);
        validation_ = std::move(other.validation_);
    }
    return *this;
}

std::vector<std::string> instance::validation_problems() const {
    if (validation_ == nullptr) {
        return {};
    }
    std::lock_guard lock(validation_->mutex);
    return validation_->problems; // copy: tests may inspect after further calls
}

void instance::clear_validation_problems() {
    if (validation_ == nullptr) {
        return;
    }
    std::lock_guard lock(validation_->mutex);
    validation_->problems.clear();
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
