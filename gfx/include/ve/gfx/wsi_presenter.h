#pragma once
#include <ve/gfx/device.h>
#include <ve/gfx/presenter.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace ve::gfx {

// Desktop WSI presenter (2.8). AGENTS boundary 3 holds exactly as it does
// for the offscreen presenter: the pass renders into a *presenter-owned
// render target*; present() BLITS that RT into the swapchain image acquired
// for the frame. The swapchain is just another target — a pass never sees a
// swapchain image, and nothing outside this class ever reads one ("backbuffer"
// never leaks upstream). The seam from 2.4 is untouched: begin_command/
// begin_pass/end_pass/submit are identical to offscreen_presenter's.
//
// Frame model = offscreen's Tier-2 simplification (2.10 brings real
// fences/semaphores): exactly one frame in flight, submit() waits its queue.
// Present mode is FIFO unconditionally (never assume mailbox, never force
// tearing). surface_lost() reports OUT_OF_DATE/SUBOPTIMAL — on desktop that
// means "the window resized": poll_events, framebuffer_size, recreate().
//
// #38: VkSurfaceKHR comes from platform/window.cpp (GLFW on Linux, its
// Windows twin later); from here up everything is VK_KHR_surface/volk —
// no GLFW, no X11, no windows.h anywhere in gfx/.
class wsi_presenter final : public presenter {
public:
    // The device's graphics queue family must be present-capable on this
    // surface (checked at create — desktop families always are; a queue
    // split would arrive with the XR presenter in platform/xr, not here).
    // req_w/req_h are used only when the window system advertises no
    // current extent; the extent is then clamped to surface capabilities.
    static std::expected<wsi_presenter, std::string> create(
        const device& dev, VkSurfaceKHR surface,
        std::uint32_t req_w, std::uint32_t req_h);

    ~wsi_presenter() override;
    wsi_presenter(const wsi_presenter&) = delete;
    wsi_presenter& operator=(const wsi_presenter&) = delete;
    wsi_presenter(wsi_presenter&& other) noexcept;
    wsi_presenter& operator=(wsi_presenter&& other) noexcept;

    // The pass target is the RT — never a swapchain image (see header note).
    VkImageView color_view() const override { return rt_view_; }
    VkFormat color_format() const override { return format_; }
    std::uint32_t width() const override { return width_; }
    std::uint32_t height() const override { return height_; }

    // Readback of the RT (works headless-ish: no compositor round-trip).
    std::vector<std::uint8_t> capture_rgba8() override;

    // Frame helpers mirror offscreen_presenter — a render pass may be written
    // against EITHER presenter without change (the whole point of 2.4).
    VkCommandBuffer begin_command();
    bool begin_pass(VkCommandBuffer cmd);          // RT -> COLOR_ATTACHMENT_OPTIMAL
    bool end_pass(VkCommandBuffer cmd);            // RT -> TRANSFER_SRC_OPTIMAL
    bool submit(VkCommandBuffer cmd);              // end + submit + wait (one in flight)

    // acquire -> barrier -> blit RT->image -> barrier -> submit+wait -> FIFO
    // present. OUT_OF_DATE/SUBOPTIMAL (acquire or present) set surface_lost()
    // and still return true — the frame was skipped or shown, and the
    // swapchain needs recreate(). Hard failures return false.
    bool present();
    bool surface_lost() const { return surface_lost_; }

    // Resize path (2.9 demo loop): rebuild swapchain AND the RT together at
    // the new extent. Call with no frame in flight — submit()/present()
    // waited everything, and this waits again as a belt.
    bool recreate(std::uint32_t req_w, std::uint32_t req_h);

private:
    wsi_presenter() = default;

    // (Re)build swapchain + images/views + RT + readback at the clamped
    // extent. Shared by create() and recreate(). Fills err on failure.
    // Memory types come from physical_ (stored), so recreate needs nothing
    // but the requested extent.
    bool build(std::uint32_t req_w, std::uint32_t req_h, std::string& err);
    void destroy_targets() noexcept; // everything except surface_/pool_
    bool make_rt(std::string& err);
    bool make_readback(std::string& err);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchain_images_;        // owned by the swapchain
    std::vector<VkImageView> swapchain_views_;     // ours; never handed out

    // The presenter's real render target (what color_view() exposes).
    VkImage rt_ = VK_NULL_HANDLE;
    VkDeviceMemory rt_memory_ = VK_NULL_HANDLE;
    VkImageView rt_view_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;

    // Layout bookkeeping, presenter-owned (a pass may never barrier this).
    // RT starts UNDEFINED; submits leave it TRANSFER_SRC (blit source).
    VkImageLayout rt_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    std::uint32_t width_ = 0;   // swapchain extent == RT extent, always
    std::uint32_t height_ = 0;
    bool surface_lost_ = false;
};

} // namespace ve::gfx
