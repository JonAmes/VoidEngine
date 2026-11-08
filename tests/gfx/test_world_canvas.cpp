// test: gfx_world_canvas — 3.14 world-space canvas primitive.
// A canvas = an offscreen render target the SAME immediate-mode UI path fills,
// plus a scene transform a later pass composites it on. Proven end to end:
//  1. a 32x32 UI draw list renders byte-identical into two different targets
//     (the canvas target is just a render target — render-target-agnostic
//     doctrine, rule #3);
//  2. the canvas target is exposed COLOR_ATTACHMENT -> SHADER_READ_ONLY and a
//     scene pass composites it as a textured quad through a deliberately
//     OFF-AXIS frustum (perspective_offaxis — the VR code path);
//  3. every sampled scene pixel is pinned to the CPU-computed canvas texel:
//     back-projection through the hand-chosen off-axis tangents (z=-2 plane),
//     NEAREST at texel centers; outside the quad, the scene clear shows.
// Golden canvas_scene_64.png strict byte-exact on lavapipe (decision #35).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "gfx_golden.h"

#include "../ve_test_portability.h" // getpid

#include <ve/cook/cook.h>
#include <ve/core/math/mat4.h>
#include <ve/core/math/projection.h>
#include <ve/gfx/descriptor.h>
#include <ve/gfx/font.h>
#include <ve/gfx/pipeline_cache.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>
#include <ve/gfx/texture.h>
#include <ve/gfx/ui.h>

#include "gfx_test_env.h"

#ifndef VE_SOURCE_DIR
#define VE_SOURCE_DIR "."
#endif

namespace {
using namespace ve::core::math;

std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_canvas_") + name + "_" + std::to_string(ve_test::pid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

constexpr int kCW = 32; // canvas target
constexpr int kSW = 64; // scene target

// Same UI shader the 3.13 test proves (pixels -> Vulkan NDC y-down).
const char* kUiVert = R"GLSL(#version 450
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

const char* kUiFrag = R"GLSL(#version 450
layout(binding = 0) uniform sampler2D t;  // slot 0: solid white, slot 1: atlas
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(color.rgb, color.a * texture(t, uv).a);
}
)GLSL";

// Scene compositing shader: canvas quad under a CPU-computed MVP (push).
const char* kCanVert = R"GLSL(#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uvin;
layout(binding = 1) uniform Mat { mat4 mvp; };  // UBO (binding 1; sampler stays 0)
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec2 uv;
void main() {
    uv = uvin;
    gl_Position = mvp * vec4(pos, 1.0);
}
)GLSL";

const char* kCanFrag = R"GLSL(#version 450
layout(binding = 0) uniform sampler2D canvas;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = texture(canvas, uv);  // opaque: canvas alpha is 1 everywhere
}
)GLSL";

// Host-visible buffer (test plumbing; see the 3.13 test).
struct host_buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    void create(const ve::gfx::device& dev, VkDeviceSize size, VkBufferUsageFlags usage) {
        const VkBufferCreateInfo bi{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr, .flags = 0, .size = size, .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr};
        REQUIRE(vkCreateBuffer(dev.handle(), &bi, nullptr, &buf) == VK_SUCCESS);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev.handle(), buf, &req);
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
        REQUIRE(mt != 0xFFFFFFFFu);
        const VkMemoryAllocateInfo ai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr, .allocationSize = req.size, .memoryTypeIndex = mt};
        REQUIRE(vkAllocateMemory(dev.handle(), &ai, nullptr, &mem) == VK_SUCCESS);
        REQUIRE(vkBindBufferMemory(dev.handle(), buf, mem, 0) == VK_SUCCESS);
        REQUIRE(vkMapMemory(dev.handle(), mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS);
    }
    void destroy(const ve::gfx::device& dev) {
        vkDestroyBuffer(dev.handle(), buf, nullptr);
        vkFreeMemory(dev.handle(), mem, nullptr);
    }
};

// Draw a ui_frame onto a presenter via the UI pipeline (identical code path
// for the canvas target and the plain target — the primitive adds NOTHING
// to the fill side; it is only a render target).
void draw_ui_frame(const ve::gfx::device& dev, ve::gfx::offscreen_presenter& pres,
                   VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set_solid,
                   VkDescriptorSet set_text, const ve::gfx::ui_frame& frame, float clear8) {
    host_buf vb, ib;
    vb.create(dev, frame.verts.size() * sizeof(ve::gfx::ui_vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ib.create(dev, frame.indices.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    std::memcpy(vb.map, frame.verts.data(), frame.verts.size() * sizeof(ve::gfx::ui_vertex));
    std::memcpy(ib.map, frame.indices.data(), frame.indices.size() * 4);

    VkCommandBuffer cmd = pres.begin_command();
    REQUIRE(cmd != VK_NULL_HANDLE);
    REQUIRE(pres.begin_pass(cmd));
    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = pres.color_view();
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {clear8 / 255.f, clear8 / 255.f, clear8 / 255.f, 1.0f};
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {static_cast<std::uint32_t>(kCW), static_cast<std::uint32_t>(kCW)}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vpt{0.f, 0.f, static_cast<float>(kCW), static_cast<float>(kCW), 0.f, 1.f};
    vkCmdSetViewport(cmd, 0, 1, &vpt);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const float push[2] = {2.f / kCW, 2.f / kCW}; // NDC y-DOWN
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    const VkDeviceSize voff = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buf, &voff);
    vkCmdBindIndexBuffer(cmd, ib.buf, 0, VK_INDEX_TYPE_UINT32);
    for (const auto& c : frame.cmds) {
        const VkDescriptorSet set = c.texture_slot == 0 ? set_solid : set_text;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        const VkRect2D sc{{c.clip_x, c.clip_y}, {static_cast<std::uint32_t>(c.clip_w),
                                                 static_cast<std::uint32_t>(c.clip_h)}};
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdDrawIndexed(cmd, c.idx_count, 1, static_cast<std::int32_t>(c.idx_offset), 0, 0);
    }
    vkCmdEndRendering(cmd);
    REQUIRE(pres.end_pass(cmd));
    REQUIRE(pres.submit(cmd));
    // submit() does not fence-wait; freeing the vertex/index buffers while
    // lavapipe's draw threads still read them SEGVs inside lvp. Drain first.
    vkDeviceWaitIdle(dev.handle());
    vb.destroy(dev);
    ib.destroy(dev);
}
} // namespace

TEST_CASE("gfx_world_canvas") {
    // ---- 1. cook font + all four shaders; load font via the runtime loader ---
    const auto font_src =
        std::filesystem::path(VE_SOURCE_DIR) / "assets/fonts/DejaVuSansMono.ttf";
    REQUIRE(std::filesystem::exists(font_src));
    const std::string src = tmp_root("src");
    const std::string out = tmp_root("out");
    std::string err;
    std::filesystem::copy_file(font_src, std::filesystem::path(src) / "DejaVuSansMono.ttf");
    write_file(std::filesystem::path(src) / "ui.vert", kUiVert);
    write_file(std::filesystem::path(src) / "ui.frag", kUiFrag);
    write_file(std::filesystem::path(src) / "canvas.vert", kCanVert);
    write_file(std::filesystem::path(src) / "canvas.frag", kCanFrag);
    auto cook = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(cook.ok, cook.error);

    ve::gfx::font0 font;
    REQUIRE_MESSAGE(ve::gfx::load_cooked_font_file(out + "/units/DejaVuSansMono.ttf.ck",
                                                   font, &err),
                    err);

    if (!gfx_test::force_lvp_icd()) { // Tier 2 needs lavapipe
        MESSAGE("no lavapipe ICD on this box — Tier-2 canvas test skipped");
        return;
    }

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    ve::gfx::CpuTexture atlas{font.atlas_w, font.atlas_h,
                              ve::core::cooked::kTexFmtRgba8, font.atlas};
    ve::gfx::CpuTexture white{1, 1, ve::core::cooked::kTexFmtRgba8, {255, 255, 255, 255}};
    auto tex_font = ve::gfx::gpu_texture::create(*dev, atlas);
    REQUIRE_MESSAGE(tex_font.has_value(), tex_font.error());
    auto tex_white = ve::gfx::gpu_texture::create(*dev, white);
    REQUIRE_MESSAGE(tex_white.has_value(), tex_white.error());
    auto sampler = ve::gfx::create_static_sampler(*dev);
    REQUIRE_MESSAGE(sampler.has_value(), sampler.error());

    // ---- 2. the UI draw list for the canvas (one frame, 32x32 screen) -------
    ve::gfx::ui_context ui;
    ve::gfx::ui_io io{.mouse_x = 0.f, .mouse_y = 0.f, .chars = {}};
    bool check = true;
    ui.begin_frame(io, font, kCW, kCW);
    REQUIRE(ui.begin_window("W", 1, 1, 30, 30));
    ui.checkbox("On", &check);
    ui.end_window();
    ui.end_frame();
    const auto& frame = ui.frame();
    REQUIRE(!frame.cmds.empty());

    // ---- 3. UI pipeline (shared by both canvas fills) ------------------------
    ve::cook::Unit u_v, u_f;
    REQUIRE(ve::cook::read_unit_file(out + "/units/ui.vert.ck", u_v, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/ui.frag.ck", u_f, &err));
    auto sm_v = ve::gfx::shader_module::create(*dev, u_v.payload);
    auto sm_f = ve::gfx::shader_module::create(*dev, u_f.payload);
    REQUIRE(sm_v.has_value());
    REQUIRE(sm_f.has_value());
    auto dsl = ve::gfx::sampler_set_layout::create(*dev, 0, VK_SHADER_STAGE_FRAGMENT_BIT);
    REQUIRE_MESSAGE(dsl.has_value(), dsl.error());
    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 64}; // mat4 (ui pushes 8)
    const VkDescriptorSetLayout sl = dsl->handle();
    const VkPipelineLayoutCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .setLayoutCount = 1, .pSetLayouts = &sl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &pci, nullptr, &layout) == VK_SUCCESS);

    auto pool = ve::gfx::descriptor_pool::create(*dev, 4, 4);
    REQUIRE_MESSAGE(pool.has_value(), pool.error());
    auto set_solid = pool->allocate(dsl->handle());
    auto set_text = pool->allocate(dsl->handle());
    REQUIRE_MESSAGE(set_solid.has_value(), set_solid.error());
    REQUIRE_MESSAGE(set_text.has_value(), set_text.error());
    pool->write_sampler(*set_solid, 0, *sampler, tex_white->view());
    pool->write_sampler(*set_text, 0, *sampler, tex_font->view());

    ve::gfx::pipeline_desc desc{};
    desc.layout = layout;
    desc.stages = {
        {VK_SHADER_STAGE_VERTEX_BIT, sm_v->handle(), "main"},
        {VK_SHADER_STAGE_FRAGMENT_BIT, sm_f->handle(), "main"},
    };
    desc.bindings = {{0, static_cast<std::uint32_t>(sizeof(ve::gfx::ui_vertex)),
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
    desc.color_formats = {VK_FORMAT_B8G8R8A8_UNORM};
    ve::gfx::blend_attachment_state blend{};
    blend.blend_enable = true;
    blend.src_color = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.src_alpha = VK_BLEND_FACTOR_ZERO; // preserve target alpha
    blend.dst_alpha = VK_BLEND_FACTOR_ONE;
    desc.blends = {blend};
    desc.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);
    ve::gfx::pipeline_cache cache(*dev);
    auto pipe_ui = cache.get(desc);
    REQUIRE_MESSAGE(pipe_ui.has_value(), pipe_ui.error());

    // ---- 4. canvas target vs plain target: byte-identical fills -------------
    auto canvas = ve::gfx::offscreen_presenter::create(*dev, kCW, kCW);
    REQUIRE_MESSAGE(canvas.has_value(), canvas.error());
    auto plain = ve::gfx::offscreen_presenter::create(*dev, kCW, kCW);
    REQUIRE_MESSAGE(plain.has_value(), plain.error());
    draw_ui_frame(*dev, *canvas, *pipe_ui, layout, *set_solid, *set_text, frame, 5.f);
    draw_ui_frame(*dev, *plain, *pipe_ui, layout, *set_solid, *set_text, frame, 5.f);
    const auto capA = canvas->capture_rgba8();
    const auto capB = plain->capture_rgba8();
    REQUIRE(capA.size() == capB.size());
    CHECK_MESSAGE(capA == capB, "canvas fill != 2D-rendered equivalent");
    // the canvas holds real chrome (not a flat fill): >=2 distinct texels
    {
        std::vector<std::uint32_t> distinct;
        for (std::size_t i = 0; i + 3 < capA.size(); i += 4) {
            const std::uint32_t t = capA[i] | (capA[i + 1] << 8) | (capA[i + 2] << 16) |
                                    (capA[i + 3] << 24);
            if (distinct.size() < 3 &&
                std::find(distinct.begin(), distinct.end(), t) == distinct.end())
                distinct.push_back(t);
        }
        CHECK(distinct.size() >= 2);
    }

    // ---- 5. expose the canvas for sampling, then composite in a scene pass --
    {
        VkCommandBuffer cmd = canvas->begin_command();
        REQUIRE(cmd != VK_NULL_HANDLE);
        REQUIRE(canvas->expose_color_for_sampling(cmd));
        REQUIRE(canvas->submit(cmd));
    }

    // off-axis frustum (the VR path): asymmetric tangents, deliberately not
    // symmetric — the canvas quad sits at z=-2 and projects off-centre.
    constexpr float kN = 1.f, kF = 10.f, kL = -1.3f, kR = 0.9f, kB = -0.8f, kT = 1.1f;
    const mat4f proj = perspective_offaxis(kN, kF, kL, kR, kB, kT, depth_range::standard_0_to_1);
    const mat4f view{}; // camera at origin looking -Z: view is identity
    const mat4f mvp = proj * view;

    // quad corners: world x=-1+2u, y=+1-2v, z=-2; uv (u,v) top-down canvas
    struct Vert { float x, y, z, u, v; };
    const std::vector<Vert> quad = {
        {-1.f, 1.f, -2.f, 0.f, 0.f},   // TL
        {1.f, 1.f, -2.f, 1.f, 0.f},    // TR
        {1.f, -1.f, -2.f, 1.f, 1.f},   // BR
        {-1.f, -1.f, -2.f, 0.f, 1.f},  // BL
    };
    host_buf qb, qi;
    qb.create(*dev, quad.size() * sizeof(Vert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    qi.create(*dev, 6 * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    std::memcpy(qb.map, quad.data(), quad.size() * sizeof(Vert));
    {
        std::uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        std::memcpy(qi.map, idx, sizeof idx);
    }

    ve::cook::Unit c_v, c_f;
    REQUIRE(ve::cook::read_unit_file(out + "/units/canvas.vert.ck", c_v, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/canvas.frag.ck", c_f, &err));
    auto cm_v = ve::gfx::shader_module::create(*dev, c_v.payload);
    auto cm_f = ve::gfx::shader_module::create(*dev, c_f.payload);
    REQUIRE(cm_v.has_value());
    REQUIRE(cm_f.has_value());
    const VkDescriptorSetLayoutBinding bnds[2]{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo dli2{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .bindingCount = 2, .pBindings = bnds};
    VkDescriptorSetLayout sl2_raw = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorSetLayout(dev->handle(), &dli2, nullptr, &sl2_raw) == VK_SUCCESS);
    const VkPipelineLayoutCreateInfo pci2{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .setLayoutCount = 1, .pSetLayouts = &sl2_raw,
        .pushConstantRangeCount = 0, .pPushConstantRanges = nullptr};
    VkPipelineLayout sl2 = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &pci2, nullptr, &sl2) == VK_SUCCESS);
    auto set_canvas = pool->allocate(sl2_raw);
    REQUIRE_MESSAGE(set_canvas.has_value(), set_canvas.error());
    pool->write_sampler(*set_canvas, 0, *sampler, canvas->color_view()); // SHADER_READ_ONLY
    host_buf mvp_ub;
    mvp_ub.create(*dev, 64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float m16[16];
    for (int i = 0; i < 16; ++i) m16[i] = mvp.m[i];
    std::memcpy(mvp_ub.map, m16, 64);
    pool->write_uniform(*set_canvas, 1, mvp_ub.buf, 64);
    ve::gfx::pipeline_desc sd = desc;
    sd.layout = sl2;
    sd.stages = {
        {VK_SHADER_STAGE_VERTEX_BIT, cm_v->handle(), "main"},
        {VK_SHADER_STAGE_FRAGMENT_BIT, cm_f->handle(), "main"},
    };
    sd.bindings = {{0, sizeof(Vert), VK_VERTEX_INPUT_RATE_VERTEX}};
    sd.attributes = {
        // pos MUST be R32G32G32: the shader reads vec3 pos and mvp's w-row is
        // m[11]=-z, so a missing z (Vulkan default-fills (0,0,1)) silently
        // makes clip.w=-1 and discards the whole quad. Silent killer.
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},  // pos
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},    // uv
    };
    // NOT clear(): the cache sizes color attachment writes from blends.size()
    // — an empty list makes a write-nothing pipeline (lavapipe drops the draw).
    sd.blends = {ve::gfx::blend_attachment_state{}}; // blend_enable=false -> opaque
    auto pipe_scene = cache.get(sd);
    REQUIRE_MESSAGE(pipe_scene.has_value(), pipe_scene.error());

    auto scene = ve::gfx::offscreen_presenter::create(*dev, kSW, kSW);
    REQUIRE_MESSAGE(scene.has_value(), scene.error());
    {
        VkCommandBuffer cmd = scene->begin_command();
        REQUIRE(cmd != VK_NULL_HANDLE);
        REQUIRE(scene->begin_pass(cmd));
        VkRenderingAttachmentInfo att{};
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = scene->color_view();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = {10.f / 255.f, 10.f / 255.f, 16.f / 255.f, 1.f};
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {static_cast<std::uint32_t>(kSW), static_cast<std::uint32_t>(kSW)}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vpt{0.f, 0.f, static_cast<float>(kSW), static_cast<float>(kSW), 0.f, 1.f};
        const VkRect2D sc{{0, 0}, {static_cast<std::uint32_t>(kSW), static_cast<std::uint32_t>(kSW)}};
        vkCmdSetViewport(cmd, 0, 1, &vpt);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe_scene);
        const VkDescriptorSet ds = *set_canvas;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sl2, 0, 1, &ds, 0, nullptr);
        const VkDeviceSize voff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &qb.buf, &voff);
        vkCmdBindIndexBuffer(cmd, qi.buf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
        vkCmdEndRendering(cmd);
        REQUIRE(scene->end_pass(cmd));
        REQUIRE(scene->submit(cmd));
    }

    // ---- 6. CPU reference: back-project pixels through the off-axis planes --
    // clip.x = A*xv + B*zv, clip.y = -(C*yv + D*zv), w = -zv; at zv = -2:
    //   xv = (2*ndc.x + 2*B) / A   (sign: clip.x = ndc.x*w = -2*ndc.x... derived
    //   below in code), yv from the y row. uv = (xv+1)/2, (1-yv)/2.
    const float A = 2.f / (kR - kL), B = (kR + kL) / (kR - kL);
    const float C = 2.f / (kT - kB), D = (kT + kB) / (kT - kB);
    const std::vector<std::uint8_t> simg = scene->capture_rgba8();
    const auto got = [&](int px, int py) -> std::uint32_t {
        const std::size_t o = (static_cast<std::size_t>(py) * kSW + px) * 4u;
        return simg[o] | (simg[o + 1] << 8) | (simg[o + 2] << 16) | (simg[o + 3] << 24);
    };
    const auto texel = [&](int tx, int ty) -> std::uint32_t {
        const std::size_t o = (static_cast<std::size_t>(ty) * kCW + tx) * 4u;
        return capA[o] | (capA[o + 1] << 8) | (capA[o + 2] << 16) | (capA[o + 3] << 24);
    };
    // forward: canvas texel center (tx,ty) -> scene pixel (hand-computed path)
    const auto project_uv = [&](float u, float v) -> std::pair<int, int> {
        const vec4f clip = mvp * vec4f::point(vec3f{-1.f + 2.f * u, 1.f - 2.f * v, -2.f});
        const float nx = clip.x / clip.w, ny = clip.y / clip.w;
        return {static_cast<int>(std::lround((nx + 1.f) * 0.5f * kSW - 0.5f)),
                static_cast<int>(std::lround((ny + 1.f) * 0.5f * kSW - 0.5f))};
    };
    // back: scene pixel center -> uv on the z=-2 plane (nullopt outside the quad)
    const auto back_uv = [&](int px, int py) -> std::optional<std::pair<float, float>> {
        const float nx = (px + 0.5f) / kSW * 2.f - 1.f;
        const float ny = (py + 0.5f) / kSW * 2.f - 1.f;
        // clip.x = A*xv + B*(-2) = nx*w = nx*2  -> xv = (2*nx + 2*B)/A
        // clip.y = -(C*yv + D*(-2)) = ny*2      -> yv = (2*D - 2*ny)/C
        const float xv = (2.f * nx + 2.f * B) / A;
        const float yv = (2.f * D - 2.f * ny) / C;
        if (xv < -1.f || xv > 1.f || yv < -1.f || yv > 1.f) return std::nullopt;
        return std::pair{(xv + 1.f) * 0.5f, (1.f - yv) * 0.5f};
    };

    // quad corners project inside the 64x64 frame, away from the corners:
    for (const auto& q : quad) {
        const auto [sx, sy] = project_uv(q.u, q.v);
        CHECK(sx >= 4);
        CHECK(sx <= 60);
        CHECK(sy >= 4);
        CHECK(sy <= 60);
    }

    // Pin A: texel centers composite byte-exact through the off-axis view.
    // NEAREST + texel-center samples: floor(uv*32) is the texel; ±½-pixel
    // raster offsets are far under the half-texel margin (quad ≈28 px wide
    // per 32 texels).
    for (auto tc : {std::pair{2, 3}, {15, 15}, {16, 16}, {24, 20}, {3, 28}, {29, 30}}) {
        const auto [sx, sy] = project_uv((tc.first + 0.5f) / kCW, (tc.second + 0.5f) / kCW);
        REQUIRE(sx >= 0);
        REQUIRE(sx < kSW);
        REQUIRE(sy >= 0);
        REQUIRE(sy < kSW);
        CHECK_MESSAGE(got(sx, sy) == texel(tc.first, tc.second),
                      "scene px " << sx << "," << sy << " (canvas texel " << tc.first << ","
                                  << tc.second << ") got " << std::hex << got(sx, sy)
                                  << " want " << texel(tc.first, tc.second));
    }

    // Pin B: interior pixels back-project to the texel they must sample.
    for (auto p : {std::pair{30, 30}, {40, 25}, {25, 50}, {50, 45}, {38, 38}}) {
        const auto uv = back_uv(p.first, p.second);
        REQUIRE(uv.has_value());
        const int tx = std::min(31, static_cast<int>(uv->first * kCW));
        const int ty = std::min(31, static_cast<int>(uv->second * kCW));
        CHECK_MESSAGE(got(p.first, p.second) == texel(tx, ty),
                      "interior px " << p.first << "," << p.second << " want texel("
                                     << tx << "," << ty << ") = " << std::hex << texel(tx, ty)
                                     << " got " << got(p.first, p.second));
    }

    // Pin C: outside the quad, the scene clear shows (canvas clear (5,5,5)
    // and scene clear (10,10,16) are distinct so this is unambiguous).
    const std::uint32_t kSceneClear = 16u | (10u << 8) | (10u << 16) | (255u << 24);
    for (auto p : {std::pair{0, 0}, {63, 63}, {2, 32}, {63, 4}}) {
        CHECK_FALSE(back_uv(p.first, p.second).has_value());
        CHECK_MESSAGE(got(p.first, p.second) == kSceneClear,
                      "outside px " << p.first << "," << p.second << " got " << std::hex
                                    << got(p.first, p.second) << " want clear " << kSceneClear);
    }
    // edge discipline: 4 px diagonally inside the TL corner = canvas, 4 px
    // outward = clear.
    {
        const auto [cx, cy] = project_uv(0.001f, 0.001f); // ~ TL corner
        CHECK(got(cx + 4, cy + 4) != kSceneClear);
        CHECK(got(cx - 4, cy - 4) == kSceneClear);
    }

    // Pin D: alpha survives the composite everywhere sampled (canvas kept
    // alpha 1 through the UI blend; the scene quad writes it out).
    for (auto p : {std::pair{30, 30}, {38, 38}, {0, 0}}) {
        CHECK_MESSAGE((got(p.first, p.second) >> 24) == 255u,
                      "alpha at " << p.first << "," << p.second);
    }

    // ---- 7. strict golden ----------------------------------------------------
    std::vector<std::uint8_t> rgba(simg.size());
    for (std::size_t i = 0; i < simg.size(); i += 4) {
        rgba[i + 0] = simg[i + 2];
        rgba[i + 1] = simg[i + 1];
        rgba[i + 2] = simg[i + 0];
        rgba[i + 3] = simg[i + 3];
    }
    const auto gv = gfx_golden::check_golden("canvas_scene_64.png", rgba.data(), kSW, kSW, {});
    CHECK_MESSAGE(gv.ok, gv.message);

    qb.destroy(*dev);
    qi.destroy(*dev);
    mvp_ub.destroy(*dev);
    vkDestroyDescriptorSetLayout(dev->handle(), sl2_raw, nullptr);
    vkDestroyPipelineLayout(dev->handle(), sl2, nullptr);
    vkDestroySampler(dev->handle(), *sampler, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);
}
