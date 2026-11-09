#include "ve/gfx/texture.h"

#include <ve/core/cooked.h>

#include <algorithm>
#include <cstring>
#include <optional>

namespace ve::gfx {

using ve::core::cooked::Bytes;
using ve::core::cooked::Unit;

namespace {
std::uint32_t get_u32(const Bytes& b, std::size_t& p, bool& bad) {
    if (p + 4 > b.size()) { bad = true; return 0; }
    std::uint32_t v = b[p] | (std::uint32_t(b[p + 1]) << 8) |
                      (std::uint32_t(b[p + 2]) << 16) | (std::uint32_t(b[p + 3]) << 24);
    p += 4;
    return v;
}
} // namespace

std::expected<CpuTexture, std::string>
load_texture_by_id(const std::string& cooked_dir, const std::string& content_id) {
    ve::core::cooked::Manifest manifest;
    if (std::string err;
        !ve::core::cooked::read_manifest(cooked_dir + "/manifest.bin", manifest, &err)) {
        return std::unexpected("texture load: " + err);
    }
    if (!manifest.contains(content_id)) {
        return std::unexpected("texture load: '" + content_id + "' not in manifest");
    }
    CpuTexture out;
    std::string err;
    if (!load_cooked_texture_file(cooked_dir + "/units/" + content_id + ".ck", out, &err)) {
        return std::unexpected("texture load: " + err);
    }
    return out;
}

bool load_cooked_texture_file(const std::string& unit_path, CpuTexture& out,
                              std::string* err) {
    Unit unit;
    if (!ve::core::cooked::read_unit_file(unit_path, unit, err)) return false;
    if (unit.type != ve::core::cooked::kUnitTex0) {
        if (err) *err = "not a TEX0 unit: " + unit_path;
        return false;
    }
    bool bad = false;
    std::size_t p = 0;
    out.width = get_u32(unit.payload, p, bad);
    out.height = get_u32(unit.payload, p, bad);
    out.format = get_u32(unit.payload, p, bad);
    if (bad || out.format != ve::core::cooked::kTexFmtRgba8) {
        if (err) *err = "bad TEX0 header: " + unit_path;
        return false;
    }
    // overflow-safe expected size (fp64 avoided: integer bound check)
    const std::uint64_t want =
        std::uint64_t(out.width) * out.height * 4;
    if (want != unit.payload.size() - p) {
        if (err) *err = "TEX0 payload size mismatch: " + unit_path;
        return false;
    }
    out.pixels.assign(unit.payload.begin() + static_cast<std::ptrdiff_t>(p),
                      unit.payload.end());
    return true;
}

// ---------------------------------------------------------------------------
// GPU side (3.7)

namespace {

std::optional<std::uint32_t> find_memory_type(const device& dev, std::uint32_t type_bits,
                                              VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(dev.physical(), &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return std::nullopt;
}

// Owner-side layout transition (the gpu_texture half of the policy: consumers
// never barrier this image). Source stage/access MUST match the previous use
// — the 3.3 lesson (VUID-02819) — except for UNDEFINED oldLayout, where no
// previous content exists and no source access is claimed.
void image_barrier(VkCommandBuffer cmd, VkImage img,
                   std::uint32_t base_mip, std::uint32_t level_count,
                   VkImageLayout old_l, VkImageLayout new_l,
                   VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                   VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    const VkImageMemoryBarrier b{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_l,
        .newLayout = new_l,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = base_mip,
            .levelCount = level_count,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

std::uint32_t mip_dim(std::uint32_t base, std::uint32_t level) {
    return std::max(1u, base >> level);
}

} // namespace

std::expected<gpu_texture, std::string> gpu_texture::create(const device& dev,
                                                            const CpuTexture& src) {
    gpu_texture t;
    if (src.width == 0 || src.height == 0 ||
        src.pixels.size() != std::size_t(src.width) * src.height * 4) {
        return std::unexpected("gpu_texture: bad CPU texture (w/h/pixels)");
    }
    t.dev_ = &dev;
    t.width_ = src.width;
    t.height_ = src.height;
    std::uint32_t levels = 1;
    // A level exists as long as the level BEFORE it was still >1 texel wide
    // (mip k = max(1, base>>k)); testing the candidate itself would stop one
    // early and drop the 1x1 tail level.
    while (mip_dim(t.width_, levels - 1) > 1 || mip_dim(t.height_, levels - 1) > 1) ++levels;
    t.mip_levels_ = levels;

    // ---- image -------------------------------------------------------------
    const VkImageCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, // sRGB tagging is 3.10 material work
        .extent = {t.width_, t.height_, 1},
        .mipLevels = levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // TRANSFER_SRC from day one: the mip blits AND capture readback use it
        // (same "keep the usage bits stable" stance as gpu_mesh, 3.5).
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0, .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (const VkResult r = vkCreateImage(dev.handle(), &ici, nullptr, &t.image_); r != VK_SUCCESS) {
        return std::unexpected("gpu_texture: vkCreateImage failed (" +
                               std::to_string(static_cast<int>(r)) + ")");
    }

    VkMemoryRequirements reqs{};
vkGetImageMemoryRequirements(dev.handle(), t.image_, &reqs);
    // device-local preferred (sampling side), host-visible as fallback — on
    // lavapipe both flags sit on the same type.
    auto mt = find_memory_type(dev, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!mt) {
        mt = find_memory_type(dev, reqs.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (!mt) {
        vkDestroyImage(dev.handle(), t.image_, nullptr);
        t.image_ = VK_NULL_HANDLE;
        return std::unexpected("gpu_texture: no usable image memory type");
    }
    const VkMemoryAllocateInfo mai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr, .allocationSize = reqs.size, .memoryTypeIndex = *mt,
    };
    if (const VkResult r = vkAllocateMemory(dev.handle(), &mai, nullptr, &t.mem_); r != VK_SUCCESS) {
        vkDestroyImage(dev.handle(), t.image_, nullptr);
        t.image_ = VK_NULL_HANDLE;
        return std::unexpected("gpu_texture: vkAllocateMemory failed");
    }
    if (const VkResult r = vkBindImageMemory(dev.handle(), t.image_, t.mem_, 0);
        r != VK_SUCCESS) {
        t.destroy();
        return std::unexpected("gpu_texture: vkBindImageMemory failed");
    }

    // ---- staging buffer ------------------------------------------------------
    const VkDeviceSize px_bytes = std::size_t(t.width_) * t.height_ * 4;
    const VkBufferCreateInfo sbci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .size = px_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0, .pQueueFamilyIndices = nullptr,
    };
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateBuffer(dev.handle(), &sbci, nullptr, &staging); r != VK_SUCCESS) {
        t.destroy();
        return std::unexpected("gpu_texture: staging vkCreateBuffer failed");
    }
vkGetBufferMemoryRequirements(dev.handle(), staging, &reqs);
    const auto smt = find_memory_type(dev, reqs.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!smt) {
        vkDestroyBuffer(dev.handle(), staging, nullptr);
        t.destroy();
        return std::unexpected("gpu_texture: no host-visible staging memory");
    }
    const VkMemoryAllocateInfo smai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr, .allocationSize = reqs.size, .memoryTypeIndex = *smt,
    };
    if (const VkResult r = vkAllocateMemory(dev.handle(), &smai, nullptr, &staging_mem);
        r != VK_SUCCESS) {
        vkDestroyBuffer(dev.handle(), staging, nullptr);
        t.destroy();
        return std::unexpected("gpu_texture: staging vkAllocateMemory failed");
    }
    if (const VkResult r = vkBindBufferMemory(dev.handle(), staging, staging_mem, 0);
        r != VK_SUCCESS) {
        vkDestroyBuffer(dev.handle(), staging, nullptr);
        vkFreeMemory(dev.handle(), staging_mem, nullptr);
        t.destroy();
        return std::unexpected("gpu_texture: staging vkBindBufferMemory failed");
    }
    {
        void* mapped = nullptr;
        vkMapMemory(dev.handle(), staging_mem, 0, px_bytes, 0, &mapped);
        std::memcpy(mapped, src.pixels.data(), px_bytes);
        vkUnmapMemory(dev.handle(), staging_mem);
    }

    // ---- one-shot upload command buffer --------------------------------------
    const VkCommandPoolCreateInfo cpci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = dev.graphics_queue_family(),
    };
    VkCommandPool cpool = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateCommandPool(dev.handle(), &cpci, nullptr, &cpool);
        r != VK_SUCCESS) {
        vkDestroyBuffer(dev.handle(), staging, nullptr);
        vkFreeMemory(dev.handle(), staging_mem, nullptr);
        t.destroy();
        return std::unexpected("gpu_texture: vkCreateCommandPool failed");
    }
    const VkCommandBufferAllocateInfo cbai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr, .commandPool = cpool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (const VkResult r = vkAllocateCommandBuffers(dev.handle(), &cbai, &cmd); r != VK_SUCCESS) {
        vkDestroyCommandPool(dev.handle(), cpool, nullptr);
        vkDestroyBuffer(dev.handle(), staging, nullptr);
        vkFreeMemory(dev.handle(), staging_mem, nullptr);
        t.destroy();
        return std::unexpected("gpu_texture: vkAllocateCommandBuffers failed");
    }
    const VkCommandBufferBeginInfo cbbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(cmd, &cbbi);

    image_barrier(cmd, t.image_, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy bic{
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
            .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {t.width_, t.height_, 1},
    };
    vkCmdCopyBufferToImage(cmd, staging, t.image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

    if (levels > 1) {
        // Blit chain: each mip must sit TRANSFER_SRC to be read and
        // TRANSFER_DST to be written; every intermediate level gets both
        // transitions in turn (uniform loop shape over per-level cleverness —
        // the last TRANSFER_SRC transition is wasted work, not a bug).
        image_barrier(cmd, t.image_, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        for (std::uint32_t i = 1; i < levels; ++i) {
            image_barrier(cmd, t.image_, i, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkImageBlit blit{
                .srcSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = i - 1,
                    .baseArrayLayer = 0, .layerCount = 1,
                },
                .srcOffsets = {{0, 0, 0},
                               {static_cast<std::int32_t>(mip_dim(t.width_, i - 1)),
                                static_cast<std::int32_t>(mip_dim(t.height_, i - 1)), 1}},
                .dstSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = i,
                    .baseArrayLayer = 0, .layerCount = 1,
                },
                .dstOffsets = {{0, 0, 0},
                               {static_cast<std::int32_t>(mip_dim(t.width_, i)),
                                static_cast<std::int32_t>(mip_dim(t.height_, i)), 1}},
            };
            vkCmdBlitImage(cmd, t.image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t.image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR); // spec-required box filter down 2x
            image_barrier(cmd, t.image_, i, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        image_barrier(cmd, t.image_, 0, levels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else {
        image_barrier(cmd, t.image_, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    vkEndCommandBuffer(cmd);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0, .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 0, .pSignalSemaphores = nullptr,
    };
    const VkResult sr = vkQueueSubmit(dev.graphics_queue(), 1, &si, VK_NULL_HANDLE);
    if (sr == VK_SUCCESS) vkQueueWaitIdle(dev.graphics_queue());
    vkFreeCommandBuffers(dev.handle(), cpool, 1, &cmd);
    vkDestroyCommandPool(dev.handle(), cpool, nullptr);
    vkDestroyBuffer(dev.handle(), staging, nullptr);
    vkFreeMemory(dev.handle(), staging_mem, nullptr);
    if (sr != VK_SUCCESS) {
        t.destroy();
        return std::unexpected("gpu_texture: upload submit failed");
    }

    // ---- view (full mip chain) ----------------------------------------------
    const VkImageViewCreateInfo vci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .image = t.image_, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
            .levelCount = levels, .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    if (const VkResult r = vkCreateImageView(dev.handle(), &vci, nullptr, &t.view_);
        r != VK_SUCCESS) {
        t.destroy();
        return std::unexpected("gpu_texture: vkCreateImageView failed");
    }
    t.layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t;
}

std::expected<std::vector<std::uint8_t>, std::string> gpu_texture::capture_mip_rgba8(
    std::uint32_t level) {
    if (level >= mip_levels_) {
        return std::unexpected("capture_mip_rgba8: level " + std::to_string(level) +
                               " >= " + std::to_string(mip_levels_));
    }
    const std::uint32_t w = mip_dim(width_, level), h = mip_dim(height_, level);
    const VkDeviceSize bytes = std::size_t(w) * h * 4;

    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0, .pQueueFamilyIndices = nullptr,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateBuffer(dev_->handle(), &bci, nullptr, &buf); r != VK_SUCCESS) {
        return std::unexpected("capture_mip_rgba8: vkCreateBuffer failed");
    }
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(dev_->handle(), buf, &reqs);
    const auto mt = find_memory_type(*dev_, reqs.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mt) {
        vkDestroyBuffer(dev_->handle(), buf, nullptr);
        return std::unexpected("capture_mip_rgba8: no host-visible memory");
    }
    const VkMemoryAllocateInfo mai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr, .allocationSize = reqs.size, .memoryTypeIndex = *mt,
    };
    if (const VkResult r = vkAllocateMemory(dev_->handle(), &mai, nullptr, &mem); r != VK_SUCCESS) {
        vkDestroyBuffer(dev_->handle(), buf, nullptr);
        return std::unexpected("capture_mip_rgba8: vkAllocateMemory failed");
    }
    if (const VkResult r = vkBindBufferMemory(dev_->handle(), buf, mem, 0); r != VK_SUCCESS) {
        vkDestroyBuffer(dev_->handle(), buf, nullptr);
        vkFreeMemory(dev_->handle(), mem, nullptr);
        return std::unexpected("capture_mip_rgba8: vkBindBufferMemory failed");
    }
    void* mapped = nullptr;
    vkMapMemory(dev_->handle(), mem, 0, bytes, 0, &mapped);

    const VkCommandPoolCreateInfo cpci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = dev_->graphics_queue_family(),
    };
    VkCommandPool cpool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev_->handle(), &cpci, nullptr, &cpool);
    const VkCommandBufferAllocateInfo cbai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr, .commandPool = cpool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev_->handle(), &cbai, &cmd);
    const VkCommandBufferBeginInfo cbbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(cmd, &cbbi);
    // READ -> TRANSFER_SRC: previous use is sampling (INPUT_READ at FS), so
    // the source stage/access must say so (3.3 lesson).
    image_barrier(cmd, image_, level, 1, layout_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferImageCopy bic{
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level,
            .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {w, h, 1},
    };
    vkCmdCopyImageToBuffer(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    image_barrier(cmd, image_, level, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkEndCommandBuffer(cmd);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0, .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 0, .pSignalSemaphores = nullptr,
    };
    const VkResult sr = vkQueueSubmit(dev_->graphics_queue(), 1, &si, VK_NULL_HANDLE);
    if (sr == VK_SUCCESS) vkQueueWaitIdle(dev_->graphics_queue());
    std::vector<std::uint8_t> out;
    if (sr == VK_SUCCESS) {
        out.assign(static_cast<std::uint8_t*>(mapped),
                   static_cast<std::uint8_t*>(mapped) + bytes);
    }
    vkUnmapMemory(dev_->handle(), mem);
    vkFreeCommandBuffers(dev_->handle(), cpool, 1, &cmd);
    vkDestroyCommandPool(dev_->handle(), cpool, nullptr);
    vkDestroyBuffer(dev_->handle(), buf, nullptr);
    vkFreeMemory(dev_->handle(), mem, nullptr);
    if (sr != VK_SUCCESS) return std::unexpected("capture_mip_rgba8: submit failed");
    return out;
}

std::expected<VkSampler, std::string> create_static_sampler(const device& dev) {
    const VkSamplerCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        // NEAREST everywhere: pin draws magnify texels 1:N and must read
        // back byte-exact authored values (3.7 pin). Mip selection policy is
        // 3.10 material work; the chain exists already (upload generates it).
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        // REPEAT: the glTF sampling default (sampler.wrapS/T default wrap),
        // and the seam-welded tangents from 3.6 assume wrapping UV space.
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE, .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f, // magnified draws sit exactly at LOD 0
        .maxLod = VK_LOD_CLAMP_NONE, // chain ready for later mip-sampling passes
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    VkSampler s = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateSampler(dev.handle(), &ci, nullptr, &s); r != VK_SUCCESS) {
        return std::unexpected("create_static_sampler: vkCreateSampler failed (" +
                               std::to_string(static_cast<int>(r)) + ")");
    }
    return s;
}

void gpu_texture::destroy() {
    if (!dev_) return;
    if (view_ != VK_NULL_HANDLE) { vkDestroyImageView(dev_->handle(), view_, nullptr); view_ = VK_NULL_HANDLE; }
    if (mem_ != VK_NULL_HANDLE) { vkFreeMemory(dev_->handle(), mem_, nullptr); mem_ = VK_NULL_HANDLE; }
    if (image_ != VK_NULL_HANDLE) { vkDestroyImage(dev_->handle(), image_, nullptr); image_ = VK_NULL_HANDLE; }
    dev_ = nullptr;
}

gpu_texture::~gpu_texture() { destroy(); }

gpu_texture::gpu_texture(gpu_texture&& o) noexcept
    : dev_(o.dev_), image_(o.image_), mem_(o.mem_), view_(o.view_),
      width_(o.width_), height_(o.height_), mip_levels_(o.mip_levels_), layout_(o.layout_) {
    o.dev_ = nullptr;
    o.image_ = VK_NULL_HANDLE;
    o.mem_ = VK_NULL_HANDLE;
    o.view_ = VK_NULL_HANDLE;
}

gpu_texture& gpu_texture::operator=(gpu_texture&& o) noexcept {
    if (this != &o) {
        destroy();
        dev_ = o.dev_;
        image_ = o.image_;
        mem_ = o.mem_;
        view_ = o.view_;
        width_ = o.width_;
        height_ = o.height_;
        mip_levels_ = o.mip_levels_;
        layout_ = o.layout_;
        o.dev_ = nullptr;
        o.image_ = VK_NULL_HANDLE;
        o.mem_ = VK_NULL_HANDLE;
        o.view_ = VK_NULL_HANDLE;
    }
    return *this;
}

} // namespace ve::gfx
