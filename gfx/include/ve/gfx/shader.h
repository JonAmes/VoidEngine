#pragma once
#include <ve/gfx/device.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace ve::gfx {

// Runtime shader object (decision #13): accepts ONLY cooked SHDR payload
// bytes (u32 stage | SPIR-V, per core/cooked.h). No GLSL, no compiler,
// no spirv tools in the engine. Corruption is rejected before Vulkan ever
// sees it — a bad magic is a load error, not a driver crash.
class shader_module {
public:
    // payload = full SHDR unit payload (stage word + SPIR-V body).
    static std::expected<shader_module, std::string> create(
        const device& dev, const std::vector<std::uint8_t>& payload);

    ~shader_module();
    shader_module(const shader_module&) = delete;
    shader_module& operator=(const shader_module&) = delete;
    shader_module(shader_module&& other) noexcept;

    VkShaderModule handle() const { return handle_; }
    std::uint32_t stage() const { return stage_; } // kShdrStage* (core/cooked.h)
    std::size_t spirv_size() const { return spirv_.size() * 4; }

private:
    shader_module() = default;
    VkDevice device_ = VK_NULL_HANDLE;
    VkShaderModule handle_ = VK_NULL_HANDLE;
    std::vector<std::uint32_t> spirv_; // kept alive: pCode must stay valid
    std::uint32_t stage_ = 0;
};

} // namespace ve::gfx
