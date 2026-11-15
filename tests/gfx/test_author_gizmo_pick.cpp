// test: author_gizmo_pick — 8.19 Tier-2 pick proof (decision #43 mode A).
//
// The mode-A pick contract: the SAME off-axis pinhole x/y map that
// author_gizmos::project_px uses to turn a world handle into a cursor
// pixel is the map the GPU rasterizes through. This test renders gizmo
// markers (8.19's minimal visual gizmo: screen-space colored quads at the
// handle's projected position, one per entity) through the 8.10 log-zoom
// projection, then pins:
//   * every marker's pixels land exactly where project_px predicted
//     (CPU pick cursor == GPU placement, no second projection math),
//   * an overlapping handle pair resolves to the NEARER one at the shared
//     pixel in BOTH draw orders (depth test, not order — the pick's
//     nearer-d tie-break and the GPU's reversed-Z agree),
//   * captures are byte-identical across draw orders.
//
// Tier 2: lavapipe + validation layer, offscreen, no display. The pins are
// programmatic color/depth reads (no golden PNG: the contract is the px
// prediction, and pinning pixels-by-prediction is the stronger claim).
#include <doctest/doctest.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>

#include "gfx_test_env.h"

#include <ve/cook/author_gizmos.h>
#include <ve/cook/cook.h>
#include <ve/core/math/mat4.h>
#include <ve/core/math/projection.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_gizmo_pick_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

// std140 push block, same shape as the 8.10 quad (mat4 + color + qa + qb).
struct alignas(16) PushLog {
    float mvp[16];
    float color[4];
    float qa[4];   // half_x, half_y, z_view, inv_log2_span
    float qb[4];   // center.x, center.y, log2(far), pad
};

constexpr int kW = 64, kH = 64;
constexpr float kInvSpan = 1.0f / 39.863137138646355f;
constexpr float kFarLog2 = 39.863137138646355f;
}  // namespace

TEST_CASE("author_gizmo_pick") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 pick proof skipped");
        return;
    }

    // ---- 1. gizmo marker shaders through the real incremental cooker ----
    const auto src = tmp_root("src");
    write_file(std::filesystem::path(src) / "gizmo.vert", R"GLSL(#version 450
layout(push_constant) uniform Push { mat4 mvp; vec4 color; vec4 qa; vec4 qb; } push;
void main() {
    // Screen-space marker: center + half-extent in the view's x/y basis at
    // constant view depth (author_gizmos builds one quad per handle).
    const vec2 sq[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                               vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    const vec2 p = push.qb.xy + sq[gl_VertexIndex] * push.qa.xy;
    gl_Position = push.mvp * vec4(p, push.qa.z, exp2(log2(-push.qa.z) * push.qa.w));
}
)GLSL");
    write_file(std::filesystem::path(src) / "gizmo.frag", R"GLSL(#version 450
layout(push_constant) uniform Push { mat4 mvp; vec4 color; vec4 qa; vec4 qb; } push;
layout(location = 0) out vec4 outColor;
void main() { outColor = push.color; }
)GLSL");
    const auto out = tmp_root("out");
    REQUIRE(ve::cook::cook_incremental(src, out).ok);
    ve::cook::Unit vu, fu;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out + "/units/gizmo.vert.ck", vu, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/gizmo.frag.ck", fu, &err));

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());
    auto pres = ve::gfx::offscreen_presenter::create(*dev, kW, kH);
    REQUIRE(pres.has_value());
    REQUIRE(pres->depth_view() != VK_NULL_HANDLE);

    auto vert = ve::gfx::shader_module::create(*dev, vu.payload);
    auto frag = ve::gfx::shader_module::create(*dev, fu.payload);
    REQUIRE(vert.has_value());
    REQUIRE(frag.has_value());

    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = sizeof(PushLog)};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const VkPipelineLayoutCreateInfo lci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .setLayoutCount = 0, .pSetLayouts = nullptr,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lci, nullptr, &layout) == VK_SUCCESS);

    const VkPipelineVertexInputStateCreateInfo vi{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .vertexBindingDescriptionCount = 0, .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0, .pVertexAttributeDescriptions = nullptr};
    const VkPipelineInputAssemblyStateCreateInfo ia{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE};
    const VkPipelineViewportStateCreateInfo vp{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .viewportCount = 1, .pViewports = nullptr,
        .scissorCount = 1, .pScissors = nullptr};
    const VkPipelineRasterizationStateCreateInfo rs{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE, .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f, .depthBiasSlopeFactor = 0.0f, .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE, .minSampleShading = 1.0f, .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE, .alphaToOneEnable = VK_FALSE};
    const VkPipelineDepthStencilStateCreateInfo ds{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_GREATER, .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE, .front = {}, .back = {},
        .minDepthBounds = 0.0f, .maxDepthBounds = 1.0f};
    const VkPipelineColorBlendAttachmentState blend{
        .blendEnable = VK_FALSE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo cb{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .logicOpEnable = VK_FALSE, .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1, .pAttachments = &blend, .blendConstants = {0, 0, 0, 0}};
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dsy{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .dynamicStateCount = 2, .pDynamicStates = dyn};
    const VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr, .viewMask = 0, .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat = ve::gfx::offscreen_presenter::kDepthFormat,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED};
    const VkPipelineShaderStageCreateInfo stage_arr[2]{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = nullptr,
         .flags = 0, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert->handle(),
         .pName = "main", .pSpecializationInfo = nullptr},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = nullptr,
         .flags = 0, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag->handle(),
         .pName = "main", .pSpecializationInfo = nullptr}};
    const VkGraphicsPipelineCreateInfo pci2{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .flags = 0, .stageCount = 2, .pStages = stage_arr,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pTessellationState = nullptr,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dsy,
        .layout = layout, .renderPass = VK_NULL_HANDLE, .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE, .basePipelineIndex = -1};
    VkPipeline pipe = VK_NULL_HANDLE;
    REQUIRE(vkCreateGraphicsPipelines(dev->handle(), VK_NULL_HANDLE, 1, &pci2, nullptr, &pipe) == VK_SUCCESS);

    // ---- 3. the SAME view the authoring pick runs with ------------------
    ve::cook::gizmo_view view;
    view.eye = {0.0, -2.0e9, 0.0};
    view.f = {0.0, 1.0, 0.0};
    view.s = {1.0, 0.0, 0.0};
    view.u = {0.0, 0.0, 1.0};
    view.tan_l = view.tan_b = -0.4;
    view.tan_r = view.tan_t = 0.4;
    view.near_d = 1.0;
    view.far_d = 1.0e12;
    view.width_px = view.height_px = kW;
    const auto mvp = ve::core::math::perspective_logzoom(
        1.0f, 1.0e12f, static_cast<float>(view.tan_l), static_cast<float>(view.tan_r),
        static_cast<float>(view.tan_b), static_cast<float>(view.tan_t));

    // Marker quads built FROM the CPU pick projection: view-frame center
    // via the same basis dots, half-extent sized so each marker spans
    // half_px screen pixels at its own depth.
    struct Marker {
        ve::core::math::dvec3 world;
        float rgba[4];
        int half_px;
    };
    const Marker markers[4] = {
        // red "station" + green "waypoint", well separated on screen
        {{3.0e8, 0.0, 0.0},     {1.f, 0.f, 0.f, 1.f}, 4},
        {{1.0e8, 2.0e6, 0.0},   {0.f, 1.f, 0.f, 1.f}, 2},
        // overlap pair at the frame center: orange NEARER (d 2.010e9),
        // purple farther (d 2.011e9) — ~0.06 px apart, one pick cursor and
        // one pixel cover both.
        {{1.0e6, 1.0e7, 0.0},   {1.f, 0.6f, 0.f, 1.f}, 4},
        {{1.05e6, 1.011e7, 0.0},{1.f, 0.f, 1.f, 1.f}, 4},
    };
    std::vector<std::pair<double, double>> pred_px;  // project_px predictions
    std::vector<PushLog> pushes;
    for (const auto& m : markers) {
        const auto pp = ve::cook::project_px(m.world, view);
        REQUIRE(pp);  // every marker is on screen
        pred_px.push_back(*pp);
        const ve::core::math::dvec3 rel = m.world - view.eye;
        const double d = ve::core::math::dot(rel, view.f);
        REQUIRE(d > view.near_d);
        REQUIRE(d < view.far_d);
        PushLog p{};
        std::copy(&mvp.m[0], &mvp.m[0] + 16, p.mvp);
        std::copy(m.rgba, m.rgba + 4, p.color);
        const double ext = m.half_px * d * (view.tan_r - view.tan_l) / kW;
        p.qa[0] = static_cast<float>(ext); p.qa[1] = static_cast<float>(ext);
        p.qa[2] = static_cast<float>(-d);  p.qa[3] = kInvSpan;
        p.qb[0] = static_cast<float>(ve::core::math::dot(rel, view.s));
        p.qb[1] = static_cast<float>(ve::core::math::dot(rel, view.u));
        p.qb[2] = kFarLog2; p.qb[3] = 0.f;
        pushes.push_back(p);
    }

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
        att.clearValue.color.float32[0] = 0.0f;  // clear blue (B8G8R8A8)
        att.clearValue.color.float32[1] = 0.0f;
        att.clearValue.color.float32[2] = 1.0f;
        att.clearValue.color.float32[3] = 1.0f;
        VkRenderingAttachmentInfo depth_att{};
        depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_att.imageView = pres->depth_view();
        depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_att.clearValue.depthStencil.depth = 0.0f;  // reversed far == 0
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {kW, kH}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        ri.pDepthAttachment = &depth_att;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vpt{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {kW, kH}};
        vkCmdSetViewport(cmd, 0, 1, &vpt);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        const auto draw = [&](const PushLog& p) {
            vkCmdPushConstants(cmd, layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(p), &p);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        };
        if (near_first) { draw(pushes[2]); draw(pushes[3]); draw(pushes[1]); draw(pushes[0]); }
        else            { draw(pushes[0]); draw(pushes[1]); draw(pushes[3]); draw(pushes[2]); }
        vkCmdEndRendering(cmd);
        REQUIRE(pres->end_pass(cmd));
        REQUIRE(pres->submit(cmd));
    };

    std::vector<std::uint8_t> rgba_bgra;
    auto check_pins = [&](const char* tag) {
        const auto px = pres->capture_rgba8();
        REQUIRE(px.size() == static_cast<std::size_t>(kW) * kH * 4u);
        rgba_bgra = px;
        const auto bgra = [&](int x, int y) {
            const std::size_t o = (static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4u;
            return std::array<std::uint8_t, 4>{px[o], px[o + 1], px[o + 2], px[o + 3]};
        };
        const auto is_red    = [](auto c) { return c[0] == 0u   && c[1] == 0u   && c[2] == 255u && c[3] == 255u; };
        const auto is_green  = [](auto c) { return c[0] == 0u   && c[1] == 255u && c[2] == 0u   && c[3] == 255u; };
        const auto is_orange = [](auto c) { return c[0] == 0u   && c[1] == 153u && c[2] == 255u && c[3] == 255u; };

        // The predicted pixel of each isolated marker carries ITS color:
        // the pick's cursor math IS the GPU's placement math.
        const int p0 = static_cast<int>(std::floor(pred_px[0].first));
        const int q0 = static_cast<int>(std::floor(pred_px[0].second));
        CHECK_MESSAGE(is_red(bgra(p0, q0)), tag << " red marker missing at predicted px ("
                      << pred_px[0].first << ", " << pred_px[0].second << ")");
        CHECK_MESSAGE(is_red(bgra(p0 - 2, q0)), tag << " red span (pick radius)");
        CHECK_MESSAGE(is_red(bgra(p0, q0 + 2)), tag << " red span vertical");
        const int p1 = static_cast<int>(std::floor(pred_px[1].first));
        const int q1 = static_cast<int>(std::floor(pred_px[1].second));
        CHECK_MESSAGE(is_green(bgra(p1, q1)), tag << " green marker missing at predicted px ("
                      << pred_px[1].first << ", " << pred_px[1].second << ")");

        // Overlap pixel: orange (nearer d) wins the shared pixel in BOTH
        // orders — the same nearer-d rule pick_at's radius scan applies.
        const int p2 = static_cast<int>(std::floor(pred_px[2].first));
        const int q2 = static_cast<int>(std::floor(pred_px[2].second));
        CHECK_MESSAGE(is_orange(bgra(p2, q2)), tag << " overlap px shows NEARER orange");
        CHECK(std::abs(pred_px[2].first - pred_px[3].first) < 0.5);    // cursors coincide
        CHECK(std::abs(pred_px[2].second - pred_px[3].second) < 0.5);  // under one pick radius

        // Depth agrees with the pick distances (reversed Z: nearer larger).
        const auto dz = pres->capture_depth_f32();
        REQUIRE(dz.size() == static_cast<std::size_t>(kW) * kH);
        const auto depth_at = [&](int x, int y) {
            return dz[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)];
        };
        CHECK(depth_at(p0, q0) > depth_at(p2, q2));  // red d=2e9 nearer than orange 2.01e9

        // untouched sky: clear blue + reversed far == exactly 0
        CHECK(bgra(2, 61)[2] == 0u);
        CHECK(bgra(2, 61)[0] == 255u);
        CHECK(depth_at(2, 61) == 0.0f);
    };

    draw_frame(true);
    check_pins("near-first");
    std::vector<std::uint8_t> first = rgba_bgra;
    draw_frame(false);
    check_pins("far-first");
    CHECK_MESSAGE(std::memcmp(first.data(), rgba_bgra.data(), first.size()) == 0,
                  "draw-order dependence: captures must be byte-identical");

    vkDestroyPipeline(dev->handle(), pipe, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);
}
