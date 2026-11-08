// test: gfx_texture_sample — 3.7. A 2x2 RGBA texture authored as a real PNG,
// cooked by the cooklet to TEX0, loaded by the runtime loader, uploaded to a
// VkImage with an upload-generated mip chain, then sampled through a quad
// (fullscreen triangle) draw with the static sampler. Pins:
//   - mip0 readback byte-identical to the authored texels (upload integrity),
//   - mip1 == exact 4-texel box average (upload-generated chain is real),
//   - deep-interior readback of the 32x magnified draw matches the texel
//     byte-exact (NEAREST at LOD 0 — the static sampler's contract),
//   - out-of-range mip capture is an error, not a crash,
//   - zero validation problems throughout.
// "The runtime never looks at texel bytes twice": the upload is the only CPU
// -> texel contact; capture_mip_rgba8 is the test-only owner path.
#include <doctest/doctest.h>

#include "gfx_golden.h" // test-side PNG authoring (impl lives in gfx_golden.cpp)

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../ve_test_portability.h" // getpid

#include <ve/cook/cook.h>
#include <ve/gfx/descriptor.h>
#include <ve/gfx/pipeline_cache.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>
#include <ve/gfx/texture.h>

#include "gfx_test_env.h"

namespace {
std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_texgpu_") + name + "_" + std::to_string(ve_test::pid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

constexpr int kTex = 2;
// TL, TR, BL, BR — distinct channels per texel so any mix-up is visible.
// 254 (not 255) for lit channels: the mip1 box average is then 127 EXACTLY
// with no .5 at any intermediate blit tap, so the pin is driver-independent
// (with 255, 127.5 lands mid-round and lavapipe rounds channels differently).
const std::vector<std::uint8_t> kRgba{
    254, 0,   0,   255, // TL red
    0,   254, 0,   255, // TR green
    0,   0,   254, 255, // BL blue
    254, 254, 254, 255, // BR white
};

constexpr const char* kVert = R"GLSL(#version 450
// Fullscreen triangle. Vulkan NDC y=+1 is the BOTTOM of the framebuffer, so
// uv = pos*0.5+0.5 puts uv.y=0 at the framebuffer top = texture row 0.
// No flip anywhere: texture top-down rows == framebuffer top-down rows.
layout(location = 0) out vec2 v_uv;
void main() {
    const vec2 p[3] = { vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0) };
    gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0);
    v_uv = p[gl_VertexIndex] * 0.5 + 0.5;
}
)GLSL";

constexpr const char* kFrag = R"GLSL(#version 450
layout(binding = 0) uniform sampler2D t_base;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() { out_color = texture(t_base, v_uv); }
)GLSL";
} // namespace

TEST_CASE("gfx_texture_sample") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 texture test skipped");
        return;
    }

    // ---- 1. authored PNG -> cooklet -> TEX0 -> CpuTexture (the 3.2-era path) ----
    const std::string src = tmp_root("src");
    const std::string out = tmp_root("out");
    std::string err;
    const std::string png = (std::filesystem::path(src) / "quad2.png").string();
    REQUIRE_MESSAGE(gfx_golden::write_png_rgba8(png, kRgba.data(), kTex, kTex, err), err);
    auto cook = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(cook.ok, cook.error);

    ve::gfx::CpuTexture cpu;
    REQUIRE_MESSAGE(ve::gfx::load_cooked_texture_file(out + "/units/quad2.png.ck", cpu, &err), err);
    CHECK(cpu.pixels == kRgba);

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    // ---- 2. upload: VkImage + upload-generated mip chain ----------------------
    auto tex = ve::gfx::gpu_texture::create(*dev, cpu);
    REQUIRE_MESSAGE(tex.has_value(), tex.error());
    CHECK(tex->width() == 2u);
    CHECK(tex->height() == 2u);
    CHECK(tex->mip_levels() == 2u); // 2x2 -> 1x1

    auto mip0 = tex->capture_mip_rgba8(0);
    REQUIRE_MESSAGE(mip0.has_value(), mip0.error());
    CHECK(*mip0 == kRgba); // staging -> image round trip is byte-exact

    auto mip1 = tex->capture_mip_rgba8(1);
    REQUIRE_MESSAGE(mip1.has_value(), mip1.error());
    REQUIRE(mip1->size() == 4u);
    // Spec LINEAR down 2->1 taps all four texels with weight 1/4:
    // R=(254+0+0+254)/4 = G=(0+254+0+254)/4 = B=(0+0+254+254)/4 = 127
    // exactly — every intermediate tap is whole, so no driver may differ.
    CHECK((*mip1)[0] == 127);
    CHECK((*mip1)[1] == 127);
    CHECK((*mip1)[2] == 127);
    CHECK((*mip1)[3] == 255);

    // Out-of-range capture: error, never UB/crash.
    CHECK_FALSE(tex->capture_mip_rgba8(2).has_value());

    // ---- 3. shaders through the real cook path --------------------------------
    write_file(std::filesystem::path(src) / "s.vert", kVert);
    write_file(std::filesystem::path(src) / "s.frag", kFrag);
    auto cook2 = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(cook2.ok, cook2.error);
    ve::cook::Unit u_v, u_f;
    REQUIRE(ve::cook::read_unit_file(out + "/units/s.vert.ck", u_v, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/s.frag.ck", u_f, &err));
    auto sm_v = ve::gfx::shader_module::create(*dev, u_v.payload);
    auto sm_f = ve::gfx::shader_module::create(*dev, u_f.payload);
    REQUIRE(sm_v.has_value());
    REQUIRE(sm_f.has_value());

    // ---- 4. static sampler + descriptor plumbing ------------------------------
    auto sampler = ve::gfx::create_static_sampler(*dev);
    REQUIRE_MESSAGE(sampler.has_value(), sampler.error());
    auto dsl = ve::gfx::sampler_set_layout::create(*dev, 0, VK_SHADER_STAGE_FRAGMENT_BIT);
    REQUIRE_MESSAGE(dsl.has_value(), dsl.error());
    const VkDescriptorSetLayout set_layouts[1] = {dsl->handle()};
    const VkPipelineLayoutCreateInfo lci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .setLayoutCount = 1, .pSetLayouts = set_layouts,
        .pushConstantRangeCount = 0, .pPushConstantRanges = nullptr,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lci, nullptr, &layout) == VK_SUCCESS);

    auto pool = ve::gfx::descriptor_pool::create(*dev, 4);
    REQUIRE_MESSAGE(pool.has_value(), pool.error());
    auto set = pool->allocate(dsl->handle());
    REQUIRE_MESSAGE(set.has_value(), set.error());
    pool->write_sampler(*set, 0, *sampler, tex->view());

    // ---- 5. textured pipeline through the 3.4 cache ---------------------------
    ve::gfx::pipeline_desc desc{};
    desc.layout = layout;
    desc.stages = {
        {VK_SHADER_STAGE_VERTEX_BIT, sm_v->handle(), "main"},
        {VK_SHADER_STAGE_FRAGMENT_BIT, sm_f->handle(), "main"},
    };
    desc.cull = VK_CULL_MODE_NONE; // fullscreen triangle winding is a formality
    desc.depth_test = false;
    desc.depth_write = false; // no depth attachment in this pass
    desc.color_formats = {VK_FORMAT_B8G8R8A8_UNORM};
    desc.blends.emplace_back(); // default opaque attachment state
    desc.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);

    auto pres = ve::gfx::offscreen_presenter::create(*dev, 64, 64);
    REQUIRE_MESSAGE(pres.has_value(), pres.error());

    ve::gfx::pipeline_cache cache(*dev);
    auto pipe = cache.get(desc);
    REQUIRE_MESSAGE(pipe.has_value(), pipe.error());

    // ---- 6. draw + byte-exact deep-interior pins ------------------------------
    VkCommandBuffer cmd = pres->begin_command();
    REQUIRE(cmd != VK_NULL_HANDLE);
    REQUIRE(pres->begin_pass(cmd));

    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = pres->color_view();
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {0.0f, 0.0f, 0.0f, 1.0f}; // unused: triangle covers all

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {64, 64}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);

    const VkViewport vpt{0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {64, 64}};
    vkCmdSetViewport(cmd, 0, 1, &vpt);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &*set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
    REQUIRE(pres->end_pass(cmd));
    REQUIRE(pres->submit(cmd));

    // 2x2 magnified 32x into B8G8R8A8: each texel is a 32x32 block. Pins sit
    // at block centers, >= 14 px from any texel edge ("deep interior").
    const auto px = pres->capture_rgba8();
    REQUIRE(px.size() == 64u * 64u * 4u);
    const auto bgra = [&](int x, int y) {
        const auto i = (static_cast<std::size_t>(y) * 64 + x) * 4;
        return std::array{px[i], px[i + 1], px[i + 2], px[i + 3]};
    };
    auto expect_px = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const auto p = bgra(x, y);
        // doctest forbids chained && inside the expression: one bool, then CHECK.
        const bool ok = p[0] == b && p[1] == g && p[2] == r && p[3] == 255;
        CHECK_MESSAGE(ok, "(" << x << "," << y << ") got B=" << int(p[0])
                              << " G=" << int(p[1]) << " R=" << int(p[2])
                              << " A=" << int(p[3]));
    };
    expect_px(16, 16, 254, 0, 0);     // TL red
    expect_px(47, 16, 0, 254, 0);     // TR green
    expect_px(16, 47, 0, 0, 254);     // BL blue
    expect_px(47, 47, 254, 254, 254); // BR white
    expect_px(1, 1, 254, 0, 0);       // near corner, still texel TL
    expect_px(62, 33, 254, 254, 254); // deep in BR block (x=62>32, y=33>=32)

    cache.clear();
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);
    vkDestroySampler(dev->handle(), *sampler, nullptr);

    gfx_test::check_clean_validation(*inst);

    std::filesystem::remove_all(src);
    std::filesystem::remove_all(out);
}
