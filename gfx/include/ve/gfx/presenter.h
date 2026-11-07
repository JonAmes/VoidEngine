#pragma once
#include <ve/gfx/device.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace ve::gfx {

// AGENTS boundary 3: nothing renders to "the backbuffer." A pass writes a
// render target; a *presenter* decides what "showing" that target means:
//  - Tier-2 offscreen_presenter (this class): the target is a plain image;
//    capture_rgba8() is the "display" — the golden image IS the frame.
//  - WSI presenter (2.8): a swapchain image is just another target; present
//    blits the pass output into it. "Backbuffer" never leaks upstream.
// VR note: presenters never assume one camera or one eye — view/eye lists
// are render-pass parameters; a presenter owns *a* target, N targets exist.
class presenter {
public:
    virtual ~presenter() = default;

    // What a pass may render into. This is the only render-target surface a
    // pass ever sees.
    virtual VkImageView color_view() const = 0;
    virtual VkFormat color_format() const = 0;
    virtual std::uint32_t width() const = 0;
    virtual std::uint32_t height() const = 0;

    // Bytes in the format's component order (B8G8R8A8_UNORM: B,G,R,A per px).
    virtual std::vector<std::uint8_t> capture_rgba8() = 0;
};

// Headless Tier-2 presenter (decision #35): color image + host-visible
// readback buffer, all on the graphics queue. No surface, no display,
// no assumption beyond one Vulkan 1.3 device.
class offscreen_presenter final : public presenter {
public:
    static std::expected<offscreen_presenter, std::string> create(
        const device& dev, std::uint32_t width, std::uint32_t height,
        VkFormat format = VK_FORMAT_B8G8R8A8_UNORM);

    ~offscreen_presenter() override;
    offscreen_presenter(const offscreen_presenter&) = delete;
    offscreen_presenter& operator=(const offscreen_presenter&) = delete;
    offscreen_presenter(offscreen_presenter&& other) noexcept;

    VkImageView color_view() const override { return view_; }
    VkFormat color_format() const override { return format_; }
    std::uint32_t width() const override { return width_; }
    std::uint32_t height() const override { return height_; }

    // Explicit clear through the full layout chain — the 2.6 render pass
    // takes over the COLOR_ATTACHMENT side of this image later.
    bool clear(const float rgba[4]);
    std::vector<std::uint8_t> capture_rgba8() override;

private:
    offscreen_presenter() = default;

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    // Layout bookkeeping: image starts UNDEFINED; every submit leaves it
    // TRANSFER_SRC so readback needs no barrier until a pass writes again.
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace ve::gfx
