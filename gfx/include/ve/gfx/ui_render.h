#pragma once
// gfx::ui_renderer — the immediate-UI render pass (EK.1, decision #54; home
// declared by #15). Draws a gfx::ui_frame (from ui_context::frame()) onto any
// color render target the caller already opened. Promoted verbatim from the
// Tier-2 harness (tests/gfx/test_ui_immediate.cpp) — the pixels the golden
// ui_64.png attests are these commands.
//
// Doctrine:
//   * No runtime shader compile (#15): create() takes cooked SHDR payloads;
//     the GLSL sources below go through the shader cooklet like any other.
//   * No backbuffer assumption: draw() records into a caller-owned active
//     render pass; a Presenter presents the target (rule 3).
//   * The vertex buffer is a host-visible one-shot: draw() memcpy's the frame
//     and records immediately, so the caller must not overlap submits that
//     read the buffer while a later draw() writes it (presenters here
//     submit-and-wait; revisit if a queued UI frame ever overlaps).
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <vector>

#include "ve/gfx/device.h" // Vk types via volk (VK_NO_PROTOTYPES ordering)
#include "ve/gfx/ui.h"

namespace ve::gfx {

class gpu_texture;
class shader_module;
class pipeline_cache;
class sampler_set_layout;
class descriptor_pool;

// The UI shaders. Cook these through the shader cooklet before create().
// Pixels are top-left origin; the shader maps them to Vulkan NDC (y DOWN).
// The blending alpha is the bound texture's (atlas coverage; the solid slot
// is alpha 1), and the target's alpha is never written by UI coverage.
extern const char* const kUi_vert_glsl;
extern const char* const kUi_frag_glsl;

class ui_renderer {
public:
    // solid_white / font_atlas bind as texture slots 0 / 1 — ui_draw_cmd's
    // texture_slot selects between them per command. color_format must match
    // the render target(s) this pass records into (the pipeline is keyed to
    // it). vert_shdr/frag_shdr are full cooked SHDR unit payloads.
    static std::expected<ui_renderer, std::string> create(
        const device& dev,
        const gpu_texture& solid_white,
        const gpu_texture& font_atlas,
        const std::vector<std::uint8_t>& vert_shdr,
        const std::vector<std::uint8_t>& frag_shdr,
        VkFormat color_format);

    ~ui_renderer();
    ui_renderer(ui_renderer&&) noexcept;
    ui_renderer& operator=(ui_renderer&&) noexcept;
    ui_renderer(const ui_renderer&) = delete;
    ui_renderer& operator=(const ui_renderer&) = delete;

    // Record the frame onto an ACTIVE color render pass (caller owns
    // vkCmdBeginRendering/EndRendering). Sets viewport, pipeline, push
    // constants, VB/IB, then one scissored indexed draw per ui_draw_cmd
    // (scissor = the cmd's clip rect). Host VB/IB grow on demand.
    bool draw(VkCommandBuffer cmd, const ui_frame& frame,
              std::int32_t width_px, std::int32_t height_px);


private:
    ui_renderer() = default;

    struct host_side {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void* map = nullptr;
        std::uint64_t capacity = 0;
    };
    bool ensure_host(const device& dev, host_side& h, std::uint64_t bytes,
                     VkBufferUsageFlags usage);
    void destroy_host(const device& dev, host_side& h);

    const device* dev_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE; // owned by cache_ (borrowed handle)
    std::unique_ptr<shader_module> sm_v_, sm_f_; // pre-refactor harness kept
    // the modules alive for the whole frame; match that (never destroy while
    // draws may still reference the pipeline built from them).
    std::unique_ptr<sampler_set_layout> dsl_;  // both sets share this layout
    std::unique_ptr<descriptor_pool> pool_;
    VkDescriptorSet set_solid_ = VK_NULL_HANDLE, set_text_ = VK_NULL_HANDLE;
    std::unique_ptr<pipeline_cache> cache_; // pipeline_cache is not movable
    host_side vb_, ib_;
};

} // namespace ve::gfx
