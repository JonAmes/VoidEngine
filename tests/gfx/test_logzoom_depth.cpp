// test: depth_zfight_golden — 8.10 GPU half of log-zoom + reversed-Z float
// depth, on the lavapipe Tier-2 gate (#35).
//
// Contract pinned (PLAN scale bullet; #43 AU-scale-drag dependency):
//   * ONE pass, ONE frustum, spans view distances 1.5 m .. 2^37 m (~0.9 AU):
//     near-surface edges AND AU-scale edges resolve and z-order correctly in
//     the SAME frame, in BOTH draw orders (depth test, not painter's);
//   * depth is the LOGARITHMIC reversed mapping (near -> 1, far -> 0) from
//     the sanctioned fragment refinement documented in projection.h, stored
//     in the D32_SFLOAT attachment (float24+ per PLAN; UNORM would quantize
//     octaves uniformly);
//   * the VR off-axis rule survives: the frustum is a hand-authored
//     ASYMMETRIC tangent set (converged per-eye shape) and every quad's
//     screen placement is predicted from the 1.6 off-axis x/y map —
//     log-zoom perturbs x/y nowhere;
//   * strict golden PNG (byte-exact on the deterministic gate, 2.11 policy)
//     + programmatic color and DEPTH readback pins.
//
// Pin geometry (64x64; n=1, f=2^38 m = 38-octave span; tangents l=-0.75,
// r=0.45, b=-0.4, t=0.6). Off-axis x/y maps (Vulkan, y-down framebuffer):
//   ndc.x = (5/3)·x/d + 0.25   -> px = 32 + 32·ndc.x
//   ndc.y = -2·y/d + 0.2       -> py = 32 + 32·ndc.y = 38.4 - 64·y/d
// Depth budget: z(d) = (38 - log2 d) / 38  (reversed: near -> 1, far -> 0).
//   A white near occluder: d=1.5    px[31.1,48.9] py[27.7,49.1] z=(38-log2 1.5)/38
//   B red near quad:       d=2      px[26.7,53.3] py[22.4,54.4] z=37/38
//   C green far quad:      d=2^37   px[ 2.7,29.3] py[19.2,44.8] z=1/38
//   D white far-front:     d=2^37-2e6 px[10.7,21.3] py[25.6,38.4] z=1/38+~5.6e-7
// D rides exactly 2000 km in front of C at ~1.37e11 m — the far z-fight pair:
// the D-vs-C depth gap is ~300 fp32 ulps of the actual attachment, ordered
// identically in both draw orders (a 24-bit UNORM bucket at that reversed-
// linear z could not separate them; the log budget spreads the 38-octave
// span evenly, ~1 ulp per 2.6e-5 relative).
#include <doctest/doctest.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>

#include "gfx_test_env.h"
#include "gfx_golden.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
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
             ("ve_logzoom_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

// std140 push block: mat4 (64) + color (16) + qa (16) + qb (16) = 112 bytes.
// qa = (half.x, half.y, z_view, inv_log2_span); qb = (center.x, center.y,
// log2(far), pad). Center rides so AU-scale quads can sit anywhere on screen
// without fp32 world-absolute coords (8.9: these are camera-relative).
struct alignas(16) PushLog {
    float mvp[16];
    float color[4];
    float qa[4];
    float qb[4];
};

constexpr int kW = 64, kH = 64;
constexpr float kD37 = 137438953472.0f;            // 2^37 m (exact in fp32)
constexpr float kDF = 137438953472.0f - 2000000.0f; // far-front: 2000 km nearer
constexpr float kInvSpan = 1.0f / 38.0f, kFarLog2 = 38.0f;
} // namespace

TEST_CASE("depth_zfight_golden") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 depth test skipped");
        return;
    }

    // ---- 1. cooked log-depth quad shaders (runtime shaders are cooked) ---
    const auto src = tmp_root("src");
    write_file(std::filesystem::path(src) / "quad.vert", R"GLSL(#version 450
layout(push_constant) uniform Push { mat4 mvp; vec4 color; vec4 qa; vec4 qb; } push;
layout(location = 0) out vec4 col;
void main() {
    // Two-triangle quad from gl_VertexIndex; half-extent + CENTER + depth
    // via push (camera-relative fp32, 8.9). sq spans the full square so
    // qa.xy is exactly the half-extent.
    const vec2 sq[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                               vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    const vec2 p = push.qb.xy + sq[gl_VertexIndex] * push.qa.xy;
    gl_Position = push.mvp * vec4(p, push.qa.z, 1.0);
    col = push.color;
}
)GLSL");
    write_file(std::filesystem::path(src) / "quad.frag", R"GLSL(#version 450
layout(push_constant) uniform Push { mat4 mvp; vec4 color; vec4 qa; vec4 qb; } push;
layout(location = 0) in vec4 col;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = col;
    // The ONE sanctioned log refinement (projection.h 8.10). GLSL window-coord
    // trap: gl_FragCoord.w == 1/w_clip == 1/d exactly for these pinhole
    // builders (w = -z_view = view distance), so log2(d) = -log2(FragCoord.w).
    gl_FragDepth = clamp((push.qb.z + log2(gl_FragCoord.w)) * push.qa.w, 0.0, 1.0);
}
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
    auto pres = ve::gfx::offscreen_presenter::create(*dev, kW, kH);
    REQUIRE(pres.has_value());
    REQUIRE(pres->depth_view() != VK_NULL_HANDLE);

    auto vert = ve::gfx::shader_module::create(*dev, vu.payload);
    auto frag = ve::gfx::shader_module::create(*dev, fu.payload);
    REQUIRE(vert.has_value());
    REQUIRE(frag.has_value());

    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushLog),
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

    // ---- 2. pipeline: depth ON, reversed GREATER, D32_SFLOAT -------------
    const VkPipelineVertexInputStateCreateInfo vi{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .vertexBindingDescriptionCount = 0, .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0, .pVertexAttributeDescriptions = nullptr,
    };
    const VkPipelineInputAssemblyStateCreateInfo ia{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };
    const VkPipelineViewportStateCreateInfo vp{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .viewportCount = 1, .pViewports = nullptr,
        .scissorCount = 1, .pScissors = nullptr,
    };
    const VkPipelineRasterizationStateCreateInfo rs{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .depthClampEnable = VK_FALSE, .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f, .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f, .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 1.0f, .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE, .alphaToOneEnable = VK_FALSE,
    };
    const VkPipelineDepthStencilStateCreateInfo ds{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_GREATER, // reversed Z: larger = nearer
        .depthBoundsTestEnable = VK_FALSE, .stencilTestEnable = VK_FALSE,
        .front = {}, .back = {},
        .minDepthBounds = 0.0f, .maxDepthBounds = 1.0f,
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
        .pNext = nullptr, .flags = 0,
        .logicOpEnable = VK_FALSE, .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1, .pAttachments = &blend,
        .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dsy{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .dynamicStateCount = 2, .pDynamicStates = dyn,
    };
    const VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr, .viewMask = 0,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat = ve::gfx::offscreen_presenter::kDepthFormat,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
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
        .basePipelineHandle = VK_NULL_HANDLE, .basePipelineIndex = -1,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    REQUIRE(vkCreateGraphicsPipelines(dev->handle(), VK_NULL_HANDLE, 1, &pci2, nullptr, &pipe) == VK_SUCCESS);

    // ---- 3. MVP: log-zoom builder (off-axis x/y, hyperbolic hw z row) ----
    const ve::core::math::mat4f mvp = ve::core::math::perspective_logzoom(
        1.0f, 274877906944.0f, -0.75f, 0.45f, -0.4f, 0.6f);

    struct Quad {
        float z_view, half_x, half_y, cx, cy;
        float rgba[4];
    };
    const Quad quads[4] = {
        // A white near occluder
        {-1.5f, 0.25f, 0.25f, 0.f, 0.f, {1.f, 1.f, 1.f, 1.f}},
        // B red near
        {-2.0f, 0.5f, 0.5f, 0.f, 0.f, {1.f, 0.f, 0.f, 1.f}},
        // D white far-front (its position rides with C's center)
        {-kDF, 0.1f * kD37, 0.1f * kD37, -0.45f * kD37, 0.1f * kD37, {1.f, 1.f, 1.f, 1.f}},
        // C green far
        {-kD37, 0.25f * kD37, 0.2f * kD37, -0.45f * kD37, 0.1f * kD37, {0.f, 1.f, 0.f, 1.f}},
    };
    auto make_push = [&](const Quad& q) {
        PushLog p{};
        std::copy(&mvp.m[0], &mvp.m[0] + 16, p.mvp);
        std::copy(q.rgba, q.rgba + 4, p.color);
        p.qa[0] = q.half_x; p.qa[1] = q.half_y; p.qa[2] = q.z_view; p.qa[3] = kInvSpan;
        p.qb[0] = q.cx; p.qb[1] = q.cy; p.qb[2] = kFarLog2; p.qb[3] = 0.f;
        return p;
    };
    const PushLog push_a = make_push(quads[0]);
    const PushLog push_b = make_push(quads[1]);
    const PushLog push_c = make_push(quads[2]); // D
    const PushLog push_d = make_push(quads[3]); // C

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

        VkRenderingAttachmentInfo depth_att{};
        depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_att.imageView = pres->depth_view();
        depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_att.clearValue.depthStencil.depth = 0.0f; // reversed far == 0

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
        if (near_first) { draw(push_a); draw(push_b); draw(push_c); draw(push_d); }
        else            { draw(push_d); draw(push_c); draw(push_b); draw(push_a); }

        vkCmdEndRendering(cmd);
        REQUIRE(pres->end_pass(cmd));
        REQUIRE(pres->submit(cmd));
    };

    // ---- 4. pins (identical in BOTH draw orders) --------------------------
    // Host mirror of the shader's fp32 arithmetic on the SAME push-rounded
    // distances the GPU sees.
    const double dA = -static_cast<double>(quads[0].z_view);
    const double dB = -static_cast<double>(quads[1].z_view);
    const double dD = -static_cast<double>(quads[2].z_view); // == 137438951424.0 (fp32 push)
    const double dC = -static_cast<double>(quads[3].z_view); // == 2^37 exactly
    const auto z_exp = [](double d) { return (38.0 - std::log2(d)) / 38.0; };
    constexpr double kZTol = 3e-7; // shader log2f vs host log2: <= ~2 ulp of the z value

    std::vector<std::uint8_t> rgba_bgra;
    auto check_pins = [&](const char* tag) {
        const auto px = pres->capture_rgba8();
        REQUIRE(px.size() == static_cast<std::size_t>(kW) * kH * 4u);
        rgba_bgra = px;
        const auto bgra = [&](int x, int y) {
            const std::size_t o = (static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4u;
            return std::array<std::uint8_t, 4>{px[o], px[o + 1], px[o + 2], px[o + 3]};
        };
        const auto is_white = [](std::array<std::uint8_t, 4> c) {
            return c[0] == 255u && c[1] == 255u && c[2] == 255u && c[3] == 255u; };
        const auto is_red = [](std::array<std::uint8_t, 4> c) {
            return c[0] == 0u && c[1] == 0u && c[2] == 255u && c[3] == 255u; };
        const auto is_green = [](std::array<std::uint8_t, 4> c) {
            return c[0] == 0u && c[1] == 255u && c[2] == 0u && c[3] == 255u; };
        const auto is_clear_blue = [](std::array<std::uint8_t, 4> c) {
            return c[0] == 255u && c[1] == 0u && c[2] == 0u && c[3] == 255u; };

        const auto dz = pres->capture_depth_f32();
        REQUIRE(dz.size() == static_cast<std::size_t>(kW) * kH);
        const auto depth_at = [&](int x, int y) {
            return dz[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)];
        };

        // --- near z-fight pair: A (d=1.5) occludes B (d=2) in the overlap
        CHECK_MESSAGE(is_white(bgra(36, 36)), tag << " A over B");
        CHECK_MESSAGE(std::abs(depth_at(36, 36) - z_exp(dA)) < kZTol,
                      tag << " z(A)=" << depth_at(36, 36) << " exp=" << z_exp(dA));
        // B visible outside A (both pins outside A's px/py span, inside B):
        CHECK_MESSAGE(is_red(bgra(50, 40)), tag << " B red");
        CHECK_MESSAGE(std::abs(depth_at(50, 40) - z_exp(dB)) < kZTol,
                      tag << " z(B)=" << depth_at(50, 40) << " exp=" << z_exp(dB));
        CHECK_MESSAGE(is_red(bgra(50, 48)), tag << " B red 2");
        CHECK_MESSAGE(std::abs(depth_at(50, 48) - z_exp(dB)) < kZTol, "z(B2)");

        // --- AU z-fight pair: D (2^37-2e6 m) in front of C (2^37 m)
        CHECK_MESSAGE(is_white(bgra(16, 32)), tag << " D over C");
        CHECK_MESSAGE(is_green(bgra(16, 40)), tag << " C green below D");
        CHECK_MESSAGE(is_green(bgra(8, 24)), tag << " C green left of D");
        const double zD = depth_at(16, 32), zC = depth_at(16, 40), zC2 = depth_at(8, 24);
        CHECK_MESSAGE(std::abs(zD - z_exp(dD)) < kZTol,
                      tag << " z(D)=" << zD << " exp=" << z_exp(dD));
        CHECK_MESSAGE(std::abs(zC - z_exp(dC)) < kZTol,
                      tag << " z(C)=" << zC << " exp=" << z_exp(dC));
        CHECK_MESSAGE(std::abs(zC2 - z_exp(dC)) < kZTol, "z(C2)");
        // The 2000 km separation at ~1.37e11 m is ORDERED with ~300-ulp margin
        // (true gap ~5.6e-7; pin at 3e-7, errors <= ~1e-7 each side).
        CHECK_MESSAGE(zD > zC + 3e-7, tag << " zD-zC=" << (zD - zC));
        // The hardware fallback (hyperbolic n/d ~ 7.3e-12 here) is 9 orders of
        // magnitude off: the log refinement is provably what got written.
        CHECK(zC > 1e-4);
        CHECK(zD < 1.0);

        // --- untouched: clear color + reversed far == exactly 0
        CHECK_MESSAGE(is_clear_blue(bgra(2, 2)), tag << " clear color");
        CHECK(depth_at(2, 2) == 0.0f);
    };

    draw_frame(/*near_first=*/true);
    check_pins("near-first");
    std::vector<std::uint8_t> first = rgba_bgra;
    draw_frame(/*near_first=*/false); // occlusion = depth test, not order
    check_pins("far-first");
    CHECK_MESSAGE(std::memcmp(first.data(), rgba_bgra.data(), first.size()) == 0,
                  "draw-order dependence: captures must be byte-identical");

    // ---- 5. strict golden (byte-exact on the deterministic lavapipe gate) --
    std::vector<std::uint8_t> rgba(first.size());
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = first[i + 2]; // B8G8R8A8 -> RGBA
        rgba[i + 1] = first[i + 1];
        rgba[i + 2] = first[i + 0];
        rgba[i + 3] = first[i + 3];
    }
    const auto gv = gfx_golden::check_golden("logzoom_zfight_64.png", rgba.data(), kW, kH, {});
    CHECK_MESSAGE(gv.ok, gv.message);

    vkDestroyPipeline(dev->handle(), pipe, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);
}
