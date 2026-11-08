#pragma once
#include <ve/gfx/device.h>
#include <ve/gfx/frame_sync.h>

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

    // Tier-2 frame helpers. The presenter owns the image, therefore it owns
    // its layout transitions — a pass may NEVER barrier the target itself.
    // Two frames in flight (2.10): submit() carries the current slot's fence
    // from the presenter-independent frame_sync machinery instead of waiting;
    // begin_command() waits that slot's previous frame before recording.
    // Call sites are identical to the WSI presenter's acquire/present pair.
    VkCommandBuffer begin_command();               // slot wait + alloc + one begin
    bool begin_pass(VkCommandBuffer cmd);          // current -> COLOR_ATTACHMENT_OPTIMAL
    bool end_pass(VkCommandBuffer cmd);            // COLOR_ATTACHMENT -> TRANSFER_SRC (readback-ready)
    bool submit(VkCommandBuffer cmd);              // end + submit, carrying the slot fence

    // Frame-slot introspection (what gfx_frame_sync pins): the slot submit()
    // just armed, and the machinery's depth.
    std::uint32_t current_frame() const { return sync_.current(); }
    std::uint32_t frames_in_flight() const { return sync_.frame_count(); }

private:
    offscreen_presenter() = default;

    static constexpr std::uint32_t kFramesInFlight = 2; // double-buffered pipeline

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;

    // Presenter-independent sync (2.10): one fence per frame slot, rotated by
    // frame_sync. The frame's command buffer must outlive its submit — it is
    // freed at the next begin_command() for that slot, once the slot's fence
    // has been waited (freeing a still-executing buffer is a validation
    // error; the same rule the WSI presenter follows for its frames).
    frame_sync sync_;
    std::vector<VkCommandBuffer> slot_cmd_; // one pending cmd per slot, VK_NULL_HANDLE = empty
    // Layout bookkeeping: image starts UNDEFINED; every submit leaves it
    // TRANSFER_SRC so readback needs no barrier until a pass writes again.
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace ve::gfx
