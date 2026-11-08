// test: gfx_instanced_eyes — checklist 3.9: per-eye single-pass instanced
// rendering. ONE vkCmdDraw draws N=4 mesh instances × E=2 eyes: the eye
// index derives from the instance id (ordered rig eyes = instance eye index,
// camera.h 3.2 contract), per-eye MVPs and per-instance model matrices ride
// a UBO, and the vertex shader selects the eye's viewport via
// gl_ViewportIndex (VK_EXT_shader_viewport_index_layer — Vulkan 1.3 core,
// enabled-if-advertised in device.cpp). Per-eye viewports+scissors live on
// one wide 128x64 RT. Desktop is the same path with one centered eye
// (vp_count=1 pipeline variant) — and its output is byte-compared against
// the non-instanced sequence (N draws with firstInstance), which is the
// "single-pass == what desktop would draw" proof (decision #34 geometry is
// unit-tested in 3.2; this test pins the DRAW PATH).
//
// Pins (all deep-interior, golden policy #35/#36 — quad edges are never
// sampled):
//   * every (eye, instance) quad center lands at the CPU-referenced pixel
//     computed from the same rig matrices that fill the UBO;
//   * the same instance shifts >= 2 px horizontally between eyes (real
//     parallax through the converged off-axis frustums) but 0 px vertically;
//   * row 1 inside each eye's rect stays exact clear color (no viewport
//     bleed across the eye seam);
//   * single-eye instanced capture == single-eye non-instanced capture,
//     byte-for-byte.
#include <doctest/doctest.h>
#include <ve/core/math/mat4.h>
#include <ve/gfx/camera.h>
#include <ve/gfx/descriptor.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>

#include "gfx_test_env.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ve/cook/cook.h>

namespace {
using namespace ve::core::math;
using namespace ve::gfx;

constexpr int kInst = 4;                    // instances per eye (fixed in shader)
constexpr float kQuadHalf = 0.10f;          // local-space half-size
// Exaggerated ipd (0.2 m) on purpose: this test pins the draw path, not the
// optics (the converged frustum math has realistic pins in 3.2). A big ipd
// makes the between-eye parallax >= 2 px so rounding can never hide it.
constexpr float kIpd = 0.2f, kFocal = 2.0f;

std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_eyes_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

// std140: mat4 mvp[2] then mat4 model[4] — 6 column-major 16-float blocks,
// matching mat4f's m[] layout exactly (no repack needed).
constexpr std::size_t kUboFloats = 6 * 16;

// Instance k -> model matrix: axis-aligned quad at the fixation plane z=-2.
// x offsets chosen so the quad columns have clear gaps inside a 64-px eye
// rect (quad half-width = kQuadHalf * 1/tan30 * 32 = 5.5 px, centers ~14 px
// apart after the eye-dependent frustum shifts are applied).
mat4f instance_model(int k) {
    const float xs[kInst] = {-0.375f, -0.125f, 0.125f, 0.375f};
    return translation(vec3f{xs[k], 0.0f, -2.0f});
}

// Instance color (RGB) authored in the fragment shader — no blending, so
// captures read back these bytes exactly (in B8G8R8A8 order).
std::array<std::uint8_t, 4> instance_bgra(int k) {
    switch (k) {
    case 0: return {0, 0, 255, 255};     // red
    case 1: return {0, 255, 0, 255};     // green
    case 2: return {255, 0, 0, 255};     // blue
    default: return {255, 255, 255, 255}; // white
    }
}
const std::array<std::uint8_t, 4> kClearBgra{255, 0, 255, 255}; // magenta

// The eye-pass shader pair. ARB name for the viewport-index write (the
// cooklet's glslangValidator accepts GL_ARB_shader_viewport_layer_array and
// emits the core-valid capability; see docs/phase-03 notes).
const char* kVertSrc = R"GLSL(#version 450
#extension GL_ARB_shader_viewport_layer_array : require
layout(set = 0, binding = 0, std140) uniform Eyes {
    mat4 mvp[2];    // per-eye proj*view, rig eye order = instance eye index
    mat4 model[4];  // per-instance
} eyes;
layout(location = 0) out flat uint inst_id;
void main() {
    const int n_inst = 4;
    int eye = gl_InstanceIndex / n_inst;
    int inst = gl_InstanceIndex % n_inst;
    gl_ViewportIndex = eye;  // instance rasterizes into ITS eye's viewport
    vec2 q[6] = vec2[6](
        vec2(-0.1, -0.1), vec2(0.1, -0.1), vec2(0.1, 0.1),
        vec2(-0.1, -0.1), vec2(0.1, 0.1), vec2(-0.1, 0.1));
    gl_Position = eyes.mvp[eye] * eyes.model[inst] * vec4(q[gl_VertexIndex], 0.0, 1.0);
    inst_id = uint(inst);
}
)GLSL";

const char* kFragSrc = R"GLSL(#version 450
layout(location = 0) in flat uint inst_id;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 cols[4] = vec3[4](vec3(1,0,0), vec3(0,1,0), vec3(0,0,1), vec3(1,1,1));
    outColor = vec4(cols[inst_id], 1.0);
}
)GLSL";

// Triangle-golden pipeline shape, parameterized by viewport/scissor count;
// depth and blend off, cull none (quads face the camera squarely).
VkPipeline build_pipeline(const device& dev, VkPipelineLayout layout,
                          VkShaderModule vert, VkShaderModule frag,
                          std::uint32_t vp_count) {
    const VkPipelineShaderStageCreateInfo stages[2]{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = nullptr,
         .flags = 0, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main",
         .pSpecializationInfo = nullptr},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = nullptr,
         .flags = 0, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main",
         .pSpecializationInfo = nullptr},
    };
    const VkPipelineVertexInputStateCreateInfo vi{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .vertexBindingDescriptionCount = 0, .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0, .pVertexAttributeDescriptions = nullptr,
    };
    const VkPipelineInputAssemblyStateCreateInfo ia{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };
    const VkPipelineViewportStateCreateInfo vp{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .viewportCount = vp_count, .pViewports = nullptr, // dynamic
        .scissorCount = vp_count, .pScissors = nullptr,               // dynamic
    };
    const VkPipelineRasterizationStateCreateInfo rs{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .depthClampEnable = VK_FALSE, .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f, .depthBiasClamp = 0.0f, .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE, .minSampleShading = 1.0f, .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE, .alphaToOneEnable = VK_FALSE,
    };
    const VkPipelineDepthStencilStateCreateInfo ds{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .depthTestEnable = VK_FALSE, .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS, .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE, .front = {}, .back = {},
        .minDepthBounds = 0.0f, .maxDepthBounds = 1.0f,
    };
    const VkPipelineColorBlendAttachmentState blend{
        .blendEnable = VK_FALSE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo cb{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .logicOpEnable = VK_FALSE, .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1, .pAttachments = &blend, .blendConstants = {0, 0, 0, 0},
    };
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dsy{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .dynamicStateCount = 2, .pDynamicStates = dyn,
    };
    const VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM; // presenter's format
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = nullptr,
        .viewMask = 0, .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat = VK_FORMAT_UNDEFINED, .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    const VkGraphicsPipelineCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering, .flags = 0,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pTessellationState = nullptr, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dsy, .layout = layout, .renderPass = VK_NULL_HANDLE, .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE, .basePipelineIndex = -1,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev.handle(), VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) !=
        VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pipe;
}

void begin_full_clear(offscreen_presenter& pres, VkCommandBuffer cmd,
                      std::uint32_t w, std::uint32_t h) {
    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = pres.color_view();
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color.float32[0] = 1.0f; // B8G8R8A8: B=1,R=1 -> magenta
    att.clearValue.color.float32[1] = 0.0f;
    att.clearValue.color.float32[2] = 1.0f;
    att.clearValue.color.float32[3] = 1.0f;
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
}

VkViewport to_vk(const viewport& v) {
    return VkViewport{.x = static_cast<float>(v.x), .y = static_cast<float>(v.y),
                      .width = static_cast<float>(v.width), .height = static_cast<float>(v.height),
                      .minDepth = 0.0f, .maxDepth = 1.0f};
}
VkRect2D to_vk_rect(const viewport& v) {
    return VkRect2D{{v.x, v.y}, {v.width, v.height}};
}

// CPU reference: where does instance k's quad CENTER land in RT pixels for
// eye e, given the same matrices that filled the UBO? Vulkan viewport
// mapping with the engine's y-down NDC convention (test_triangle_golden's
// "y-down: this points down on screen"): ndc +1 = bottom row.
struct Px { int x, y; };
Px expected_center(const mat4f& mvp, const viewport& vp, const mat4f& model) {
    const vec4f clip = mvp * vec4f::point(transform_point(model, vec3f{0, 0, 0}));
    const float ndc_x = clip.x / clip.w, ndc_y = clip.y / clip.w;
    return {vp.x + static_cast<int>(std::lround((ndc_x + 1.0f) * 0.5f * vp.width)),
            vp.y + static_cast<int>(std::lround((ndc_y + 1.0f) * 0.5f * vp.height))};
}
} // namespace

TEST_CASE("gfx_instanced_eyes") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 eye-pass test skipped");
        return;
    }

    // 1. Cooked shader pair.
    const auto src = tmp_root("src");
    write_file(std::filesystem::path(src) / "eyes.vert", kVertSrc);
    write_file(std::filesystem::path(src) / "eyes.frag", kFragSrc);
    const auto out = tmp_root("out");
    REQUIRE(ve::cook::cook_incremental(src, out).ok);
    ve::cook::Unit vu, fu;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out + "/units/eyes.vert.ck", vu, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/eyes.frag.ck", fu, &err));

    auto inst = instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = device::create(*inst);
    REQUIRE(dev.has_value());

    auto vert = shader_module::create(*dev, vu.payload);
    auto frag = shader_module::create(*dev, fu.payload);
    REQUIRE(vert.has_value());
    REQUIRE(frag.has_value());

    auto uset_layout = uniform_set_layout::create(*dev, 0, VK_SHADER_STAGE_VERTEX_BIT);
    REQUIRE(uset_layout.has_value());
    const VkDescriptorSetLayout set_layouts[1] = {uset_layout->handle()};
    const VkPipelineLayoutCreateInfo lci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pNext = nullptr, .flags = 0,
        .setLayoutCount = 1, .pSetLayouts = set_layouts,
        .pushConstantRangeCount = 0, .pPushConstantRanges = nullptr,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lci, nullptr, &layout) == VK_SUCCESS);

    auto pool = descriptor_pool::create(*dev, 0, 4);
    REQUIRE(pool.has_value());

    // ---------------------------------------------------------------------
    // A. Stereo pass: E=2 eyes x N=4 instances in ONE draw on a 128x64 RT.
    // ---------------------------------------------------------------------
    const auto stereo = converged_stereo_frustums(deg_to_rad(60.0f), 1.0f, 0.1f, 100.0f,
                                                  kIpd, kFocal);
    camera_rig rig;
    eye_def le{}, re{};
    le.rig_local.position = {-kIpd / 2.0f, 0.0f, 0.0f};
    le.frust = stereo.first;
    le.view = {0, 0, 64, 64};
    re.rig_local.position = {kIpd / 2.0f, 0.0f, 0.0f};
    re.frust = stereo.second;
    re.view = {64, 0, 64, 64};
    rig.add_eye(le); // eye order is authored order = instance eye index
    rig.add_eye(re);
    const auto frames = rig.compute();
    REQUIRE(frames.size() == 2);

    // UBO: mvp[e] = proj*view, model[k]; the CPU reference reuses these.
    mat4f mvp[2];
    for (int e = 0; e < 2; ++e) mvp[e] = frames[e].proj * frames[e].view;
    std::array<float, kUboFloats> ubo{};
    for (int e = 0; e < 2; ++e)
        for (int i = 0; i < 16; ++i) ubo[e * 16 + i] = mvp[e].m[i];
    for (int k = 0; k < kInst; ++k) {
        const mat4f mdl = instance_model(k);
        for (int i = 0; i < 16; ++i) ubo[(2 + k) * 16 + i] = mdl.m[i];
    }
    auto ub = uniform_buffer::create(*dev, sizeof(float) * kUboFloats);
    REQUIRE(ub.has_value());
    ub->write(ubo.data(), sizeof(float) * kUboFloats);

    auto dset = pool->allocate(uset_layout->handle());
    REQUIRE(dset.has_value());
    pool->write_uniform(*dset, 0, ub->handle(), ub->size());

    auto presA = offscreen_presenter::create(*dev, 128, 64);
    REQUIRE(presA.has_value());
    VkPipeline pipe2 = build_pipeline(*dev, layout, vert->handle(), frag->handle(), 2);
    REQUIRE(pipe2 != VK_NULL_HANDLE);

    {
        VkCommandBuffer cmd = presA->begin_command();
        REQUIRE(cmd != VK_NULL_HANDLE);
        REQUIRE(presA->begin_pass(cmd));
        begin_full_clear(*presA, cmd, 128, 64);
        const VkViewport vps[2] = {to_vk(frames[0].viewport_), to_vk(frames[1].viewport_)};
        const VkRect2D scis[2] = {to_vk_rect(frames[0].viewport_), to_vk_rect(frames[1].viewport_)};
        vkCmdSetViewport(cmd, 0, 2, vps);
        vkCmdSetScissor(cmd, 0, 2, scis);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe2);
        const VkDescriptorSet set_a = *dset;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set_a, 0,
                                nullptr);
        vkCmdDraw(cmd, 6, kInst * 2, 0, 0); // THE single pass: N x E in one draw
        vkCmdEndRendering(cmd);
        REQUIRE(presA->end_pass(cmd));
        REQUIRE(presA->submit(cmd));
    }
    const auto px = presA->capture_rgba8();
    REQUIRE(px.size() == 128u * 64u * 4u);
    const auto bgra = [&](int x, int y) {
        const std::size_t o = (static_cast<std::size_t>(y) * 128u + static_cast<std::size_t>(x)) * 4u;
        return std::array<std::uint8_t, 4>{px[o], px[o + 1], px[o + 2], px[o + 3]};
    };

    // Pin 1: every (eye, instance) center at the CPU-referenced pixel.
    std::int64_t centers[2][kInst][2];
    for (int e = 0; e < 2; ++e) {
        for (int k = 0; k < kInst; ++k) {
            const Px p = expected_center(mvp[e], frames[e].viewport_, instance_model(k));
            centers[e][k][0] = p.x;
            centers[e][k][1] = p.y;
            CHECK(p.x >= static_cast<int>(frames[e].viewport_.x));
            CHECK(p.x < static_cast<int>(frames[e].viewport_.x + frames[e].viewport_.width));
            const auto got = bgra(p.x, p.y);
            const auto want = instance_bgra(k);
            CHECK_MESSAGE(got == want, "eye " << e << " inst " << k << " at (" << p.x << "," << p.y
                                              << "): got " << int(got[0]) << "," << int(got[1]) << ","
                                              << int(got[2]) << " want " << int(want[0]) << ","
                                              << int(want[1]) << "," << int(want[2]));
        }
    }
    // Pin 2: real parallax between eyes — >= 2 px horizontal shift, 0 vertical.
    for (int k = 0; k < kInst; ++k) {
        const int dx = std::abs(static_cast<int>(centers[0][k][0] - centers[1][k][0]));
        CHECK_MESSAGE(dx >= 2, "inst " << k << " eyes at x " << centers[0][k][0] << " / "
                                       << centers[1][k][0] << " — no parallax through the "
                                       << "off-axis frustums?");
        CHECK(centers[0][k][1] == centers[1][k][1]); // no vertical disparity
    }
    // Pin 3: no viewport bleed — row 1 (far above every quad: quad columns
    // span rows 29..35) stays exact clear color inside both eye rects, and
    // the computed gap column between quad columns 1 and 2 (>= 2 px clear
    // between edges, centers ~14 px apart, half-width 5.5 px) as well.
    for (int e = 0; e < 2; ++e) {
        const auto& v = frames[e].viewport_;
        CHECK(bgra(v.x + 1, v.y + 1) == kClearBgra);
        CHECK(bgra(v.x + 62, v.y + 1) == kClearBgra);
        const int gap = static_cast<int>((centers[e][1][0] + centers[e][2][0]) / 2);
        CHECK_MESSAGE(bgra(gap, static_cast<int>(centers[e][0][1])) == kClearBgra,
                      "eye " << e << " gap column " << gap << " bled");
    }

    // ---------------------------------------------------------------------
    // B. Desktop path: same shaders/layout/UBO pattern, ONE centered eye,
    //    instanced vs non-instanced byte-equality on a 64x64 RT.
    // ---------------------------------------------------------------------
    camera_rig desktop;
    eye_def de{};
    de.frust = symmetric_frustum(deg_to_rad(60.0f), 1.0f, 0.1f, 100.0f);
    de.view = {0, 0, 64, 64};
    desktop.add_eye(de);
    const auto df = desktop.compute();
    REQUIRE(df.size() == 1);
    mat4f dmvp = df[0].proj * df[0].view;
    std::array<float, kUboFloats> ubo_d{};
    for (int i = 0; i < 16; ++i) { ubo_d[i] = dmvp.m[i]; ubo_d[16 + i] = dmvp.m[i]; }
    for (int k = 0; k < kInst; ++k) {
        const mat4f mdl = instance_model(k);
        for (int i = 0; i < 16; ++i) ubo_d[(2 + k) * 16 + i] = mdl.m[i];
    }
    auto ub_d = uniform_buffer::create(*dev, sizeof(float) * kUboFloats);
    REQUIRE(ub_d.has_value());
    ub_d->write(ubo_d.data(), sizeof(float) * kUboFloats);
    auto dset_d = pool->allocate(uset_layout->handle());
    REQUIRE(dset_d.has_value());
    pool->write_uniform(*dset_d, 0, ub_d->handle(), ub_d->size());

    // vp_count=1 variant: the same gl_ViewportIndex write stays legal with a
    // single viewport (eye index is always 0) — desktop through this path.
    VkPipeline pipe1 = build_pipeline(*dev, layout, vert->handle(), frag->handle(), 1);
    REQUIRE(pipe1 != VK_NULL_HANDLE);

    auto presB = offscreen_presenter::create(*dev, 64, 64);
    REQUIRE(presB.has_value());

    std::vector<std::uint8_t> cap_inst, cap_seq;
    {
        VkCommandBuffer cmd = presB->begin_command();
        REQUIRE(cmd != VK_NULL_HANDLE);
        REQUIRE(presB->begin_pass(cmd));
        begin_full_clear(*presB, cmd, 64, 64);
        const VkViewport vpt = to_vk(df[0].viewport_);
        const VkRect2D sc = to_vk_rect(df[0].viewport_);
        vkCmdSetViewport(cmd, 0, 1, &vpt);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe1);
        const VkDescriptorSet set_b = *dset_d;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set_b, 0,
                                nullptr);
        vkCmdDraw(cmd, 6, kInst, 0, 0); // instanced: one draw
        vkCmdEndRendering(cmd);
        REQUIRE(presB->end_pass(cmd));
        REQUIRE(presB->submit(cmd));
        cap_inst = presB->capture_rgba8();

        cmd = presB->begin_command();
        REQUIRE(cmd != VK_NULL_HANDLE);
        REQUIRE(presB->begin_pass(cmd));
        begin_full_clear(*presB, cmd, 64, 64);
        vkCmdSetViewport(cmd, 0, 1, &vpt);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe1);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set_b, 0,
                                nullptr);
        // Non-instanced sequence: firstInstance supplies the instance id —
        // same shader math, no instancing hardware path.
        for (int k = 0; k < kInst; ++k) vkCmdDraw(cmd, 6, 1, 0, k);
        vkCmdEndRendering(cmd);
        REQUIRE(presB->end_pass(cmd));
        REQUIRE(presB->submit(cmd));
        cap_seq = presB->capture_rgba8();
    }
    REQUIRE(cap_inst.size() == 64u * 64u * 4u);
    CHECK_MESSAGE(cap_inst == cap_seq,
                  "single-eye instanced output != non-instanced sequence ("
                  << 64 * 64 * 4 << " bytes compared)");

    // Desktop centers are pinned too — the centered eye draws the same grid.
    const auto bgra_d = [&](int x, int y) {
        const std::size_t o = (static_cast<std::size_t>(y) * 64u + static_cast<std::size_t>(x)) * 4u;
        return std::array<std::uint8_t, 4>{cap_inst[o], cap_inst[o + 1], cap_inst[o + 2],
                                           cap_inst[o + 3]};
    };
    for (int k = 0; k < kInst; ++k) {
        const Px p = expected_center(dmvp, df[0].viewport_, instance_model(k));
        CHECK_MESSAGE(bgra_d(p.x, p.y) == instance_bgra(k),
                      "desktop inst " << k << " at (" << p.x << "," << p.y << ")");
    }

    vkDestroyPipeline(dev->handle(), pipe2, nullptr);
    vkDestroyPipeline(dev->handle(), pipe1, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);

    // Strict-validation gate (Tier-2 contract): the extension plumbing and
    // the two-viewport draw must produce ZERO validation problems.
    gfx_test::check_clean_validation(*inst);
}
