#pragma once
#include <volk.h>

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace ve::gfx {

namespace detail {
// Heap-stable so the messenger callback's user pointer survives instance
// moves; defined in instance.cpp (2.7 strict messenger bookkeeping).
struct validation_state;
} // namespace detail

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

    // 2.7 policy: validation ERRORs and WARNINGs (INFO/VERBOSE filtered) are
    // recorded here instead of merely printing. Every gfx test asserts this
    // vector is empty at its end — "green" means "zero validation problems",
    // not "the driver tolerated it". A deliberately wrong call must show up
    // here (gfx_validation_strict proves the wiring).
    std::vector<std::string> validation_problems() const;
    void clear_validation_problems();

private:
    instance() = default;
    VkInstance handle_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    std::unique_ptr<detail::validation_state> validation_;
};

} // namespace ve::gfx
