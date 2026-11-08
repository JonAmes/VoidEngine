// test: gfx_triangle_golden — the Tier-2 golden core (checklist 2.6):
// the offscreen presenter's own RT is written by a real render pass
// (VK1.3 dynamic rendering — no VkRenderPass objects, API floor is 1.3 per
// decision #37) with a cooked vertex+fragment shader pair and a
// push-constant MVP. Pins are cross-driver safe: deep-interior pixel exact,
// corner = exact clear color; edge pixels are NOT pinned (rasterization
// rules differ at the half-pixel boundary, decision #35/#36 golden policy).
#include <doctest/doctest.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>

#include "gfx_golden.h" // 2.11: rerun the golden through the committed-image harness
#include "gfx_test_env.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ve/cook/cook.h>

namespace {
std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_tri_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

// MVP identity lives in a push constant so the same pipeline serves every
// later pass (the MVP pattern decision #34 established in Phase 1).
struct PushMvp {
    float m[16];
};

VkPipeline build_pipeline(const ve::gfx::device& dev, VkPipelineLayout layout,
                          VkShaderModule vert, VkShaderModule frag) {
    const VkPipelineShaderStageCreateInfo stages[2]{
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
    };
    // No vertex input: the triangle comes from gl_VertexIndex (baked in the
    // cooked shader). Fewer objects, fewer failure modes, same output.
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
        .depthTestEnable = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
        .front = {}, // stencil disabled: zero-init, don't fabricate ops
        .back = {},
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 1.0f,
    };
    const VkPipelineColorBlendAttachmentState blend{
        // declaration order: blendEnable first, factors, ops, then writeMask
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
    // Dynamic rendering still needs a render pass *description* for the
    // pipeline layout contract (format only; no VkRenderPass object).
    const VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format, // (VkFormat){...} is C, not C++
        .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    const VkGraphicsPipelineCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .flags = 0,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pTessellationState = nullptr,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .pDynamicState = &dsy,
        .layout = layout,
        .renderPass = VK_NULL_HANDLE, // dynamic rendering (VK1.3)
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev.handle(), VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pipe;
}
} // namespace

TEST_CASE("gfx_triangle_golden") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 golden skipped");
        return;
    }

    // 1. Cooked shader pair (the only way shaders reach runtime).
    const auto src = tmp_root("src");
    write_file(std::filesystem::path(src) / "tri.vert", R"GLSL(#version 450
layout(push_constant) uniform Push { mat4 mvp; } push;
layout(location = 0) out vec3 col;
void main() {
    // One green triangle covering the center of [-1,1] NDC (Vulkan y-down:
    // this points down on screen; the golden doesn't care, position does).
    vec2 p[3] = vec2[3](vec2(-0.5, 0.4), vec2(0.5, 0.4), vec2(0.0, -0.4));
    gl_Position = push.mvp * vec4(p[gl_VertexIndex], 0.0, 1.0);
    col = vec3(0.0, 1.0, 0.0);
}
)GLSL");
    write_file(std::filesystem::path(src) / "tri.frag", R"GLSL(#version 450
layout(location = 0) in vec3 col;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(col, 1.0); }
)GLSL");
    const auto out = tmp_root("out");
    REQUIRE(ve::cook::cook_incremental(src, out).ok);
    ve::cook::Unit vu, fu;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out + "/units/tri.vert.ck", vu, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/tri.frag.ck", fu, &err));

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());
    auto pres = ve::gfx::offscreen_presenter::create(*dev, 64, 64);
    REQUIRE(pres.has_value());

    auto vert = ve::gfx::shader_module::create(*dev, vu.payload);
    auto frag = ve::gfx::shader_module::create(*dev, fu.payload);
    REQUIRE(vert.has_value());
    REQUIRE(frag.has_value());

    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(PushMvp),
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

    VkPipeline pipe = build_pipeline(*dev, layout, vert->handle(), frag->handle());
    REQUIRE(pipe != VK_NULL_HANDLE);

    // 2. Frame: the presenter owns the target and its layout transitions —
    // the pass only ever sees color_view() (the seam from 2.4, VK1.3 dynamic
    // rendering per decision #37: no VkRenderPass objects anywhere).
    VkCommandBuffer cmd = pres->begin_command();
    REQUIRE(cmd != VK_NULL_HANDLE);
    REQUIRE(pres->begin_pass(cmd));

    // Value-init + explicit assignment: the struct gains fields across SDK
    // header versions (resolveMode in 1.4); designated initializers would
    // pin this test to one header revision (#38: Windows SDK headers differ).
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
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {64, 64}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);

    const VkViewport vpt{
        .x = 0.0f, .y = 0.0f, .width = 64.0f, .height = 64.0f, .minDepth = 0.0f, .maxDepth = 1.0f};
    const VkRect2D sc{.offset = {0, 0}, .extent = {64, 64}};
    vkCmdSetViewport(cmd, 0, 1, &vpt);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

    PushMvp mvp{}; // identity, column-major (Phase 1 mat4 layout)
    mvp.m[0] = mvp.m[5] = mvp.m[10] = mvp.m[15] = 1.0f;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), &mvp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    REQUIRE(pres->end_pass(cmd));
    REQUIRE(pres->submit(cmd));

    // 3. Read back through the presenter (2.4) — it bookkeeps that a pass
    // changed the layout and barriers TRANSFER_SRC itself.
    const auto px = pres->capture_rgba8();
    REQUIRE(px.size() == 64u * 64u * 4u);

    const auto bgra = [&](int x, int y) {
        const std::size_t o = (static_cast<std::size_t>(y) * 64u + static_cast<std::size_t>(x)) * 4u;
        return std::array<std::uint8_t, 4>{px[o], px[o + 1], px[o + 2], px[o + 3]};
    };
    const auto is_green = [](std::array<std::uint8_t, 4> c) {
        return c[0] == 0u && c[1] == 255u && c[2] == 0u && c[3] == 255u;
    };
    const auto is_clear_blue = [](std::array<std::uint8_t, 4> c) {
        return c[0] == 255u && c[1] == 0u && c[2] == 0u && c[3] == 255u;
    };

    // Interior pins: triangle spans y NDC [-0.4..0.4], x [-0.5..0.5] (widest
    // at |y|=0.4). Center (32,32) and (30,28) are deep inside: exact green.
    CHECK(is_green(bgra(32, 32)));
    CHECK(is_green(bgra(30, 28)));
    CHECK(is_green(bgra(34, 36)));

    // Exterior pins: corners are deep outside the scissor... inside it, deep
    // outside the triangle: exact clear color.
    CHECK(is_clear_blue(bgra(0, 0)));
    CHECK(is_clear_blue(bgra(63, 63)));
    CHECK(is_clear_blue(bgra(0, 63)));
    CHECK(is_clear_blue(bgra(63, 0)));
    CHECK(is_clear_blue(bgra(2, 32))); // left of triangle, mid-height

    // Count sanity: exact area = (1/2 * base 1.0 * height 0.8) / 4 (NDC area)
    // * 4096 px = 409.6 -> ~410 interior + rasterized edges ~420. Guards
    // against a pipeline that silently drew nothing (or everything).
    int green_count = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            if (is_green(bgra(x, y))) {
                ++green_count;
            }
        }
    }
    CHECK(green_count > 380);
    CHECK(green_count < 460);

    // 2.11 rerun through the golden-PNG harness: capture -> committed image.
    // The readback is raw B8G8R8A8; goldens are stored as plain RGBA8 so they
    // open in any image viewer without surprises (swap R/B once, here).
    // Policy stays STRICT: this test only runs under forced lavapipe (the
    // deterministic gate, #35), where byte-exact is the contract; the harness
    // tolerances exist for real-GPU goldens on other tiers.
    // Generate/update the image with VE_GOLDEN_UPDATE=1 (docs/CHEATS.md).
    std::vector<std::uint8_t> rgba(px.size());
    for (std::size_t i = 0; i < px.size(); i += 4u) {
        rgba[i + 0] = px[i + 2];
        rgba[i + 1] = px[i + 1];
        rgba[i + 2] = px[i + 0];
        rgba[i + 3] = px[i + 3];
    }
    const auto gv = gfx_golden::check_golden("triangle_64.png", rgba.data(), 64, 64, {});
    CHECK_MESSAGE(gv.ok, gv.message);

    vkDestroyPipeline(dev->handle(), pipe, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);

    // The Tier-2 golden gate doubles as a strict-validation gate: a triangle
    // that "works" with a pile of validation problems is not green.
    gfx_test::check_clean_validation(*inst);
}
