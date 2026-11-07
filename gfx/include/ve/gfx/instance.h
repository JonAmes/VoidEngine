#pragma once
#include <volk.h>

#include <expected>
#include <string>
#include <vector>

namespace ve::gfx {

// VkInstance policy for this engine (decision #37 + #35 + 2.7):
//  - API floor VK_API_VERSION_1_3; 1.4 features only opportunistically later.
//  - The Khronos validation layer is REQUIRED on every instance we create —
//    the strict-messenger watchdog depends on it; an unvalidated instance is
//    a bug, never a fallback. (Release-mode opt-out is a later build-mode
//    decision, not an ad-hoc flag.)
//  - Surfaceless by default: no WSI extensions here. Desktop WSI lands in 2.8
//    as its own path; XR API calls never come through this class at all
//    (AGENTS boundary 2: XR lives in platform/xr/).
// OS-neutral per decision #38: volk loads the platform loader at runtime
// (no link against libvulkan/vulkan-1.lib).
class instance {
public:
    static std::expected<instance, std::string> create(const char* app_name = "void-engine");

    ~instance();
    instance(const instance&) = delete;
    instance& operator=(const instance&) = delete;
    instance(instance&& other) noexcept;
    instance& operator=(instance&& other) noexcept;

    VkInstance handle() const { return handle_; }

    // Empty on failure (clean-failure doctrine; enumeration errors are
    // uninteresting beyond "no devices").
    std::vector<VkPhysicalDevice> enumerate_physical_devices() const;

private:
    instance() = default;
    VkInstance handle_ = VK_NULL_HANDLE;
};

} // namespace ve::gfx
