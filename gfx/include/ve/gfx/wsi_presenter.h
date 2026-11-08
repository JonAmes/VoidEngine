#pragma once
#include <ve/gfx/presenter.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace ve::gfx {

// Desktop WSI presenter (2.8) — the second presenter behind the 2.4 seam, and
// the ONLY thing in the engine that ever touches a swapchain (AGENTS boundary 3
// + Phase 2 note). The rule that keeps the seam honest: a render pass sees
// color_view(), which here is a presenter-owned render target, never a swapchain
// image. "Presenting" means this class blits that target into a swapchain image
// it acquired itself. Nothing upstream ever learns a swapchain exists — which is
// exactly why the same passes will drive OpenXR later (Presenter is the seam).
//
// Swapchain policy (checklist 2.8): FIFO (the one present mode Vulkan guarantees
// exists), one frame in flight, and the swapchain format IS the render-target
// format — blits require format-compatible images, and UNORM vs SRGB of the same
// channel layout is not format-compatible, so choosing one format everywhere
// removes a whole class of driver-dependent blit validation.
//
// CI stance: a window needs a desktop seat, so none of this RUNS in CI (decision
// #36). It compiles in CI (ve_gfx always builds this TU + the gfx_wsi_compile_gate
// case pins the seam), and 2.9 exercises it for real under Xvfb + lavapipe.
class wsi_presenter final : public presenter {
public:
    // surface comes from ve::platform::window::create_surface(). The caller owns
    // it: it must outlive this presenter and be destroyed before the VkInstance.
    // dev must have been created WITH that surface (device::create takes it) so
    // VK_KHR_swapchain is enabled and the queue is present-capable.
    static std::expected<wsi_presenter, std::string> create(
        const device& dev, VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height);

    ~wsi_presenter() override;
    wsi_presenter(const wsi_presenter&) = delete;
    wsi_presenter& operator=(const wsi_presenter&) = delete;
    wsi_presenter(wsi_presenter&& other) noexcept;
    wsi_presenter& operator=(wsi_presenter&& other) noexcept;

    // --- presenter seam: the pass renders into the RT below, never the swapchain.
    VkImageView color_view() const override { return rt_view_; }
    VkFormat color_format() const override { return format_; }
    std::uint32_t width() const override { return width_; }
    std::uint32_t height() const override { return height_; }
    std::vector<std::uint8_t> capture_rgba8() override; // RT readback, as Tier 2

    // --- frame loop: one frame in flight. 2.10 replaces this fence discipline
    // with presenter-independent sync at the same call sites.
    std::expected<std::uint32_t, std::string> acquire(); // index + image_available_ signaled
    VkCommandBuffer begin_command();                     // from the internal pool
    bool begin_pass(VkCommandBuffer cmd);                // RT current -> COLOR_ATTACHMENT
    bool end_pass(VkCommandBuffer cmd);                  // RT -> TRANSFER_SRC (blit-ready)
    bool present(VkCommandBuffer cmd);                   // blit RT -> image, present, fence armed

    // Resize / stale-surface rebuild ("resize → VB + recreate"): the 2.6 pipeline
    // keeps viewport+scissor DYNAMIC and its vertex data in NDC, so a resize
    // rebuilds the swapchain, the RT and their views — no pipeline or vertex
    // buffer re-upload. Returns false if the surface cannot be rebuilt.
    bool recreate(std::uint32_t width, std::uint32_t height);

    // True once the swapchain reports OUT_OF_DATE/SUBOPTIMAL: the frame loop must
    // call recreate() before the next acquire. Never silent — acquire()/present()
    // set it and say so in their messages.
    bool needs_recreate() const { return stale_; }

private:
    wsi_presenter() = default;

    bool create_targets(std::uint32_t width, std::uint32_t height, std::string& err);
    void destroy_targets();
    bool create_sync(std::string& err);
    void destroy_sync();

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE; // the present-capable queue (one queue, 2.8)
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchain_images_; // owned by the swapchain, never by us
    std::vector<VkImageView> swapchain_views_;

    // The render target a pass actually writes (the seam from 2.4, unchanged).
    VkImage rt_ = VK_NULL_HANDLE;
    VkDeviceMemory rt_memory_ = VK_NULL_HANDLE;
    VkImageView rt_view_ = VK_NULL_HANDLE;

    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    VkDeviceSize readback_size_ = 0; // width*height*4, recomputed by create_targets
    VkCommandPool pool_ = VK_NULL_HANDLE;

    // The frame's command buffer outlives present(): its submit carries the frame
    // fence, so freeing it inside present() would free a buffer still executing
    // (a validation error, and worse than a leak). It is freed at the next
    // acquire(), once that fence has been waited out — which is exactly what keeps
    // 2.9's ~120-frame spin from growing the pool without bound.
    VkCommandBuffer last_cmd_ = VK_NULL_HANDLE;
    bool last_cmd_valid_ = false;
    std::uint32_t queue_family_ = 0; // the family queue_ came from (pools are per-family)


    VkSemaphore image_available_ = VK_NULL_HANDLE; // acquire -> blit
    // ONE blit->present semaphore PER SWAPCHAIN IMAGE, indexed by image index.
    // A single shared one is a validation error (VUID-vkQueueSubmit-pSignalSemaphores-00067,
    // caught live in 2.9): the frame fence proves only that the SUBMIT finished, never
    // that vkQueuePresentKHR stopped waiting, so the next frame can re-signal a semaphore
    // a previous present still owns. Per-image is sound because a given semaphore is only
    // re-signaled after its image was re-acquired, and acquire() hands an image back only
    // once its earlier presentation is over. (The other fix is VK_KHR_swapchain_maintenance1,
    // which attaches a fence to present — that extension is a decision, not a Phase 2 need.)
    // Sized with the swapchain, so these live with create_targets/destroy_targets.
    std::vector<VkSemaphore> render_finished_;
    VkFence frame_fence_ = VK_NULL_HANDLE; // blit completion (one in flight)

    // Layout bookkeeping, same doctrine as offscreen_presenter: the presenter
    // owns its image's transitions, a pass may never barrier the target.
    VkImageLayout rt_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t current_ = 0; // the image index acquire() handed out
    bool acquired_ = false;     // holding an image between acquire and present
    bool in_flight_ = false;    // a blit is outstanding (fence not yet waited)
    bool stale_ = false;        // recreate() required before the next frame
};

} // namespace ve::gfx
