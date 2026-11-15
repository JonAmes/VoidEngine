// gfx::ui_renderer implementation (EK.1). The setup and recording sequence is
// the GPU half of tests/gfx/test_ui_immediate.cpp moved into the engine
// verbatim; the test now consumes this class and its golden (ui_64.png)
// attests the moved code byte-for-byte.
#include <cstdint>
#include <cstring>
#include <utility>

#include "ve/gfx/ui_render.h"

#include "ve/gfx/descriptor.h"
#include "ve/gfx/device.h"
#include "ve/gfx/pipeline_cache.h"
#include "ve/gfx/shader.h"
#include "ve/gfx/texture.h"

namespace ve::gfx {

// Moved verbatim from the harness (test_ui_immediate.cpp:61-85).
const char* const kUi_vert_glsl = R"GLSL(#version 450
layout(location = 0) in vec2 pos;   // pixels, top-left origin
layout(location = 1) in vec2 uvin;  // texcenter uv
layout(location = 2) in vec2 colrg;
layout(location = 3) in vec2 colba;
layout(push_constant) uniform Push { float sx; float sy; };  // 2/W, 2/H
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;
void main() {
    uv = uvin;
    color = vec4(colrg, colba);
    gl_Position = vec4(pos.x * sx - 1.0, pos.y * sy - 1.0, 0.5, 1.0);
}
)GLSL";

const char* const kUi_frag_glsl = R"GLSL(#version 450
layout(binding = 0) uniform sampler2D t;  // slot 0: solid white, slot 1: atlas
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(color.rgb, color.a * texture(t, uv).a);
}
)GLSL";

std::expected<ui_renderer, std::string> ui_renderer::create(
    const device& dev,
    const gpu_texture& solid_white,
    const gpu_texture& font_atlas,
    const std::vector<std::uint8_t>& vert_shdr,
    const std::vector<std::uint8_t>& frag_shdr,
    VkFormat color_format) {
    auto sm_v = shader_module::create(dev, vert_shdr);
    if (!sm_v) return std::unexpected(sm_v.error());
    auto sm_f = shader_module::create(dev, frag_shdr);
    if (!sm_f) return std::unexpected(sm_f.error());

    auto sampler = create_static_sampler(dev);
    if (!sampler) return std::unexpected(sampler.error());
    auto dsl = sampler_set_layout::create(dev, 0, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!dsl) return std::unexpected(dsl.error());

    const VkDescriptorSetLayout set_layouts[2] = {dsl->handle(), dsl->handle()};
    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 8};
    const VkPipelineLayoutCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .setLayoutCount = 2, .pSetLayouts = set_layouts,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev.handle(), &pci, nullptr, &layout) != VK_SUCCESS)
        return std::unexpected("ui_renderer: pipeline layout create failed");

    auto pool = descriptor_pool::create(dev, 4);
    if (!pool) return std::unexpected(pool.error());
    auto set_solid = pool->allocate(dsl->handle());
    auto set_text = pool->allocate(dsl->handle());
    if (!set_solid) return std::unexpected(set_solid.error());
    if (!set_text) return std::unexpected(set_text.error());
    pool->write_sampler(*set_solid, 0, *sampler, solid_white.view());
    pool->write_sampler(*set_text, 0, *sampler, font_atlas.view());

    pipeline_desc desc{};
    desc.layout = layout;
    desc.stages = {
        {VK_SHADER_STAGE_VERTEX_BIT, sm_v->handle(), "main"},
        {VK_SHADER_STAGE_FRAGMENT_BIT, sm_f->handle(), "main"},
    };
    desc.bindings = {{0, static_cast<std::uint32_t>(sizeof(ui_vertex)),
                      VK_VERTEX_INPUT_RATE_VERTEX}};
    desc.attributes = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},   // pos px
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},   // uv
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 16},  // color.rg
        {3, 0, VK_FORMAT_R32G32_SFLOAT, 24},  // color.ba
    };
    desc.cull = VK_CULL_MODE_NONE;
    desc.depth_test = false;
    desc.depth_write = false;
    desc.color_formats = {color_format};
    blend_attachment_state blend{};
    blend.blend_enable = true;
    blend.src_color = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    // RGB: src-alpha over dst. A: keep the target's alpha (never write UI
    // coverage into it) — src_alpha=0, dst_alpha=1.
    blend.src_alpha = VK_BLEND_FACTOR_ZERO;
    blend.dst_alpha = VK_BLEND_FACTOR_ONE;
    desc.blends = {blend};
    desc.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);

    ui_renderer r;
    r.dev_ = &dev;
    r.sm_v_ = std::make_unique<shader_module>(std::move(*sm_v));
    r.sm_f_ = std::make_unique<shader_module>(std::move(*sm_f));
    r.sampler_ = *sampler;
    r.layout_ = layout;
    r.dsl_ = std::make_unique<sampler_set_layout>(std::move(*dsl));
    r.pool_ = std::make_unique<descriptor_pool>(std::move(*pool));
    r.set_solid_ = *set_solid;
    r.set_text_ = *set_text;
    r.cache_ = std::make_unique<pipeline_cache>(dev);
    auto pipe = r.cache_->get(desc);
    if (!pipe) {
        vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
        vkDestroySampler(dev.handle(), r.sampler_, nullptr);
        return std::unexpected(pipe.error());
    }
    r.pipeline_ = *pipe;
    return r;
}

bool ui_renderer::ensure_host(const device& dev, host_side& h, std::uint64_t bytes,
                              VkBufferUsageFlags usage) {
    if (h.capacity >= bytes) return true;
    destroy_host(dev, h);
    const std::uint64_t cap = bytes < 65536 ? 65536 : bytes; // amortize small churn
    const VkBufferCreateInfo bi{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .size = cap,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr};
    if (vkCreateBuffer(dev.handle(), &bi, nullptr, &h.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev.handle(), h.buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(dev.physical(), &mp);
    std::uint32_t mt = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mt = i;
            break;
        }
    }
    if (mt == 0xFFFFFFFFu) {
        vkDestroyBuffer(dev.handle(), h.buf, nullptr);
        h.buf = VK_NULL_HANDLE;
        return false;
    }
    const VkMemoryAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr, .allocationSize = req.size, .memoryTypeIndex = mt};
    if (vkAllocateMemory(dev.handle(), &ai, nullptr, &h.mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev.handle(), h.buf, nullptr);
        h.buf = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(dev.handle(), h.buf, h.mem, 0) != VK_SUCCESS ||
        vkMapMemory(dev.handle(), h.mem, 0, VK_WHOLE_SIZE, 0, &h.map) != VK_SUCCESS) {
        destroy_host(dev, h);
        return false;
    }
    h.capacity = cap;
    return true;
}

void ui_renderer::destroy_host(const device& dev, host_side& h) {
    vkDestroyBuffer(dev.handle(), h.buf, nullptr);
    vkFreeMemory(dev.handle(), h.mem, nullptr);
    h = host_side{};
}

bool ui_renderer::draw(VkCommandBuffer cmd, const ui_frame& frame,
                       std::int32_t width_px, std::int32_t height_px) {
    if (frame.verts.empty() || frame.indices.empty() || width_px <= 0 || height_px <= 0)
        return false;
    if (!ensure_host(*dev_, vb_, frame.verts.size() * sizeof(ui_vertex),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return false;
    if (!ensure_host(*dev_, ib_, frame.indices.size() * sizeof(std::uint32_t),
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return false;
    std::memcpy(vb_.map, frame.verts.data(), frame.verts.size() * sizeof(ui_vertex));
    std::memcpy(ib_.map, frame.indices.data(), frame.indices.size() * 4);

    const VkViewport vpt{0.0f, 0.0f, static_cast<float>(width_px),
                         static_cast<float>(height_px), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vpt);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const float push[2] = {2.0f / static_cast<float>(width_px),
                           2.0f / static_cast<float>(height_px)}; // NDC y-DOWN: row r -> 2r/H-1
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    const VkDeviceSize voff = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb_.buf, &voff);
    vkCmdBindIndexBuffer(cmd, ib_.buf, 0, VK_INDEX_TYPE_UINT32);
    for (const auto& c : frame.cmds) {
        const VkDescriptorSet set = c.texture_slot == 0 ? set_solid_ : set_text_;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                                &set, 0, nullptr);
        const VkRect2D sc{{c.clip_x, c.clip_y},
                          {static_cast<std::uint32_t>(c.clip_w),
                           static_cast<std::uint32_t>(c.clip_h)}};
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdDrawIndexed(cmd, c.idx_count, 1, static_cast<std::int32_t>(c.idx_offset), 0, 0);
    }
    return true;
}

// Hand-rolled: the defaulted move would COPY the raw sampler_/layout_/
// pipeline_/vb_/ib_ handles, and the moved-from source's destructor (which
// create()'s `return r;` into std::expected always leaves behind — NRVO
// cannot elide across the wrapper type) would then destroy the sampler and
// pipeline layout the live object still uses. Bind-of-dead-layout = SEGV
// inside lavapipe at submit. Source must be nulled member-wise.
ui_renderer::ui_renderer(ui_renderer&& o) noexcept
    : dev_(std::exchange(o.dev_, nullptr)),
      sampler_(std::exchange(o.sampler_, VK_NULL_HANDLE)),
      layout_(std::exchange(o.layout_, VK_NULL_HANDLE)),
      pipeline_(std::exchange(o.pipeline_, VK_NULL_HANDLE)),
      sm_v_(std::move(o.sm_v_)), sm_f_(std::move(o.sm_f_)),
      dsl_(std::move(o.dsl_)), pool_(std::move(o.pool_)),
      set_solid_(std::exchange(o.set_solid_, VK_NULL_HANDLE)),
      set_text_(std::exchange(o.set_text_, VK_NULL_HANDLE)),
      cache_(std::move(o.cache_)),
      vb_(std::exchange(o.vb_, {})), ib_(std::exchange(o.ib_, {})) {}

ui_renderer& ui_renderer::operator=(ui_renderer&& o) noexcept {
    if (this != &o) {
        ui_renderer tmp(std::move(o)); // tmp now owns o's state; we keep ours
        std::swap(dev_, tmp.dev_); std::swap(sampler_, tmp.sampler_);
        std::swap(layout_, tmp.layout_); std::swap(pipeline_, tmp.pipeline_);
        std::swap(sm_v_, tmp.sm_v_); std::swap(sm_f_, tmp.sm_f_);
        std::swap(dsl_, tmp.dsl_); std::swap(pool_, tmp.pool_);
        std::swap(set_solid_, tmp.set_solid_); std::swap(set_text_, tmp.set_text_);
        std::swap(cache_, tmp.cache_);
        std::swap(vb_, tmp.vb_); std::swap(ib_, tmp.ib_);
        // tmp destructs with OUR old state: correct teardown order.
    }
    return *this;
}

ui_renderer::~ui_renderer() {
    if (!dev_) return;
    // Teardown order: GPU objects the device still sees first (buffers, then
    // layout/sampler), then pool/layouts via members, then cached pipelines
    // via cache_ (must precede device death; ui_renderer is declared before
    // the device at every use site).
    destroy_host(*dev_, vb_);
    destroy_host(*dev_, ib_);
    vkDestroyPipelineLayout(dev_->handle(), layout_, nullptr);
    vkDestroySampler(dev_->handle(), sampler_, nullptr);
}

} // namespace ve::gfx
