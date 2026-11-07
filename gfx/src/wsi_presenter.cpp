#include <ve/gfx/wsi_presenter.h>

#include <ve/core/log.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ve::gfx {

namespace {

// The same command/memory helpers offscreen_presenter uses. Deliberately
// duplicated file-locally instead of pulled into a shared header in this commit:
// 2.10 rebuilds exactly this machinery ("frame-sync machinery is presenter-
// independent"), and refactoring the green Tier-2 path for a cosmetic DRY-up
// would put 41 cases / 518 asserts at risk in a task that cannot run them here.
std::optional<std::uint32_t> find_memory_type(VkPhysicalDevice phys, std::uint32_t type_bits,
                                              VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0u &&
            (mem_props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

VkCommandBuffer alloc_cmd(VkDevice device, VkCommandPool pool) {
    const VkCommandBufferAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return cmd;
}

VkResult begin_one_shot(VkCommandBuffer cmd) {
    const VkCommandBufferBeginInfo bi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    return vkBeginCommandBuffer(cmd, &bi);
}

// end + submit + wait, and nothing else. Whoever allocated frees (2.7 rule:
// vkFreeCommandBuffers on an already-freed handle is itself a validation error).
bool submit_and_wait(VkQueue queue, VkCommandBuffer cmd) {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return false;
    }
    const VkCommandBuffer handles[1] = {cmd};
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
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        return false;
    }
    return vkQueueWaitIdle(queue) == VK_SUCCESS;
}

// A WSI call is stale when the swapchain no longer matches the surface. SUBOPTIMAL
// is a success code (the frame is still usable); OUT_OF_DATE is not. Both mean
// "recreate", and neither may be swallowed (2.8's resize contract).
bool result_is_stale(VkResult r) {
    return r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR;
}

std::string result_text(VkResult r) {
    switch (r) {
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    default: return "VkResult(" + std::to_string(static_cast<int>(r)) + ")";
    }
}

// Which stage last touched the render target, derived from the layout we believe
// it is in — the only bookkeeping the presenter needs for its own barriers.
VkPipelineStageFlags source_stage_for(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    default: return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; // UNDEFINED: nothing happened yet
    }
}

VkPipelineStageFlags dest_stage_for(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    default: return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
}

// Access masks are part of the layout contract, not decoration: Vulkan 1.3 made
// it a VUID that a barrier leaving COLOR_ATTACHMENT_OPTIMAL declares
// COLOR_ATTACHMENT_WRITE, leaving TRANSFER_SRC declares TRANSFER_READ, leaving
// TRANSFER_DST declares TRANSFER_WRITE (and the same for the layout entered).
// 2.7's strict messenger turns any of those into a test failure, so they are
// derived from the layouts here rather than remembered per call site — which is
// exactly how offscreen_presenter's barriers passed validation in 2.6/2.7.
VkAccessFlags source_access_for(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    default: return 0; // UNDEFINED: nothing to wait for
    }
}

VkAccessFlags dest_access_for(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    default: return 0;
    }
}

// The presenter owns its image's layout transitions — same doctrine as the
// offscreen path (2.7): a render pass may never barrier the target it writes.
void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    const VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = source_access_for(from),
        .dstAccessMask = dest_access_for(to),
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, source_stage_for(from), dest_stage_for(to), 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

// A swapchain image's layout is undefined every time it is acquired, so the
// first barrier against it must start from UNDEFINED (old contents are garbage).
constexpr VkImageLayout kSwapchainEntryLayout = VK_IMAGE_LAYOUT_UNDEFINED;

} // namespace

std::expected<wsi_presenter, std::string> wsi_presenter::create(
    const device& dev, VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height) {
    if (surface == VK_NULL_HANDLE) {
        return std::unexpected("wsi_presenter needs a VkSurfaceKHR from the platform seam "
                               "(ve::platform::window::create_surface)");
    }
    if (width == 0u || height == 0u) {
        return std::unexpected("wsi_presenter needs a non-zero extent, got " +
                               std::to_string(width) + "x" + std::to_string(height));
    }
    const VkQueue queue = dev.present_queue();
    if (queue == VK_NULL_HANDLE) {
        return std::unexpected("device has no present queue — hand the surface to device::create "
                               "so it enables " VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    // --- surface format: ONE format everywhere (blit compatibility) -----------
    std::uint32_t format_count = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical(), surface, &format_count, nullptr) != VK_SUCCESS ||
        format_count == 0u) {
        return std::unexpected("surface reports no formats — is VK_KHR_surface enabled on the VkInstance?");
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical(), surface, &format_count, formats.data()) != VK_SUCCESS) {
        return std::unexpected("vkGetPhysicalDeviceSurfaceFormatsKHR failed on the second pass");
    }

    VkSurfaceFormatKHR chosen{};
    if (formats.size() == 1u && formats[0].format == VK_FORMAT_UNDEFINED) {
        // The legacy "any format at all" answer: take the engine's own format and
        // keep the colorspace the surface offered.
        chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
        chosen.colorSpace = formats[0].colorSpace;
    } else {
        // Order matters: the offscreen presenter's format first, so a Tier-2
        // golden and a desktop frame agree on what the passes see. Only 8-bit
        // RGBA is accepted — capture_rgba8()'s 4-bytes-per-pixel readback assumes
        // it, which is exactly why anything else is a hard stop, not a fallback.
        static constexpr std::array<VkFormat, 4> kPreferred = {
            VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
        };
        bool found = false;
        for (const VkFormat want : kPreferred) {
            for (const auto& f : formats) {
                if (f.format == want) {
                    chosen = f;
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        if (!found) {
            return std::unexpected("surface offers no 8-bit RGBA format among " +
                                   std::to_string(formats.size()) + " candidate(s)");
        }
    }

    // --- capabilities: the surface owns extent limits and image count ---------
    VkSurfaceCapabilitiesKHR caps{}; // VkSurfaceCapabilitiesKHR has no sType (v1 struct)
    if (const VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physical(), surface, &caps);
        r != VK_SUCCESS) {
        return std::unexpected("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: " + result_text(r));
    }
    if (caps.maxImageExtent.width == 0u || caps.maxImageExtent.height == 0u) {
        return std::unexpected("surface reports a 0x0 max extent — no display behind this surface");
    }

    // --- present mode: FIFO is the one mode Vulkan guarantees exists ----------
    std::uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical(), surface, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    if (mode_count > 0u) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical(), surface, &mode_count, modes.data());
    }
    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_KHR) == modes.end()) {
        return std::unexpected("surface does not offer FIFO (" + std::to_string(mode_count) +
                               " present mode(s) listed) — 2.8 pins FIFO; no mailbox fallback");
    }

    wsi_presenter out;
    out.device_ = dev.handle();
    out.physical_ = dev.physical();
    out.queue_ = queue;
    out.queue_family_ = dev.present_queue_family();
    out.surface_ = surface;

    out.format_ = chosen.format;
    out.color_space_ = chosen.colorSpace;

    std::string err;
    if (!out.create_targets(width, height, err)) {
        return std::unexpected(err);
    }
    if (!out.create_sync(err)) {
        out.destroy_targets();
        return std::unexpected(err);
    }

    VE_LOG_INFO("gfx: WSI presenter up — %ux%u, %u swapchain image(s), FIFO, one frame in flight",
                out.width_, out.height_, static_cast<std::uint32_t>(out.swapchain_images_.size()));
    return out;
}

// Swapchain + its views + the render target a pass actually writes. width/height
// are the requested FRAMEBUFFER size, clamped into the surface's own range — the
// surface, never the caller, decides what is legal.
bool wsi_presenter::create_targets(std::uint32_t width, std::uint32_t height, std::string& err) {
    VkSurfaceCapabilitiesKHR caps{};
    if (const VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);
        r != VK_SUCCESS) {
        err = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: " + result_text(r);
        return false;
    }

    // minImageCount+1: one image on display while the next one is drawn. The
    // classic off-by-one — asking for exactly minImageCount stalls the loop,
    // because acquire and present then cannot overlap at all.
    std::uint32_t image_count = caps.minImageCount + 1u;
    if (caps.maxImageCount > 0u && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount; // maxImageCount == 0 means "no limit"
    }

    // A 0 extent means "whatever the surface has now" (the minimized-window
    // case); any other request is clamped rather than trusted.
    VkExtent2D extent = caps.currentExtent;
    if (width > 0u && height > 0u) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0u || extent.height == 0u) {
        err = "clamped swapchain extent is 0x0 — the surface has no usable size";
        return false;
    }

    // Value-init + assignment, never designated init: this struct has gained
    // fields across SDK header revisions (the 1.4 resolveMode lesson from 2.6).
    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = format_;
    sci.imageColorSpace = color_space_;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    // TRANSFER_DST only. The swapchain image is a blit DESTINATION; a pass never
    // renders into it, so it never needs COLOR_ATTACHMENT (AGENTS boundary 3).
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform; // composite the surface as it already is
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = VK_NULL_HANDLE; // recreate() waits idle first, so no chaining needed

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain); r != VK_SUCCESS) {
        err = "vkCreateSwapchainKHR failed: " + result_text(r) + " — extent " +
              std::to_string(extent.width) + "x" + std::to_string(extent.height);
        return false;
    }
    swapchain_ = swapchain;

    // The images belong to the swapchain; we only ever borrow the handles. We DO
    // own the views, and they must die before the swapchain does.
    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());
    if (swapchain_images_.empty()) {
        err = "swapchain reported no images";
        destroy_targets();
        return false;
    }

    swapchain_views_.reserve(swapchain_images_.size());
    for (const VkImage image : swapchain_images_) {
        const VkImageViewCreateInfo vi{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format_,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view = VK_NULL_HANDLE;
        if (const VkResult r = vkCreateImageView(device_, &vi, nullptr, &view); r != VK_SUCCESS) {
            err = "vkCreateImageView on a swapchain image failed";
            destroy_targets();
            return false;
        }
        swapchain_views_.push_back(view);
    }

    const VkImageCreateInfo rti{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format_,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // Same contract as the offscreen target (2.5/2.6): COLOR_ATTACHMENT for
        // passes, TRANSFER_SRC for the blit, TRANSFER_DST for capture_rgba8().
        // Format == the swapchain's, which is what makes the blit legal:
        // VK_IMAGE_BLIT_OPTIMAL requires format-compatible images.
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (const VkResult r = vkCreateImage(device_, &rti, nullptr, &rt_); r != VK_SUCCESS) {
        err = "vkCreateImage (WSI render target) failed";
        destroy_targets();
        return false;
    }
    VkMemoryRequirements rt_req{};
    vkGetImageMemoryRequirements(device_, rt_, &rt_req);
    const auto rt_type = find_memory_type(physical_, rt_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!rt_type) {
        err = "no device-local memory type for the WSI render target";
        destroy_targets();
        return false;
    }
    const VkMemoryAllocateInfo rt_ai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = rt_req.size,
        .memoryTypeIndex = *rt_type,
    };
    if (vkAllocateMemory(device_, &rt_ai, nullptr, &rt_memory_) != VK_SUCCESS) {
        err = "vkAllocateMemory (WSI render target) failed";
        destroy_targets();
        return false;
    }
    if (vkBindImageMemory(device_, rt_, rt_memory_, 0) != VK_SUCCESS) {
        err = "vkBindImageMemory (WSI render target) failed";
        destroy_targets();
        return false;
    }
    const VkImageViewCreateInfo rvi{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = rt_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format_,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (const VkResult r = vkCreateImageView(device_, &rvi, nullptr, &rt_view_); r != VK_SUCCESS) {
        err = "vkCreateImageView (WSI render target) failed";
        destroy_targets();
        return false;
    }
    // --- readback staging + command pool (capture_rgba8 and every helper blit)
    readback_size_ = static_cast<VkDeviceSize>(extent.width) * extent.height * 4u;
    const VkBufferCreateInfo rbi{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = readback_size_,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (vkCreateBuffer(device_, &rbi, nullptr, &readback_) != VK_SUCCESS) {
        err = "vkCreateBuffer (WSI readback) failed";
        destroy_targets();
        return false;
    }
    VkMemoryRequirements rb_req{};
    vkGetBufferMemoryRequirements(device_, readback_, &rb_req);
    const auto rb_type = find_memory_type(
        physical_, rb_req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!rb_type) {
        err = "no host-visible memory type for WSI readback";
        destroy_targets();
        return false;
    }
    const VkMemoryAllocateInfo rb_ai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = rb_req.size,
        .memoryTypeIndex = *rb_type,
    };
    if (vkAllocateMemory(device_, &rb_ai, nullptr, &readback_memory_) != VK_SUCCESS) {
        err = "vkAllocateMemory (WSI readback) failed";
        destroy_targets();
        return false;
    }
    if (vkBindBufferMemory(device_, readback_, readback_memory_, 0) != VK_SUCCESS) {
        err = "vkBindBufferMemory (WSI readback) failed";
        destroy_targets();
        return false;
    }
    const VkCommandPoolCreateInfo cpi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        // RESET_COMMAND_BUFFER_BIT: every helper here builds a fresh one-shot
        // buffer, exactly like the offscreen path (2.7's alloc/free pairing rule).
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_,
    };
    if (vkCreateCommandPool(device_, &cpi, nullptr, &pool_) != VK_SUCCESS) {
        err = "vkCreateCommandPool (WSI) failed";
        destroy_targets();
        return false;
    }

    width_ = extent.width;
    height_ = extent.height;
    rt_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

// Everything on the swapchain side. Order is load-bearing: the views belong to
// the swapchain's images, so they die first, and the swapchain itself has to die
// before the device does.
void wsi_presenter::destroy_targets() {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, pool_, nullptr); // frees its buffers too (2.7 rule)
        pool_ = VK_NULL_HANDLE;
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
    if (rt_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, rt_, nullptr);
        rt_ = VK_NULL_HANDLE;
    }
    if (rt_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, rt_memory_, nullptr);
        rt_memory_ = VK_NULL_HANDLE;
    }
    for (const VkImageView view : swapchain_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_views_.clear();
    swapchain_images_.clear(); // borrowed handles — the swapchain owns the images
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    width_ = 0;
    height_ = 0;
    acquired_ = false;
    in_flight_ = false;
    rt_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

// One frame in flight (2.8's stated budget; 2.10 doubles it): acquire signals
// image_available_, the blit signals render_finished_, and the fence on the
// present submit marks the whole frame done. Two semaphores rather than one
// because each crosses a different boundary — the first is handed to the WSI,
// the second only ever meets our own queue.
bool wsi_presenter::create_sync(std::string& err) {
    const VkSemaphoreCreateInfo sei{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    if (vkCreateSemaphore(device_, &sei, nullptr, &image_available_) != VK_SUCCESS) {
        err = "vkCreateSemaphore (image_available) failed";
        return false;
    }
    if (vkCreateSemaphore(device_, &sei, nullptr, &render_finished_) != VK_SUCCESS) {
        err = "vkCreateSemaphore (render_finished) failed";
        destroy_sync();
        return false;
    }
    // Unsignaled at creation: the first present's wait is armed by this frame's
    // blit, not by a previous one, which is exactly the one-in-flight start state.
    const VkFenceCreateInfo fei{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    if (vkCreateFence(device_, &fei, nullptr, &frame_fence_) != VK_SUCCESS) {
        err = "vkCreateFence (frame) failed";
        destroy_sync();
        return false;
    }
    return true;
}

void wsi_presenter::destroy_sync() {
    if (frame_fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, frame_fence_, nullptr);
        frame_fence_ = VK_NULL_HANDLE;
    }
    if (render_finished_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, render_finished_, nullptr);
        render_finished_ = VK_NULL_HANDLE;
    }
    if (image_available_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, image_available_, nullptr);
        image_available_ = VK_NULL_HANDLE;
    }
}

wsi_presenter::~wsi_presenter() {
    if (device_ == VK_NULL_HANDLE) {
        return; // moved-from, or never created
    }
    // Wait before tearing anything down: destroying a swapchain or image a queue
    // still touches is device-loss territory, not a leak.
    if (in_flight_ && frame_fence_ != VK_NULL_HANDLE) {
        vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, ~std::uint64_t{0});
    }
    vkDeviceWaitIdle(device_);
    destroy_targets();
    destroy_sync();
    // surface_ is the caller's (documented in the header) — never destroyed here.
    device_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
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
      readback_size_(std::exchange(other.readback_size_, 0)),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
      queue_family_(std::exchange(other.queue_family_, 0)),
      image_available_(std::exchange(other.image_available_, VK_NULL_HANDLE)),
      render_finished_(std::exchange(other.render_finished_, VK_NULL_HANDLE)),
      frame_fence_(std::exchange(other.frame_fence_, VK_NULL_HANDLE)),
      rt_layout_(other.rt_layout_),
      format_(other.format_),
      color_space_(other.color_space_),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)),
      current_(std::exchange(other.current_, 0)),
      acquired_(std::exchange(other.acquired_, false)),
      in_flight_(std::exchange(other.in_flight_, false)),
      stale_(std::exchange(other.stale_, false)) {
    other.swapchain_images_.clear();
    other.swapchain_views_.clear();
    other.rt_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    other.format_ = VK_FORMAT_UNDEFINED;
}

wsi_presenter& wsi_presenter::operator=(wsi_presenter&& other) noexcept {
    if (this != &other) {
        this->~wsi_presenter();
        new (this) wsi_presenter(std::move(other));
    }
    return *this;
}

// --- the frame loop -------------------------------------------------------
// One frame in flight (2.8's budget; 2.10 replaces this fence discipline at the
// same call sites): the CPU waits the previous blit's fence before touching the
// render target again. Two semaphores because they cross different boundaries —
// image_available_ is handed to the WSI by acquire, render_finished_ only ever
// meets our own queue and vkQueuePresentKHR.
std::expected<std::uint32_t, std::string> wsi_presenter::acquire() {
    if (stale_) {
        return std::unexpected("swapchain is stale — call recreate() before acquiring");
    }
    if (acquired_) {
        return std::unexpected("acquire() called while an image is already held — "
                               "one frame in flight: present() (or recreate()) first");
    }
    if (in_flight_) {
        if (vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, ~std::uint64_t{0}) != VK_SUCCESS) {
            return std::unexpected("waiting on the frame fence failed — device lost?");
        }
        if (vkResetFences(device_, 1, &frame_fence_) != VK_SUCCESS) {
            return std::unexpected("vkResetFences failed on the frame fence");
        }
        in_flight_ = false;
    }

    std::uint32_t index = 0;
    const VkResult r = vkAcquireNextImageKHR(device_, swapchain_, ~std::uint64_t{0}, image_available_,
                                             VK_NULL_HANDLE, &index);
    if (r == VK_TIMEOUT) {
        return std::unexpected("vkAcquireNextImageKHR timed out — nothing is presenting to this surface");
    }
    if (result_is_stale(r)) {
        // OUT_OF_DATE / SUBOPTIMAL: never swallowed (2.8's resize contract). The
        // acquisition semaphore may already be signaled, so the frame is dropped
        // outright and recreate() rebuilds the sync objects along with the chain.
        stale_ = true;
        return std::unexpected(std::string("surface changed at acquire (") + result_text(r) +
                               ") — call recreate()");
    }
    if (r != VK_SUCCESS) {
        return std::unexpected("vkAcquireNextImageKHR failed: " + result_text(r));
    }
    current_ = index;
    acquired_ = true;
    return index;
}

VkCommandBuffer wsi_presenter::begin_command() {
    if (pool_ == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    const VkCommandBuffer cmd = alloc_cmd(device_, pool_);
    if (cmd == VK_NULL_HANDLE || begin_one_shot(cmd) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return cmd;
}

bool wsi_presenter::begin_pass(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || rt_ == VK_NULL_HANDLE) {
        return false;
    }
    // The pass sees a color attachment — never a swapchain image (seam rule).
    // UNDEFINED as the source means "previous contents don't matter", which is
    // exactly right for the first frame onto a fresh target.
    transition(cmd, rt_, rt_layout_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rt_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return true;
}

bool wsi_presenter::end_pass(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || rt_ == VK_NULL_HANDLE) {
        return false;
    }
    if (rt_layout_ != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        VE_LOG_ERROR("gfx: end_pass with the render target in layout %d — expected COLOR_ATTACHMENT_OPTIMAL",
                     static_cast<int>(rt_layout_));
        return false;
    }
    // Straight into blit-ready: the frame's only other use for this image is as
    // the source of the present blit.
    transition(cmd, rt_, rt_layout_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    rt_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return true;
}

// The whole point of 2.8: the frame the pass drew into the render target is
// copied into a swapchain image the presenter acquired. The blit lives in the
// caller's command buffer so the pass and its present stay one submission —
// which is what lets a single fence cover the frame.
bool wsi_presenter::present(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE) {
        VE_LOG_ERROR("gfx: present() with no command buffer — nothing was recorded to blit");
        return false;
    }
    if (!acquired_) {
        VE_LOG_ERROR("gfx: present() without acquire() — no swapchain image is held");
        return false;
    }
    if (rt_layout_ != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        VE_LOG_ERROR("gfx: present() with the render target in layout %d — end_pass() first",
                     static_cast<int>(rt_layout_));
        return false;
    }
    if (current_ >= swapchain_images_.size()) {
        VE_LOG_ERROR("gfx: present() at image index %u, but the swapchain has %zu image(s)", current_,
                     swapchain_images_.size());
        return false;
    }
    const VkImage dst = swapchain_images_[current_];

    // The image arrives with undefined contents, so its first barrier starts from
    // UNDEFINED and its only usage flag is TRANSFER_DST (set at create time).
    transition(cmd, dst, kSwapchainEntryLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // The layouts this blit requires are exactly the two the barriers above put the
    // images in — VK_IMAGE_BLIT_OPTIMAL on both sides, same format, same extent.
    // The offsets are int32_t in the API while the extent is uint32_t, so the
    // casts are written out: implicit narrowing in a braced list is a hard error
    // on both toolchains, and a presenter-sized image never approaches 2^31.
    const std::int32_t w = static_cast<std::int32_t>(width_);
    const std::int32_t h = static_cast<std::int32_t>(height_);
    const VkImageBlit blit{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {w, h, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {w, h, 1}},
    };
    vkCmdBlitImage(cmd, rt_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        VE_LOG_ERROR("gfx: vkEndCommandBuffer failed on the frame's command buffer");
        acquired_ = false;
        stale_ = true;
        return false;
    }

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSemaphore waits[1] = {image_available_};
    const VkSemaphore signals[1] = {render_finished_};
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = waits,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = signals,
    };
    if (vkQueueSubmit(queue_, 1, &si, frame_fence_) != VK_SUCCESS) {
        VE_LOG_ERROR("gfx: vkQueueSubmit failed on the frame — acquire abandoned, recreate required");
        acquired_ = false;
        stale_ = true;
        return false;
    }
    in_flight_ = true;

    VkPresentInfoKHR pi{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = signals,
        .swapchainCount = 1,
        .pSwapchains = &swapchain_,
        .pImageIndices = &current_,
        .pResults = nullptr,
    };
    const VkResult r = vkQueuePresentKHR(queue_, &pi);
    acquired_ = false; // the image is the WSI's now, presented or abandoned
    if (r == VK_SUBOPTIMAL_KHR) {
        // The frame DID go up; the chain just no longer fits the surface
        // (rotated/resized mid-frame). Report success, insist on a rebuild.
        stale_ = true;
        VE_LOG_WARN("gfx: presented, but the swapchain is suboptimal — recreate() next frame");
        return true;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        stale_ = true;
        VE_LOG_WARN("gfx: present rejected as OUT_OF_DATE — recreate() required");
        return false;
    }
    if (r != VK_SUCCESS) {
        stale_ = true;
        VE_LOG_ERROR("gfx: vkQueuePresentKHR failed: %s", result_text(r).c_str());
        return false;
    }
    return true;
}

// Readback of the render target — the same Tier-2 capture path the offscreen
// presenter uses, so a WSI frame is checkable with the identical helper. Call it
// outside a pass (after present(), before the next begin_pass); calling it
// mid-pass would strip the target out of attachment layout under a live pass.
std::vector<std::uint8_t> wsi_presenter::capture_rgba8() {
    if (device_ == VK_NULL_HANDLE || rt_ == VK_NULL_HANDLE || readback_ == VK_NULL_HANDLE ||
        pool_ == VK_NULL_HANDLE) {
        return {};
    }
    if (in_flight_) {
        if (vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, ~std::uint64_t{0}) != VK_SUCCESS) {
            VE_LOG_ERROR("gfx: capture_rgba8 could not wait on the frame fence");
            return {};
        }
        vkResetFences(device_, 1, &frame_fence_);
        in_flight_ = false;
    }

    const VkCommandBuffer cmd = begin_command();
    if (cmd == VK_NULL_HANDLE) {
        VE_LOG_ERROR("gfx: capture_rgba8 could not allocate a command buffer");
        return {};
    }
    transition(cmd, rt_, rt_layout_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    rt_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0, // 0 = tightly packed: width*4 bytes per row, which capture assumes
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width_, height_, 1},
    };
    vkCmdCopyImageToBuffer(cmd, rt_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &region);

    if (!submit_and_wait(queue_, cmd)) {
        VE_LOG_ERROR("gfx: capture_rgba8 transfer failed");
        vkFreeCommandBuffers(device_, pool_, 1, &cmd);
        return {};
    }
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);

    void* pixels = nullptr;
    if (vkMapMemory(device_, readback_memory_, 0, readback_size_, 0, &pixels) != VK_SUCCESS ||
        pixels == nullptr) {
        VE_LOG_ERROR("gfx: capture_rgba8 could not map readback memory");
        return {};
    }
    std::vector<std::uint8_t> out(readback_size_);
    std::memcpy(out.data(), pixels, readback_size_);
    vkUnmapMemory(device_, readback_memory_);
    return out;
}

// "resize → VB + recreate" from the checklist. The 2.6 pipeline keeps viewport
// and scissor DYNAMIC and its vertex data in NDC, so a resize rebuilds the
// swapchain, the render target and their views — no pipeline rebuild, no vertex
// buffer re-upload. Rebuilding the sync objects too is not decoration: a stale
// acquire can leave image_available_ signaled with no frame waiting on it, and
// carrying that into a fresh chain is how you get a frame that renders over top
// of one still in flight.
bool wsi_presenter::recreate(std::uint32_t width, std::uint32_t height) {
    if (device_ == VK_NULL_HANDLE || surface_ == VK_NULL_HANDLE) {
        VE_LOG_ERROR("gfx: recreate() on a presenter that was never created (or was moved from)");
        return false;
    }
    if (acquired_) {
        VE_LOG_ERROR("gfx: recreate() while holding an acquired image — present() the frame first");
        return false;
    }
    if (in_flight_) {
        if (vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, ~std::uint64_t{0}) != VK_SUCCESS) {
            VE_LOG_ERROR("gfx: recreate() could not wait on the frame fence");
            return false;
        }
        vkResetFences(device_, 1, &frame_fence_);
        in_flight_ = false;
    }
    if (vkDeviceWaitIdle(device_) != VK_SUCCESS) {
        VE_LOG_ERROR("gfx: recreate() could not idle the device — nothing may touch the old chain");
        return false;
    }

    destroy_targets();
    destroy_sync();

    std::string err;
    if (!create_targets(width, height, err)) {
        stale_ = true;
        VE_LOG_ERROR("gfx: swapchain rebuild failed: %s", err.c_str());
        return false;
    }
    if (!create_sync(err)) {
        destroy_targets();
        stale_ = true;
        VE_LOG_ERROR("gfx: swapchain rebuild lost its sync objects: %s", err.c_str());
        return false;
    }
    stale_ = false;
    VE_LOG_INFO("gfx: WSI chain rebuilt at %ux%u", width_, height_);
    return true;
}

} // namespace ve::gfx









