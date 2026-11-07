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
//  - Present-capable queue families are only meaningful once a surface
//    exists; that lands with the WSI presenter (2.8) — nothing here queries
//    WSI (surfaceless instance, XR never enters gfx/, AGENTS boundary 2).
class device {
public:
    static std::expected<device, std::string> create(const instance& inst);

    ~device();
    device(const device&) = delete;
    device& operator=(const device&) = delete;
    device(device&& other) noexcept;
    device& operator=(device&& other) noexcept;

    VkDevice handle() const { return device_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkQueue graphics_queue() const { return graphics_queue_; }
    std::uint32_t graphics_queue_family() const { return graphics_family_; }

private:
    device() = default;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    std::uint32_t graphics_family_ = 0;
};

} // namespace ve::gfx
