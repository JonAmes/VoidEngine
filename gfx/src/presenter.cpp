#include <ve/gfx/presenter.h>

#include <optional>
#include <utility>

namespace ve::gfx {

namespace {

std::optional<std::uint32_t> find_memory_type(const device& dev, std::uint32_t type_bits,
                                              VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(dev.physical(), &mem_props);
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0u &&
            (mem_props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

std::expected<offscreen_presenter, std::string> offscreen_presenter::create(
    const device& dev, std::uint32_t width, std::uint32_t height, VkFormat format) {
    offscreen_presenter out;
    out.device_ = dev.handle();
    out.queue_ = dev.graphics_queue();
    out.format_ = format;
    out.width_ = width;
    out.height_ = height;
    const std::size_t byte_size = static_cast<std::size_t>(width) * height * 4u;

    const VkImageCreateInfo image_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // COLOR_ATTACHMENT: the 2.6 render pass writes this same image.
        // TRANSFER_SRC: Tier-2 "present" copies it to the readback buffer.
        // SAMPLED: an offscreen target doubles as a world-space canvas
        // (3.14) — a later scene pass samples its color view.
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (const VkResult r = vkCreateImage(out.device_, &image_ci, nullptr, &out.image_); r != VK_SUCCESS) {
        return std::unexpected("vkCreateImage failed: VkResult(" + std::to_string(static_cast<int>(r)) + ")");
    }

    VkMemoryRequirements image_reqs{};
    vkGetImageMemoryRequirements(out.device_, out.image_, &image_reqs);
    const auto image_mt = find_memory_type(dev, image_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!image_mt) {
        vkDestroyImage(out.device_, out.image_, nullptr);
        return std::unexpected("no DEVICE_LOCAL memory type for color image");
    }
    const VkMemoryAllocateInfo image_alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = image_reqs.size,
        .memoryTypeIndex = *image_mt,
    };
    if (const VkResult r = vkAllocateMemory(out.device_, &image_alloc, nullptr, &out.image_memory_); r != VK_SUCCESS) {
        vkDestroyImage(out.device_, out.image_, nullptr);
        return std::unexpected("image memory alloc failed");
    }
    vkBindImageMemory(out.device_, out.image_, out.image_memory_, 0);

    const VkImageViewCreateInfo view_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = out.image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (const VkResult r = vkCreateImageView(out.device_, &view_ci, nullptr, &out.view_); r != VK_SUCCESS) {
        vkFreeMemory(out.device_, out.image_memory_, nullptr);
        vkDestroyImage(out.device_, out.image_, nullptr);
        return std::unexpected("vkCreateImageView failed");
    }

    const VkBufferCreateInfo readback_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = byte_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (const VkResult r = vkCreateBuffer(out.device_, &readback_ci, nullptr, &out.readback_); r != VK_SUCCESS) {
        vkDestroyImageView(out.device_, out.view_, nullptr);
        vkFreeMemory(out.device_, out.image_memory_, nullptr);
        vkDestroyImage(out.device_, out.image_, nullptr);
        return std::unexpected("vkCreateBuffer failed");
    }
    VkMemoryRequirements buf_reqs{};
    vkGetBufferMemoryRequirements(out.device_, out.readback_, &buf_reqs);
    // HOST_COHERENT: readback is read right after QueueWaitIdle — coherent
    // removes cache-flush bookkeeping from the Tier-2 path entirely.
    const auto buf_mt = find_memory_type(
        dev, buf_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!buf_mt) {
        vkDestroyBuffer(out.device_, out.readback_, nullptr);
        vkDestroyImageView(out.device_, out.view_, nullptr);
        vkFreeMemory(out.device_, out.image_memory_, nullptr);
        vkDestroyImage(out.device_, out.image_, nullptr);
        return std::unexpected("no HOST_VISIBLE|HOST_COHERENT memory type for readback");
    }
    const VkMemoryAllocateInfo buf_alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = buf_reqs.size,
        .memoryTypeIndex = *buf_mt,
    };
    if (const VkResult r = vkAllocateMemory(out.device_, &buf_alloc, nullptr, &out.readback_memory_); r != VK_SUCCESS) {
        vkDestroyBuffer(out.device_, out.readback_, nullptr);
        vkDestroyImageView(out.device_, out.view_, nullptr);
        vkFreeMemory(out.device_, out.image_memory_, nullptr);
        vkDestroyImage(out.device_, out.image_, nullptr);
        return std::unexpected("readback memory alloc failed");
    }
    vkBindBufferMemory(out.device_, out.readback_, out.readback_memory_, 0);

    // 3.3 — the depth attachment: always present on an offscreen target.
    // teardown_prefix frees everything created before the depth stage; the
    // depth stage's own partials are added to each failure branch.
    auto teardown_prefix = [&]() {
        vkFreeMemory(out.device_, out.readback_memory_, nullptr);
        vkDestroyBuffer(out.device_, out.readback_, nullptr);
        vkDestroyImageView(out.device_, out.view_, nullptr);
        vkFreeMemory(out.device_, out.image_memory_, nullptr);
        vkDestroyImage(out.device_, out.image_, nullptr);
    };

    const VkImageCreateInfo depth_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = kDepthFormat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // DEPTH_STENCIL_ATTACHMENT: mandatory usage for depth images.
        // TRANSFER_SRC: capture_depth_f32 (reversed-Z texel pins) reads it
        // back. SAMPLED: 3.11 shadow maps — a depth target can BE a shadow
        // map: another pass samples it via expose_depth_for_sampling().
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (const VkResult r = vkCreateImage(out.device_, &depth_ci, nullptr, &out.depth_image_); r != VK_SUCCESS) {
        teardown_prefix();
        return std::unexpected("vkCreateImage(depth) failed: VkResult(" + std::to_string(static_cast<int>(r)) + ")");
    }
    VkMemoryRequirements depth_reqs{};
    vkGetImageMemoryRequirements(out.device_, out.depth_image_, &depth_reqs);
    const auto depth_mt = find_memory_type(dev, depth_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!depth_mt) {
        vkDestroyImage(out.device_, out.depth_image_, nullptr);
        teardown_prefix();
        return std::unexpected("no DEVICE_LOCAL memory type for depth image");
    }
    {
        const VkMemoryAllocateInfo alloc{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = depth_reqs.size,
            .memoryTypeIndex = *depth_mt,
        };
        if (const VkResult r = vkAllocateMemory(out.device_, &alloc, nullptr, &out.depth_memory_); r != VK_SUCCESS) {
            vkDestroyImage(out.device_, out.depth_image_, nullptr);
            teardown_prefix();
            return std::unexpected("depth image memory alloc failed");
        }
    }
    vkBindImageMemory(out.device_, out.depth_image_, out.depth_memory_, 0);

    const VkImageViewCreateInfo depth_view_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = out.depth_image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = kDepthFormat,
        .components = {},
        // DEPTH aspect only: stencil does not exist for D32_SFLOAT.
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    if (const VkResult r = vkCreateImageView(out.device_, &depth_view_ci, nullptr, &out.depth_view_); r != VK_SUCCESS) {
        vkFreeMemory(out.device_, out.depth_memory_, nullptr);
        vkDestroyImage(out.device_, out.depth_image_, nullptr);
        teardown_prefix();
        return std::unexpected("vkCreateImageView(depth) failed");
    }

    const VkBufferCreateInfo depth_readback_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = byte_size, // D32_SFLOAT texel = 4 bytes, packed rows
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (const VkResult r = vkCreateBuffer(out.device_, &depth_readback_ci, nullptr, &out.depth_readback_); r != VK_SUCCESS) {
        vkDestroyImageView(out.device_, out.depth_view_, nullptr);
        vkFreeMemory(out.device_, out.depth_memory_, nullptr);
        vkDestroyImage(out.device_, out.depth_image_, nullptr);
        teardown_prefix();
        return std::unexpected("vkCreateBuffer(depth readback) failed");
    }
    VkMemoryRequirements depth_buf_reqs{};
    vkGetBufferMemoryRequirements(out.device_, out.depth_readback_, &depth_buf_reqs);
    const auto depth_buf_mt = find_memory_type(dev, depth_buf_reqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!depth_buf_mt) {
        vkDestroyBuffer(out.device_, out.depth_readback_, nullptr);
        vkDestroyImageView(out.device_, out.depth_view_, nullptr);
        vkFreeMemory(out.device_, out.depth_memory_, nullptr);
        vkDestroyImage(out.device_, out.depth_image_, nullptr);
        teardown_prefix();
        return std::unexpected("no HOST_VISIBLE|HOST_COHERENT memory type for depth readback");
    }
    {
        const VkMemoryAllocateInfo alloc{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = depth_buf_reqs.size,
            .memoryTypeIndex = *depth_buf_mt,
        };
        if (const VkResult r = vkAllocateMemory(out.device_, &alloc, nullptr, &out.depth_readback_memory_); r != VK_SUCCESS) {
            vkDestroyBuffer(out.device_, out.depth_readback_, nullptr);
            vkDestroyImageView(out.device_, out.depth_view_, nullptr);
            vkFreeMemory(out.device_, out.depth_memory_, nullptr);
            vkDestroyImage(out.device_, out.depth_image_, nullptr);
            teardown_prefix();
            return std::unexpected("depth readback memory alloc failed");
        }
    }
    vkBindBufferMemory(out.device_, out.depth_readback_, out.depth_readback_memory_, 0);

    const VkCommandPoolCreateInfo pool_ci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = dev.graphics_queue_family(),
    };
    if (const VkResult r = vkCreateCommandPool(out.device_, &pool_ci, nullptr, &out.pool_); r != VK_SUCCESS) {
        vkFreeMemory(out.device_, out.depth_readback_memory_, nullptr);
        vkDestroyBuffer(out.device_, out.depth_readback_, nullptr);
        vkDestroyImageView(out.device_, out.depth_view_, nullptr);
        vkFreeMemory(out.device_, out.depth_memory_, nullptr);
        vkDestroyImage(out.device_, out.depth_image_, nullptr);
        teardown_prefix();
        return std::unexpected("vkCreateCommandPool failed");
    }

    // Frame sync (2.10): one fence per slot, created signaled; the per-slot
    // pending-command slots start empty.
    auto fs = frame_sync::create(out.device_, kFramesInFlight);
    if (!fs) {
        vkDestroyCommandPool(out.device_, out.pool_, nullptr);
        vkFreeMemory(out.device_, out.depth_readback_memory_, nullptr);
        vkDestroyBuffer(out.device_, out.depth_readback_, nullptr);
        vkDestroyImageView(out.device_, out.depth_view_, nullptr);
        vkFreeMemory(out.device_, out.depth_memory_, nullptr);
        vkDestroyImage(out.device_, out.depth_image_, nullptr);
        teardown_prefix();
        return std::unexpected(fs.error());
    }
    out.sync_ = std::move(*fs);
    out.slot_cmd_.assign(kFramesInFlight, VK_NULL_HANDLE);

    return out;
}

offscreen_presenter::~offscreen_presenter() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    vkDestroyCommandPool(device_, pool_, nullptr);
    vkFreeMemory(device_, depth_readback_memory_, nullptr);
    vkDestroyBuffer(device_, depth_readback_, nullptr);
    vkFreeMemory(device_, readback_memory_, nullptr);
    vkDestroyBuffer(device_, readback_, nullptr);
    vkDestroyImageView(device_, depth_view_, nullptr);
    vkFreeMemory(device_, depth_memory_, nullptr);
    vkDestroyImage(device_, depth_image_, nullptr);
    vkDestroyImageView(device_, view_, nullptr);
    vkFreeMemory(device_, image_memory_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
}

offscreen_presenter::offscreen_presenter(offscreen_presenter&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      queue_(std::exchange(other.queue_, VK_NULL_HANDLE)),
      image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      image_memory_(std::exchange(other.image_memory_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)),
      depth_image_(std::exchange(other.depth_image_, VK_NULL_HANDLE)),
      depth_memory_(std::exchange(other.depth_memory_, VK_NULL_HANDLE)),
      depth_view_(std::exchange(other.depth_view_, VK_NULL_HANDLE)),
      readback_(std::exchange(other.readback_, VK_NULL_HANDLE)),
      readback_memory_(std::exchange(other.readback_memory_, VK_NULL_HANDLE)),
      depth_readback_(std::exchange(other.depth_readback_, VK_NULL_HANDLE)),
      depth_readback_memory_(std::exchange(other.depth_readback_memory_, VK_NULL_HANDLE)),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
      sync_(std::move(other.sync_)),
      slot_cmd_(std::move(other.slot_cmd_)),
      layout_(other.layout_),
      depth_layout_(other.depth_layout_),
      format_(other.format_),
      width_(other.width_),
      height_(other.height_) {
    other.slot_cmd_.clear();
}

namespace {

VkCommandBuffer alloc_cmd(VkDevice device, VkCommandPool pool) {
    const VkCommandBufferAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &ai, &cmd);
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

// Allocate AND begin: for internal one-op uses (clear, capture) whose
// caller never touches the buffer. The frame helper begin_command() must
// NOT use this — it would begin twice (VUID-vkBeginCommandBuffer-...-00049).
VkCommandBuffer alloc_one_shot(VkDevice device, VkCommandPool pool) {
    VkCommandBuffer cmd = alloc_cmd(device, pool);
    if (cmd != VK_NULL_HANDLE && begin_one_shot(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

void submit_and_wait(VkQueue queue, VkCommandBuffer cmd) {
    // End + submit + wait only: whoever allocated the buffer frees it
    // (double-free is a validation failure, proven by the strict messenger).
    // QueueWaitIdle covers this queue's work.
    vkEndCommandBuffer(cmd);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
}

} // namespace

bool offscreen_presenter::clear(const float rgba[4]) {
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkCommandBuffer cmd = alloc_one_shot(device_, pool_);
    if (cmd == VK_NULL_HANDLE) {
        return false;
    }

    // UNDEFINED (or whatever pass left it in) -> TRANSFER_DST. Old layout is
    // bookkept, never guessed; UNDEFINED old = "content discarded", which is
    // exactly what clear means.
    const VkImageMemoryBarrier to_dst{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = layout_,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image_,
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_dst);

    const VkClearColorValue color{{rgba[0], rgba[1], rgba[2], rgba[3]}};
    vkCmdClearColorImage(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

    const VkImageMemoryBarrier to_src{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image_,
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_src);

    submit_and_wait(queue_, cmd);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
    layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return true;
}

// --- Tier-2 frame helpers: presenter-owned layout transitions -------------

VkCommandBuffer offscreen_presenter::begin_command() {
    // Slot discipline (2.10): this slot's PREVIOUS frame must be complete
    // (fence) before we re-arm it — and that wait is what makes freeing that
    // frame's command buffer legal right here (freeing a still-executing
    // buffer is a validation error).
    if (!sync_.wait_current() || !sync_.reset_current()) {
        return VK_NULL_HANDLE;
    }
    if (VkCommandBuffer& pending = slot_cmd_[sync_.current()]; pending != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, pool_, 1, &pending);
        pending = VK_NULL_HANDLE;
    }
    // alloc + exactly one begin (alloc_one_shot would begin a second time).
    VkCommandBuffer cmd = alloc_cmd(device_, pool_);
    if (cmd == VK_NULL_HANDLE) {
        return cmd;
    }
    if (begin_one_shot(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, pool_, 1, &cmd);
        return VK_NULL_HANDLE;
    }
    slot_cmd_[sync_.current()] = cmd; // freed at this slot's NEXT begin
    return cmd;
}

bool offscreen_presenter::begin_pass(VkCommandBuffer cmd) {
    // Bookkept old layout (never guessed). From UNDEFINED the contents are
    // discarded — a pass using LOAD_OP_CLEAR makes that legal and cheap.
    const VkImageMemoryBarrier to_att{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = layout_,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image_,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_att);
    layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Depth side of the same pass-begin (3.3): presenter-owned transition,
    // same bookkept-old-layout discipline. UNDEFINED old = contents
    // discarded, legal under LOAD_OP_CLEAR (passes clear depth to 0 =
    // reversed-Z far every frame).
    const VkAccessFlags depth_src_access =
        depth_layout_ == VK_IMAGE_LAYOUT_UNDEFINED
            ? VkAccessFlags{0}
            : depth_layout_ == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                  ? VkAccessFlags{VK_ACCESS_SHADER_READ_BIT} // sampled last (3.11)
                  : VkAccessFlags{VK_ACCESS_TRANSFER_READ_BIT}; // came from a capture
    const VkPipelineStageFlags depth_src_stage =
        depth_layout_ == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            : depth_layout_ == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                  ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                  : VK_PIPELINE_STAGE_TRANSFER_BIT; // TRANSFER_READ only legal there
    const VkImageMemoryBarrier depth_to_att{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = depth_src_access,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = depth_layout_,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = depth_image_,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, depth_src_stage,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &depth_to_att);
    depth_layout_ = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    return true;
}

bool offscreen_presenter::end_pass(VkCommandBuffer cmd) {
    const VkImageMemoryBarrier to_src{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = layout_,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image_,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_src);
    layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    const VkImageMemoryBarrier depth_to_src{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = depth_layout_,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = depth_image_,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &depth_to_src);
    depth_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return true;
}

// 3.11 shadow-map plumbing: a rendered depth target becomes a sampled
// shadow map. Owner-side only (the presenter owns its layouts, per the
// class contract). Call after end_pass — the TRANSFER_SRC side of the
// end_pass barrier already orders the depth writes, so the source stage
// here is TRANSFER with TRANSFER_READ access (the bookkept state).
bool offscreen_presenter::expose_color_for_sampling(VkCommandBuffer cmd) {
    // 3.14: make this target's color image sampleable by the NEXT pass
    // (world canvas). Caller must have ended the filling pass; end_pass
    // leaves layout_ in COLOR_ATTACHMENT_OPTIMAL (capture paths move it to
    // TRANSFER_SRC and restore is not needed here — reading it again re-
    // exposes).
    const VkImageMemoryBarrier to_read{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = layout_,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image_,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_read);
    layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

bool offscreen_presenter::expose_depth_for_sampling(VkCommandBuffer cmd) {
    const VkImageMemoryBarrier to_read{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = depth_layout_,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = depth_image_,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_read);
    depth_layout_ = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    return true;
}

bool offscreen_presenter::submit(VkCommandBuffer cmd) {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return false;
    }
    // Queue submit needs the handle on its own storage (pointer-to-pointer).
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
    // The slot fence carries the frame (2.10): the driver signals it on
    // completion and the slot's next begin_command() waits it. No
    // QueueWaitIdle — that stand-in is exactly what 2.10 removes. The buffer
    // stays allocated: whoever allocated frees, at the slot's next begin.
    if (vkQueueSubmit(queue_, 1, &si, sync_.fence()) != VK_SUCCESS) {
        return false;
    }
    sync_.advance();
    return true;
}

std::vector<std::uint8_t> offscreen_presenter::capture_rgba8() {
    const std::size_t byte_size = static_cast<std::size_t>(width_) * height_ * 4u;
    std::vector<std::uint8_t> out;

    VkCommandBuffer cmd = alloc_one_shot(device_, pool_);
    if (cmd == VK_NULL_HANDLE) {
        return out;
    }

    if (layout_ != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        // A pass (2.6) may have left it COLOR_ATTACHMENT_OPTIMAL; bring it
        // to the copy source layout before reading.
        const VkImageMemoryBarrier to_src{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = layout_,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image_,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
        layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    // Tight packing: bufferRowLength/Height 0 = packed rows (exact 4B/px).
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width_, height_, 1},
    };
    vkCmdCopyImageToBuffer(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &region);

    // Make transfer writes visible to the host before we map.
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
    if (const VkResult r = vkMapMemory(device_, readback_memory_, 0, byte_size, 0, &mapped); r != VK_SUCCESS) {
        return {};
    }
    out.assign(static_cast<std::uint8_t*>(mapped), static_cast<std::uint8_t*>(mapped) + byte_size);
    vkUnmapMemory(device_, readback_memory_);
    return out;
}

std::vector<float> offscreen_presenter::capture_depth_f32() {
    // Same copy machinery as capture_rgba8, on the DEPTH aspect. Pin-only
    // tooling: no consumer code path may depend on it existing.
    const std::size_t byte_size = static_cast<std::size_t>(width_) * height_ * 4u;
    std::vector<float> out;

    VkCommandBuffer cmd = alloc_one_shot(device_, pool_);
    if (cmd == VK_NULL_HANDLE) {
        return out;
    }

    if (depth_layout_ != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        const VkImageMemoryBarrier to_src{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = depth_layout_,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = depth_image_,
            .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
        depth_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width_, height_, 1},
    };
    vkCmdCopyImageToBuffer(cmd, depth_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           depth_readback_, 1, &region);

    const VkBufferMemoryBarrier to_host{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = depth_readback_,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &to_host, 0, nullptr);

    submit_and_wait(queue_, cmd);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);

    void* mapped = nullptr;
    if (const VkResult r = vkMapMemory(device_, depth_readback_memory_, 0, byte_size, 0, &mapped); r != VK_SUCCESS) {
        return {};
    }
    const auto* first = static_cast<float*>(mapped);
    out.assign(first, first + width_ * height_);
    vkUnmapMemory(device_, depth_readback_memory_);
    return out;
}

} // namespace ve::gfx
