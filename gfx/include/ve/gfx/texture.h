#pragma once

#include <ve/gfx/device.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

// Cooked-texture readback (decision #13): the runtime loads cooked TEX0
// units only — PNG never reaches runtime code, and no decoder is linked
// here. This is pure bounds-checked byte reading.
namespace ve::gfx {

struct CpuTexture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0; // ve::core::cooked::kTexFmtRgba8
    std::vector<std::uint8_t> pixels; // top-down rows, width*height*4
};

// Reads a VECK unit (type TEX0) from disk and copies its payload.
// Returns false (no exceptions; gfx may not throw) on bad magic/type/size.
bool load_cooked_texture_file(const std::string& unit_path, CpuTexture& out,
                              std::string* err);

// ---------------------------------------------------------------------------
// GPU textures (3.7). The CPU half above is the ONLY place texel bytes are
// ever seen: gpu_texture::upload is one-shot (staging -> optimal image), and
// from then on sampling is entirely GPU-side. Consumers never put barriers
// on the texture image either — the owner does every layout transition, the
// same policy the presenter applies to its targets.
//
// Format: R8G8B8A8_UNORM for now, so readback pins are byte-exact. sRGB
// *tagging* of albedo textures is material work (3.10) — the cooklet stores
// raw bytes either way, so the view format is purely a runtime choice.
class gpu_texture {
public:
    // "Mip chain generated at upload for now" (3.7 checklist): base mip via
    // a host-visible staging buffer, each upper mip via a LINEAR-filtered
    // vkCmdBlitImage chain down to 1x1. One-shot command buffer, submit +
    // wait — load-time cost; a frame-budgeted uploader is later work.
    static std::expected<gpu_texture, std::string> create(const device& dev,
                                                          const CpuTexture& src);
    ~gpu_texture();
    gpu_texture(gpu_texture&&) noexcept;
    gpu_texture& operator=(gpu_texture&&) noexcept;
    gpu_texture(const gpu_texture&) = delete;
    gpu_texture& operator=(const gpu_texture&) = delete;

    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint32_t mip_levels() const { return mip_levels_; }
    VkImageLayout layout() const { return layout_; } // always SHADER_READ_ONLY after create

    // NOT a consumer path — test/golden readback of one mip, mirroring the
    // presenter's capture_depth_f32 policy. Owner-managed transitions:
    // SHADER_READ_ONLY -> TRANSFER_SRC -> copy -> back.
    std::expected<std::vector<std::uint8_t>, std::string> capture_mip_rgba8(std::uint32_t level);

private:
    gpu_texture() = default;
    void destroy();

    const device* dev_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory mem_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    std::uint32_t width_ = 0, height_ = 0, mip_levels_ = 0;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

// The static sampler (3.7 checklist): sampler state is authored once at
// descriptor/pipeline level — it is never dynamic state. NEAREST throughout:
// the debug/pin renderer magnifies texels 1:N and must read back the exact
// authored texel; mip-selection policy becomes a material concern in 3.10+.
// LOD clamp at 0 (minLod) keeps magnified draws exactly at LOD 0; maxLod
// stays NONE so the upload-generated chain is ready for later passes.
// Caller owns the handle; destroy before the device.
std::expected<VkSampler, std::string> create_static_sampler(const device& dev);

} // namespace ve::gfx
