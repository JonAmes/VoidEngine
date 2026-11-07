#include <ve/gfx/shader.h>

#include <utility>

namespace ve::gfx {

std::expected<shader_module, std::string> shader_module::create(
    const device& dev, const std::vector<std::uint8_t>& payload) {
    // SHDR payload layout (core/cooked.h): u32 stage | SPIR-V binary.
    if (payload.size() < 8) {
        return std::unexpected("SHDR payload too small");
    }
    const std::uint32_t stage = static_cast<std::uint32_t>(payload[0]) |
                                (static_cast<std::uint32_t>(payload[1]) << 8) |
                                (static_cast<std::uint32_t>(payload[2]) << 16) |
                                (static_cast<std::uint32_t>(payload[3]) << 24);
    if (stage > 2u) {
        return std::unexpected("SHDR stage out of range: " + std::to_string(stage));
    }
    const std::size_t body = payload.size() - 4;
    if (body < 20 || (body % 4u) != 0u) {
        return std::unexpected("SHDR SPIR-V body too small or not word-aligned");
    }
    // SPIR-V magic 0x07230203 little-endian = 03 02 23 07. Rejected here so
    // a corrupt unit never reaches vkCreateShaderModule (driver behavior on
    // garbage is vendor-specific; ours is a clean load error).
    if (payload[4] != 0x03u || payload[5] != 0x02u || payload[6] != 0x23u || payload[7] != 0x07u) {
        return std::unexpected("SHDR SPIR-V bad magic");
    }

    shader_module out;
    out.device_ = dev.handle();
    out.stage_ = stage;
    out.spirv_.resize(body / 4);
    for (std::size_t i = 0; i < out.spirv_.size(); ++i) {
        const std::uint8_t* w = payload.data() + 4 + i * 4;
        out.spirv_[i] = static_cast<std::uint32_t>(w[0]) |
                        (static_cast<std::uint32_t>(w[1]) << 8) |
                        (static_cast<std::uint32_t>(w[2]) << 16) |
                        (static_cast<std::uint32_t>(w[3]) << 24);
    }

    const VkShaderModuleCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = out.spirv_.size() * 4,
        .pCode = out.spirv_.data(),
    };
    if (const VkResult r = vkCreateShaderModule(out.device_, &ci, nullptr, &out.handle_); r != VK_SUCCESS) {
        return std::unexpected("vkCreateShaderModule failed: VkResult(" + std::to_string(static_cast<int>(r)) + ")");
    }
    return out;
}

shader_module::~shader_module() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, handle_, nullptr);
    }
}

shader_module::shader_module(shader_module&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      handle_(std::exchange(other.handle_, VK_NULL_HANDLE)),
      spirv_(std::move(other.spirv_)),
      stage_(other.stage_) {}

} // namespace ve::gfx
