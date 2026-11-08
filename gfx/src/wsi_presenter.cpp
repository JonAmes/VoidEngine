#include <ve/gfx/wsi_presenter.h>

#include "cmd_helpers.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace ve::gfx {

namespace {

constexpr VkImageSubresourceRange color_range() {
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

// Pick the swapchain surface format. Preference order matches the engine's
// default RT format (B8G8R8A8) so the blit is same-format — cross-format
// blits restrict filtering and add surprises we never want at this seam.
std::expected<std::pair<VkFormat, VkColorSpaceKHR>, std::string>
pick_surface_format(VkPhysicalDevice phys, VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, nullptr) != VK_SUCCESS ||
        count == 0u) {
        return std::unexpected("surface reports zero formats");
    }
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, formats.data());
    for (const VkFormat wanted : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB}) {
        for (const auto& f : formats) {
            if (f.format == wanted) {
                return std::pair{f.format, f.colorSpace};
            }
        }
    }
    return std::pair{formats[0].format, formats[0].colorSpace};
}

} // namespace

std::expected<wsi_presenter, std::string> wsi_presenter::create(
    const device& dev, VkSurfaceKHR surface, std::uint32_t req_w, std::uint32_t req_h) {
    if (surface == VK_NULL_HANDLE) {
        return std::unexpected(
            "wsi_presenter needs a VkSurfaceKHR — make one via platform/window (GLFW owns it)");
    }

    wsi_presenter out;
    out.device_ = dev.handle();
    out.physical_ = dev.physical();
    out.queue_ = dev.graphics_queue();
    out.surface_ = surface;

    // Present capability of OUR graphics family (desktop: same family; the
    // XR split-queue case is platform/xr's presenter, not this class).
    VkBool32 presentable = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(out.physical_, dev.graphics_queue_family(),
                                            surface, &presentable) != VK_SUCCESS ||
        presentable != VK_TRUE) {
        return std::unexpected("graphics queue family cannot present to this surface");
    }

    // FIFO is mandatory by engine policy (never assume mailbox).
    std::uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(out.physical_, surface, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(out.physical_, surface, &mode_count, modes.data());
    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_KHR) == modes.end()) {
        return std::unexpected("surface lacks VK_PRESENT_MODE_FIFO_KHR (spec-required; driver bug)");
    }

    // Command pool for the presenter's internal one-shot work.
    const VkCommandPoolCreateInfo pool_ci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = dev.graphics_queue_family(),
    };
    if (const VkResult r = vkCreateCommandPool(out.device_, &pool_ci, nullptr, &out.pool_);
        r != VK_SUCCESS) {
        return std::unexpected("vkCreateCommandPool failed: VkResult(" +
                               std::to_string(static_cast<int>(r)) + ")");
    }

    std::string err;
    if (!out.build(req_w, req_h, err)) {
        vkDestroyCommandPool(out.device_, out.pool_, nullptr);
        return std::unexpected(err);
    }
    return out;
}

bool wsi_presenter::build(std::uint32_t req_w, std::uint32_t req_h, std::string& err) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS) {
        err = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
        return false;
    }

    // currentExtent == UINT32_MAX means "we choose" (Wayland-ish); desktop
    // X11/Win32 always advertise one. Either way clamp into the caps.
    std::uint32_t w = req_w;
    std::uint32_t h = req_h;
    if (caps.currentExtent.width != UINT32_MAX) {
        w = caps.currentExtent.width;
        h = caps.currentExtent.height;
    }
    w = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
    h = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (w == 0u || h == 0u) {
        err = "surface extent is 0 (minimized window) — recreate after restore";
        return false;
    }

    const auto fmt = pick_surface_format(physical_, surface_);
    if (!fmt) {
        err = fmt.error();
        return false;
    }
    format_ = fmt->first;
    width_ = w;
    height_ = h;

    // minImageCount+1: one image while the previous frame is still on screen
    // (present is waited, but the compositor keeps showing the old image).
    std::uint32_t image_count = caps.minImageCount + 1u;
    if (caps.maxImageCount > 0u && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    const VkSwapchainCreateInfoKHR sc_ci{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = surface_,
        .minImageCount = image_count,
        .imageFormat = format_,
        .imageColorSpace = fmt->second,
        .imageExtent = {width_, height_},
        .imageArrayLayers = 1,
        // TRANSFER_DST: the blit lands here. COLOR_ATTACHMENT is kept because
        // present() ends the image in COLOR_ATTACHMENT_OPTIMAL (a layout
        // vkQueuePresentKHR accepts) and future presenters may render direct.
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };
    if (const VkResult r = vkCreateSwapchainKHR(device_, &sc_ci, nullptr, &swapchain_);
        r != VK_SUCCESS) {
        err = "vkCreateSwapchainKHR failed: VkResult(" + std::to_string(static_cast<int>(r)) + ")";
        return false;
    }

    std::uint32_t img_count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &img_count, nullptr);
    swapchain_images_.resize(img_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &img_count, swapchain_images_.data());
    swapchain_views_.reserve(img_count);
    for (const VkImage img : swapchain_images_) {
        const VkImageViewCreateInfo view_ci{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format_,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = color_range(),
        };
        VkImageView view = VK_NULL_HANDLE;
        if (const VkResult r = vkCreateImageView(device_, &view_ci, nullptr, &view);
            r != VK_SUCCESS) {
            err = "swapchain image view failed";
            destroy_targets();
            return false;
        }
        swapchain_views_.push_back(view); // never handed to a pass — kept for
                                          // the day a direct-render presenter
                                          // replaces the blit path
    }

    if (!make_rt(err)) {
        destroy_targets();
        return false;
    }
    if (!make_readback(err)) {
        destroy_targets();
        return false;
    }
    rt_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    surface_lost_ = false;
    return true;
}

bool wsi_presenter::make_rt(std::string& err) {
    const VkImageCreateInfo image_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format_,
        .extent = {width_, height_, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // Same usage as the offscreen presenter's RT: a pass writes it, a
        // transfer reads it (blit to swapchain AND readback).
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (const VkResult r = vkCreateImage(device_, &image_ci, nullptr, &rt_); r != VK_SUCCESS) {
        err = "RT vkCreateImage failed: VkResult(" + std::to_string(static_cast<int>(r)) + ")";
        return false;
    }
    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(device_, rt_, &reqs);
    const auto mt = find_memory_type(physical_, reqs.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!mt) {
        err = "no DEVICE_LOCAL memory type for WSI RT";
        return false;
    }
    const VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = reqs.size,
        .memoryTypeIndex = *mt,
    };
    if (const VkResult r = vkAllocateMemory(device_, &alloc, nullptr, &rt_memory_); r != VK_SUCCESS) {
        err = "RT memory alloc failed";
        return false;
    }
    vkBindImageMemory(device_, rt_, rt_memory_, 0);

    const VkImageViewCreateInfo view_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = rt_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format_,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = color_range(),
    };
    if (const VkResult r = vkCreateImageView(device_, &view_ci, nullptr, &rt_view_);
        r != VK_SUCCESS) {
        err = "RT vkCreateImageView failed";
        return false;
    }
    return true;
}

bool wsi_presenter::make_readback(std::string& err) {
    const std::size_t byte_size = static_cast<std::size_t>(width_) * height_ * 4u;
    const VkBufferCreateInfo buf_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = byte_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (const VkResult r = vkCreateBuffer(device_, &buf_ci, nullptr, &readback_); r != VK_SUCCESS) {
        err = "readback vkCreateBuffer failed";
        return false;
    }
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, readback_, &reqs);
    const auto mt = find_memory_type(
        physical_, reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mt) {
        err = "no HOST_VISIBLE|HOST_COHERENT memory type for WSI readback";
        return false;
    }
    const VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = reqs.size,
        .memoryTypeIndex = *mt,
    };
    if (const VkResult r = vkAllocateMemory(device_, &alloc, nullptr, &readback_memory_);
        r != VK_SUCCESS) {
        err = "readback memory alloc failed";
        return false;
    }
    vkBindBufferMemory(device_, readback_, readback_memory_, 0);
    return true;
}

void wsi_presenter::destroy_targets() noexcept {
    for (const VkImageView v : swapchain_views_) {
        vkDestroyImageView(device_, v, nullptr);
    }
    swapchain_views_.clear();
    swapchain_images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        // MUST die before the surface it was built from (window teardown
        // order: presenter -> surface -> GLFW window).
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    if (readback_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, readback_, nullptr);
        readback_ = VK_NULL_HANDLE;
    }
    if (readback_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, readback_memory_, nullptr);
        readback_memory_ = VK_NULL_HANDLE;
    }
    if (rt_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, rt_view_, nullptr);
        rt_view_ = VK_NULL_HANDLE;
    }
    if (rt_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, rt_memory_, nullptr);
        rt_memory_ = VK_NULL_HANDLE;
    }
    if (rt_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, rt_, nullptr);
        rt_ = VK_NULL_HANDLE;
    }
}

wsi_presenter::~wsi_presenter() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    destroy_targets();
    vkDestroyCommandPool(device_, pool_, nullptr);
    // surface_ belongs to platform/window (GLFW destroys it) — never here.
}

wsi_presenter::wsi_presenter(wsi_presenter&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      physical_(std::exchange(other.physical_, VK_NULL_HANDLE)),
      queue_(std::exchange(other.queue_, VK_NULL_HANDLE)),
      surface_(std::exchange(other.surface_, VK_NULL_HANDLE)),
      swapchain_(std::exchange(other.swapchain_, VK_NULL_HANDLE)),
      swapchain_images_(std::move(other.swapchain_images_)),
      swapchain_views_(std::move(other.swapchain_views_)),
      rt_(std::exchange(other.rt_, VK_NULL_HANDLE)),
      rt_memory_(std::exchange(other.rt_memory_, VK_NULL_HANDLE)),
      rt_view_(std::exchange(other.rt_view_, VK_NULL_HANDLE)),
      readback_(std::exchange(other.readback_, VK_NULL_HANDLE)),
      readback_memory_(std::exchange(other.readback_memory_, VK_NULL_HANDLE)),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
      rt_layout_(other.rt_layout_),
      format_(other.format_),
      width_(other.width_),
      height_(other.height_),
      surface_lost_(other.surface_lost_) {}

wsi_presenter& wsi_presenter::operator=(wsi_presenter&& other) noexcept {
    if (this != &other) {
        this->~wsi_presenter();
        new (this) wsi_presenter(std::move(other));
    }
    return *this;
}

// --- frame helpers: identical call shape to offscreen_presenter ----------

VkCommandBuffer wsi_presenter::begin_command() {
    // alloc + exactly ONE begin (2.7 lesson: alloc_one_shot would begin twice).
    VkCommandBuffer cmd = alloc_cmd(device_, pool_);
    if (cmd == VK_NULL_HANDLE) {
        return cmd;
    }
    if (begin_one_shot(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, pool_, 1, &cmd);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

bool wsi_presenter::begin_pass(VkCommandBuffer cmd) {
    const VkImageMemoryBarrier to_att{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = rt_layout_, // bookkept, never guessed
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = rt_,
        .subresourceRange = color_range(),
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_att);
    rt_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return true;
}

bool wsi_presenter::end_pass(VkCommandBuffer cmd) {
    const VkImageMemoryBarrier to_src{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = rt_layout_,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = rt_,
        .subresourceRange = color_range(),
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_src);
    rt_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return true;
}

bool wsi_presenter::submit(VkCommandBuffer cmd) {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return false;
    }
    VkCommandBuffer handles[1] = {cmd};
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = handles,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    if (vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        return false;
    }
    const bool ok = (vkQueueWaitIdle(queue_) == VK_SUCCESS);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
    return ok;
}

bool wsi_presenter::present() {
    if (swapchain_ == VK_NULL_HANDLE) {
        return false;
    }

    // One frame in flight: the pass submit already waited the queue, and the
    // previous present()'s wait covers the compositor handoff enough for
    // FIFO. So acquire needs no semaphore (2.10 replaces this with real
    // fence/semaphore machinery — presenter-independent then).
    std::uint32_t image_index = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, VK_NULL_HANDLE,
                                       VK_NULL_HANDLE, &image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        surface_lost_ = true; // frame skipped; recreate() is the recovery
        return true;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        surface_lost_ = true; // shown, but the swapchain drifted (resize)
    } else if (r != VK_SUCCESS) {
        return false;
    }

    const VkImage dst = swapchain_images_[image_index];
    VkCommandBuffer cmd = alloc_one_shot(device_, pool_);
    if (cmd == VK_NULL_HANDLE) {
        return false;
    }

    // RT must be blit-source-ready. If a caller skipped end_pass, move it.
    if (rt_layout_ != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        const VkImageMemoryBarrier rt_to_src{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = rt_layout_,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = rt_,
            .subresourceRange = color_range(),
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &rt_to_src);
        rt_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    // Acquired exclusive-mode image is UNDEFINED by contract: the barrier's
    // UNDEFINED old layout discards whatever the compositor left — content
    // we neither own nor need.
    const VkImageMemoryBarrier to_dst{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = dst,
        .subresourceRange = color_range(),
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_dst);

    // RT (same format as the swapchain by construction) -> swapchain image.
    // Extents are equal by build(); LINEAR stays valid for same-format blits.
    const VkImageBlit blit{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        // VkOffset3D members are int32_t: explicit cast, extents already
        // clamped to caps.maxImageExtent (fits int32) at build().
        .srcOffsets = {{0, 0, 0}, {static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_), 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_), 1}},
    };
    vkCmdBlitImage(cmd, rt_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    // Present-accepted end layout: COLOR_ATTACHMENT_OPTIMAL (a layout
    // vkQueuePresentKHR takes; also what a future direct-render pass ends in).
    const VkImageMemoryBarrier to_present{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = dst,
        .subresourceRange = color_range(),
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_present);

    submit_and_wait(queue_, cmd);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);

    const VkPresentInfoKHR pi{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .swapchainCount = 1,
        .pSwapchains = &swapchain_,
        .pImageIndices = &image_index,
        .pResults = nullptr,
    };
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        surface_lost_ = true; // shown or skipped; recreate() at caller's pace
        return true;
    }
    return r == VK_SUCCESS;
}

bool wsi_presenter::recreate(std::uint32_t req_w, std::uint32_t req_h) {
    if (device_ == VK_NULL_HANDLE || surface_ == VK_NULL_HANDLE) {
        return false;
    }
    // No frame may be in flight; submit()/present() already waited, this is
    // the belt to that suspenders (cheap once per resize).
    vkDeviceWaitIdle(device_);
    // Old swapchain and its views die first (vkDestroySwapchainKHR must run
    // before the surface it came from — the surface is untouched here).
    // build() re-queries caps/formats, so a monitor change mid-resize is
    // handled by the same code path as create.
    destroy_targets();
    std::string err;
    return build(req_w, req_h, err);
}

std::vector<std::uint8_t> wsi_presenter::capture_rgba8() {
    const std::size_t byte_size = static_cast<std::size_t>(width_) * height_ * 4u;
    std::vector<std::uint8_t> out;
    if (rt_ == VK_NULL_HANDLE) {
        return out;
    }

    VkCommandBuffer cmd = alloc_one_shot(device_, pool_);
    if (cmd == VK_NULL_HANDLE) {
        return out;
    }

    if (rt_layout_ != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        const VkImageMemoryBarrier to_src{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = rt_layout_,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = rt_,
            .subresourceRange = color_range(),
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_src);
        rt_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width_, height_, 1},
    };
    vkCmdCopyImageToBuffer(cmd, rt_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &region);

    const VkBufferMemoryBarrier to_host{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback_,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &to_host, 0, nullptr);

    submit_and_wait(queue_, cmd);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);

    void* mapped = nullptr;
    if (const VkResult r = vkMapMemory(device_, readback_memory_, 0, byte_size, 0, &mapped);
        r != VK_SUCCESS) {
        return {};
    }
    out.assign(static_cast<std::uint8_t*>(mapped), static_cast<std::uint8_t*>(mapped) + byte_size);
    vkUnmapMemory(device_, readback_memory_);
    return out;
}

} // namespace ve::gfx
