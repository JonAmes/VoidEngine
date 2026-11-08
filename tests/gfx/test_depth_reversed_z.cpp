// test: gfx_depth_reversed_z — 3.3 depth attachment + reversed-Z.
// Two overlapping quads at different distances through a reversed-Z
// projection (clear depth = 0 == far, compare GREATER): color readback
// proves the NEARER quad occludes the farther one in BOTH draw orders
// (depth test, not painter's algorithm), and depth-texel pins verify the
// attachment itself: cleared texels == 0 (reversed far), visible-texel
// depths match the Phase 1 reversed builder.
//
// Pin geometry (fov 90 deg, aspect 1 → ndc.x = x_view/dist exactly):
//   near quad: z_view=-2, half-extent 0.8 -> ndc +/-0.4 -> px [19.2, 44.8]
//   far  quad: z_view=-4, half-extent 2.4 -> ndc +/-0.6 -> px [12.8, 51.2]
// Ring pins sit >= 3 px from every quad edge (half-pixel rule, #35/#36).
// Depth pins (n=0.5 f=20 reversed: clip.z = z/39 + 20/39, w = dist):
//   d=2: ndc.z = (18/39)/2 = 3/13 | d=4: (16/39)/4 = 4/39 | clear: 0
#include <doctest/doctest.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>

#include "gfx_test_env.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <ve/cook/cook.h>
#include <ve/core/math/mat4.h>
#include <ve/core/math/projection.h>

namespace {
std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_depth_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

// std140 push block: mat4 (64) + color (16) + quad (16) = 96 bytes.
// quad.xy = half-extents, quad.z = view-space z (negative distance).
struct alignas(16) PushQuad {
    float mvp[16];
    float color[4];
    float quad[4];
};
} // namespace

TEST_CASE("gfx_depth_reversed_z") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 depth test skipped");
        return;
    }

    // ---- 1. cooked quad shaders (runtime shaders are always cooked) ------
    const auto src = tmp_root("src");
    write_file(std::filesystem::path(src) / "quad.vert", R"GLSL(#version 450
layout(push_constant) uniform Push { mat4 mvp; vec4 color; vec4 quad; } push;
layout(location = 0) out vec4 col;
void main() {
    // Two-triangle quad from gl_VertexIndex; extent + depth via push quad.
    const vec2 sq[6] = vec2[6](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
                               vec2(-0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
    const vec2 p = sq[gl_VertexIndex] * push.quad.xy;
    gl_Position = push.mvp * vec4(p, push.quad.z, 1.0);
    col = push.color;
}
)GLSL");
    write_file(std::filesystem::path(src) / "quad.frag", R"GLSL(#version 450
layout(push_constant) uniform Push { mat4 mvp; vec4 color; vec4 quad; } push;
layout(location = 0) in vec4 col;
layout(location = 0) out vec4 outColor;
void main() { outColor = col; }
)GLSL");
    const auto out = tmp_root("out");
    REQUIRE(ve::cook::cook_incremental(src, out).ok);
    ve::cook::Unit vu, fu;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out + "/units/quad.vert.ck", vu, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/quad.frag.ck", fu, &err));

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());
    auto pres = ve::gfx::offscreen_presenter::create(*dev, 64, 64);
    REQUIRE(pres.has_value());

    // ---- 2. the 3.3 contract: the target has depth, but the consumer
    // surface (color_view/capture) is untouched — depth is pass plumbing ---
    CHECK(pres->depth_view() != VK_NULL_HANDLE);
    CHECK(pres->color_view() != VK_NULL_HANDLE);

    auto vert = ve::gfx::shader_module::create(*dev, vu.payload);
    auto frag = ve::gfx::shader_module::create(*dev, fu.payload);
    REQUIRE(vert.has_value());
    REQUIRE(frag.has_value());

    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushQuad),
    };
    const VkPipelineLayoutCreateInfo lci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 0,
        .pSetLayouts = nullptr,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lci, nullptr, &layout) == VK_SUCCESS);

    // ---- 3. pipeline: depth test ON, reversed-Z compare (GREATER) -------
    const VkPipelineVertexInputStateCreateInfo vi{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr,
    };
    const VkPipelineInputAssemblyStateCreateInfo ia{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };
    const VkPipelineViewportStateCreateInfo vp{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr, // dynamic
        .scissorCount = 1,
        .pScissors = nullptr, // dynamic
    };
    const VkPipelineRasterizationStateCreateInfo rs{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 1.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    const VkPipelineDepthStencilStateCreateInfo ds{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_GREATER, // reversed Z: larger = nearer
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
        .front = {},
        .back = {},
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 1.0f,
    };
    const VkPipelineColorBlendAttachmentState blend{
        .blendEnable = VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo cb{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &blend,
        .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dsy{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn,
    };
    const VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat = ve::gfx::offscreen_presenter::kDepthFormat,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    // stages array must outlive pci; built locally with the full state.
    const VkPipelineShaderStageCreateInfo stage_arr[2]{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = nullptr, .flags = 0, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert->handle(), .pName = "main", .pSpecializationInfo = nullptr},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = nullptr, .flags = 0, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag->handle(), .pName = "main", .pSpecializationInfo = nullptr},
    };
    const VkGraphicsPipelineCreateInfo pci2{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .flags = 0,
        .stageCount = 2, .pStages = stage_arr,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pTessellationState = nullptr,
        .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds,
        .pColorBlendState = &cb, .pDynamicState = &dsy,
        .layout = layout,
        .renderPass = VK_NULL_HANDLE, // dynamic rendering (VK1.3, #37)
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    REQUIRE(vkCreateGraphicsPipelines(dev->handle(), VK_NULL_HANDLE, 1, &pci2, nullptr, &pipe) == VK_SUCCESS);

    // ---- 4. MVP: reversed-Z builder straight from Phase 1 ----------------
    ve::core::math::mat4f mvp = ve::core::math::perspective(
        ve::core::math::deg_to_rad(90.0f), 1.0f, 0.5f, 20.0f,
        ve::core::math::depth_range::reversed_1_to_0);

    struct QuadCfg { PushQuad p; };
    auto make_push = [&](float z, float half, const float rgba[4]) {
        PushQuad q{};
        std::copy(&mvp.m[0], &mvp.m[0] + 16, q.mvp);
        std::copy(rgba, rgba + 4, q.color);
        q.quad[0] = half * 2.0f; // shader multiplies by the +-0.5 square
        q.quad[1] = half * 2.0f;
        q.quad[2] = z;          // view-space z (negative distance)
        q.quad[3] = 0.0f;
        return q;
    };
    const float red[4]{1.0f, 0.0f, 0.0f, 1.0f};     // near
    const float green[4]{0.0f, 1.0f, 0.0f, 1.0f};   // far
    PushQuad near_q = make_push(-2.0f, 0.8f, red);
    PushQuad far_q = make_push(-4.0f, 2.4f, green);

    auto draw_frame = [&](bool near_first) {
        VkCommandBuffer cmd = pres->begin_command();
        REQUIRE(cmd != VK_NULL_HANDLE);
        REQUIRE(pres->begin_pass(cmd));

        VkRenderingAttachmentInfo att{};
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = pres->color_view();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color.float32[0] = 0.0f; // clear blue (B8G8R8A8: B=1)
        att.clearValue.color.float32[1] = 0.0f;
        att.clearValue.color.float32[2] = 1.0f;
        att.clearValue.color.float32[3] = 1.0f;

        // Reversed-Z clear: depth 0 == far. With GREATER, every real
        // surface (depth > 0) beats it; untouched texels stay exactly 0.
        VkRenderingAttachmentInfo depth_att{};
        depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_att.imageView = pres->depth_view();
        depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_att.clearValue.depthStencil.depth = 0.0f;

        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {64, 64}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        ri.pDepthAttachment = &depth_att;
        vkCmdBeginRendering(cmd, &ri);

        const VkViewport vpt{0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {64, 64}};
        vkCmdSetViewport(cmd, 0, 1, &vpt);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

        const auto draw = [&](const PushQuad& q) {
            vkCmdPushConstants(cmd, layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(q), &q);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        };
        if (near_first) { draw(near_q); draw(far_q); } else { draw(far_q); draw(near_q); }

        vkCmdEndRendering(cmd);
        REQUIRE(pres->end_pass(cmd));
        REQUIRE(pres->submit(cmd));
    };

    // ---- 5. pins (identical in BOTH draw orders) -------------------------
    constexpr float kDepthNear = 3.0f / 13.0f; // d=2 reversed pin
    constexpr float kDepthFar = 4.0f / 39.0f;  // d=4 reversed pin
    auto check_pins = [&]() {
        const auto px = pres->capture_rgba8();
        REQUIRE(px.size() == 64u * 64u * 4u);
        const auto bgra = [&](int x, int y) {
            const std::size_t o = (static_cast<std::size_t>(y) * 64u + static_cast<std::size_t>(x)) * 4u;
            return std::array<std::uint8_t, 4>{px[o], px[o + 1], px[o + 2], px[o + 3]};
        };
        const auto is_red = [](std::array<std::uint8_t, 4> c) {
            return c[0] == 0u && c[1] == 0u && c[2] == 255u && c[3] == 255u;
        };
        const auto is_green = [](std::array<std::uint8_t, 4> c) {
            return c[0] == 0u && c[1] == 255u && c[2] == 0u && c[3] == 255u;
        };
        const auto is_clear_blue = [](std::array<std::uint8_t, 4> c) {
            return c[0] == 255u && c[1] == 0u && c[2] == 0u && c[3] == 255u;
        };

        const auto dz = pres->capture_depth_f32();
        REQUIRE(dz.size() == 64u * 64u);
        const auto depth_at = [&](int x, int y) {
            return dz[static_cast<std::size_t>(y) * 64u + static_cast<std::size_t>(x)];
        };

        // center: NEAR wins (red + near depth pin), regardless of order
        CHECK(is_red(bgra(32, 32)));
        CHECK(depth_at(32, 32) == doctest::Approx(kDepthNear).epsilon(1e-4));

        // ring: only the far quad covers these pins (green + far depth pin)
        for (auto [x, y] : std::to_array<std::pair<int, int>>({{16, 32}, {48, 32}, {32, 16}, {32, 48}})) {
            CHECK_MESSAGE(is_green(bgra(x, y)), "(" << x << "," << y << ") color");
            CHECK_MESSAGE(depth_at(x, y) == doctest::Approx(kDepthFar).epsilon(1e-4),
                          "(" << x << "," << y << ") depth");
        }

        // untouched: clear color + reversed far == exactly 0
        CHECK(is_clear_blue(bgra(2, 2)));
        CHECK(depth_at(2, 2) == 0.0f);
    };

    draw_frame(/*near_first=*/true);
    check_pins();
    draw_frame(/*near_first=*/false); // occlusion must be the depth test, not order
    check_pins();

    vkDestroyPipeline(dev->handle(), pipe, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);
}
