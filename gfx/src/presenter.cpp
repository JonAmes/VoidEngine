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
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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

    const VkCommandPoolCreateInfo pool_ci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = dev.graphics_queue_family(),
    };
    if (const VkResult r = vkCreateCommandPool(out.device_, &pool_ci, nullptr, &out.pool_); r != VK_SUCCESS) {
        vkFreeMemory(out.device_, out.readback_memory_, nullptr);
        vkDestroyBuffer(out.device_, out.readback_, nullptr);
        vkDestroyImageView(out.device_, out.view_, nullptr);
        vkFreeMemory(out.device_, out.image_memory_, nullptr);
        vkDestroyImage(out.device_, out.image_, nullptr);
        return std::unexpected("vkCreateCommandPool failed");
    }

    return out;
}

offscreen_presenter::~offscreen_presenter() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    vkDestroyCommandPool(device_, pool_, nullptr);
    vkFreeMemory(device_, readback_memory_, nullptr);
    vkDestroyBuffer(device_, readback_, nullptr);
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
      readback_(std::exchange(other.readback_, VK_NULL_HANDLE)),
      readback_memory_(std::exchange(other.readback_memory_, VK_NULL_HANDLE)),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
      layout_(other.layout_),
      format_(other.format_),
      width_(other.width_),
      height_(other.height_) {}

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
    // alloc + exactly one begin (alloc_one_shot would begin a second time).
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
    if (vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        return false;
    }
    const bool ok = (vkQueueWaitIdle(queue_) == VK_SUCCESS);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
    return ok;
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

} // namespace ve::gfx
