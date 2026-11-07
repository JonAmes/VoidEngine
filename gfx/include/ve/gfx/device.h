#pragma once
#include <ve/gfx/instance.h>

#include <cstdint>
#include <expected>
#include <string>

namespace ve::gfx {

// Physical/logical device selection (decision #35 + #38):
//  - VE_GPU env = case-insensitive deviceName substring override (dev runs
//    on real hardware: VE_GPU=P5000 keeps the inference 3090 untouched).
//  - Default: first DISCRETE_GPU, else first device of any type — in CI the
//    forced lvp ICD (#35) leaves exactly one CPU device, so the default path
//    lands on lavapipe without special-casing.
//  - Present-capable queue families only mean anything once a surface exists, so
//    the query is opt-in: pass the VkSurfaceKHR the platform seam produced (2.8).
//    With one, the device also enables VK_KHR_swapchain — the WSI presenter is
//    the only swapchain toucher, and it needs the device to allow the call.
class device {
public:
    // surface == VK_NULL_HANDLE (default) is the Tier-2 surfaceless path every
    // existing gfx test uses. A real surface additionally requires the chosen
    // graphics family to be present-capable and VK_KHR_swapchain to exist.
    static std::expected<device, std::string> create(const instance& inst,
                                                     VkSurfaceKHR surface = VK_NULL_HANDLE);

    ~device();
    device(const device&) = delete;
    device& operator=(const device&) = delete;
    device(device&& other) noexcept;
    device& operator=(device&& other) noexcept;

    VkDevice handle() const { return device_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkQueue graphics_queue() const { return graphics_queue_; }
    std::uint32_t graphics_queue_family() const { return graphics_family_; }

    // Present side of the seam (2.8). Phase 2 policy: ONE queue does graphics and
    // present — a device whose present family differs from its graphics family is
    // a clean failure, not a hidden second queue (that topology is a later
    // decision, and silently accepting it would move work into 2.10's sync design).
    VkQueue present_queue() const { return present_queue_; }
    std::uint32_t present_queue_family() const { return present_family_; }
    bool present_capable() const { return present_family_ != VK_QUEUE_FAMILY_IGNORED; }

private:
    device() = default;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    std::uint32_t graphics_family_ = 0;
    std::uint32_t present_family_ = VK_QUEUE_FAMILY_IGNORED;
};

} // namespace ve::gfx
