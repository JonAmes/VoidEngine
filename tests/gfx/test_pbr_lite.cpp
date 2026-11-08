// test: gfx_pbr_lite — checklist 3.10: hand-written Cook-Torrance GGX with
// one directional light. Shaders are GLSL cooked to SPIR-V like everything
// else (cooklet + glslangValidator). Light params ride a UBO, per-material
// params ride a UBO indexed by material (Q5), the per-draw model matrix and
// material index ride push constants (Q5). Two side-by-side quads = two
// materials through ONE pipeline in one pass.
//
// Pins (golden policy #35/#36: deep interior only, never quad edges):
//   * CPU reference mirrors the GLSL expression order in fp32 at each pin
//     pixel (probe-derived); |delta| <= 2/255 per channel — lavapipe may
//     contract mul+add into fma, so pins are tolerant but tight; the full
//     64x64 image is byte-exact against golden pbr_lite_64.png.
//   * the two materials land on visibly different colors (UBO indexing).
//   * a N·L == 0 fallback pin: the light-free half-space stays exactly
//     ambient*baseColor (lit term clamped to zero).
#include <doctest/doctest.h>
#include <ve/core/math/mat4.h>
#include <ve/gfx/camera.h>
#include <ve/gfx/descriptor.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>

#include "gfx_golden.h"
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

constexpr int kW = 64, kH = 64;

std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_pbr_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

// Scene constants (shared CPU<->GLSL; all authored fp32).
constexpr float kQuadHalf = 0.95f;
constexpr float kZ = -4.0f;
constexpr float kLightDir[3] = {0.5f, 0.5f, 1.0f}; // normalized in shader & ref
constexpr float kLightColor[4] = {1.f, 1.f, 1.f, 1.f}; // rgb * intensity(a)
constexpr float kAmbient[4] = {0.1f, 0.1f, 0.1f, 1.f};
constexpr float kBase[2][4] = {{1.f, 0.15f, 0.15f, 1.f}, {0.2f, 0.4f, 1.f, 1.f}};
constexpr float kMetal[2] = {0.2f, 0.f};
constexpr float kRough[2] = {0.5f, 0.8f};
constexpr std::array<std::uint8_t, 4> kClearBgra{255, 0, 255, 255}; // magenta

const char* kVertSrc = R"GLSL(#version 450
layout(set = 0, binding = 0, std140) uniform Cam {
    mat4 view; mat4 proj; vec4 camPos;
} cam;
layout(push_constant) uniform Draw {
    mat4 model; uint matIndex;
} draw;
layout(location = 0) out vec3 wpos;
layout(location = 1) out vec3 wnorm;
layout(location = 2) flat out uint matid;
void main() {
    vec2 q[6] = vec2[6](
        vec2(-0.95, -0.95), vec2(0.95, -0.95), vec2(0.95, 0.95),
        vec2(-0.95, -0.95), vec2(0.95, 0.95), vec2(-0.95, 0.95));
    vec4 wp = draw.model * vec4(q[gl_VertexIndex], 0.0, 1.0);
    wpos = wp.xyz;
    wnorm = mat3(draw.model) * vec3(0.0, 0.0, 1.0);
    matid = draw.matIndex;
    gl_Position = cam.proj * cam.view * wp;
}
)GLSL";

// Cook-Torrance: GGX NDF, Smith-Schlick visibility in the UE4
// approximation form (0.25/(ndl*ndv)), Schlick Fresnel. Diffuse Lambert
// scaled by (1-metallic). ambient*baseColor + emissive ride even when
// N·L clamps to 0. Constants identical to the CPU reference below.
const char* kFragSrc = R"GLSL(#version 450
layout(set = 0, binding = 0, std140) uniform Cam {
    mat4 view; mat4 proj; vec4 camPos;
} cam;
layout(set = 0, binding = 1, std140) uniform Mats {
    vec4 base[2]; vec4 emis[2]; vec4 mr[2]; // mr = (metallic, roughness, -, -)
} mats;
layout(set = 0, binding = 2, std140) uniform Lights {
    vec4 dir; vec4 color; vec4 ambient;
} lights;
layout(location = 0) in vec3 wpos;
layout(location = 1) in vec3 wnorm;
layout(location = 2) flat in uint matid;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 bcol = mats.base[matid].rgb;
    float metal = mats.mr[matid].x;
    float rough = max(mats.mr[matid].y, 0.04);
    vec3 emis = mats.emis[matid].rgb;

    vec3 N = normalize(wnorm);
    vec3 V = normalize(cam.camPos.xyz - wpos);
    vec3 L = normalize(lights.dir.xyz);
    float ndl = max(dot(N, L), 0.0);
    float ndv = max(dot(N, V), 0.0001);
    vec3 H = normalize(V + L);
    float ndh = max(dot(N, H), 0.0);
    float vdh = max(dot(V, H), 0.0);

    float a = rough * rough;
    float a2 = a * a;
    float h = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / max(3.14159265 * h * h, 1e-7);
    float vis = 0.25 / max(ndl * ndv, 0.0001);
    vec3 F0 = mix(vec3(0.04), bcol, metal);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - vdh, 5.0);
    vec3 spec = D * vis * F;
    vec3 diff = bcol * (1.0 - metal) / 3.14159265;

    vec3 li = lights.color.rgb * lights.color.a;
    vec3 amb = lights.ambient.rgb * lights.ambient.a;
    vec3 c = li * ndl * (diff + spec) + amb * bcol + emis;
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)GLSL";

// CPU reference — mirror of kFragSrc in the same fp32 operation order.
std::array<float, 3> pbr_ref(const vec3f& wp, int mat) {
    const vec3f N{0.f, 0.f, 1.f};
    const float lv = std::sqrt(kLightDir[0] * kLightDir[0] + kLightDir[1] * kLightDir[1] +
                               kLightDir[2] * kLightDir[2]);
    const vec3f L{kLightDir[0] / lv, kLightDir[1] / lv, kLightDir[2] / lv};
    const vec3f d = vec3f{0.f, 0.f, 0.f} - wp; // camPos = origin
    const float dv = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const vec3f V{d.x / dv, d.y / dv, d.z / dv};

    const float ndl = std::max(N.x * L.x + N.y * L.y + N.z * L.z, 0.0f);
    const float ndv = std::max(N.x * V.x + N.y * V.y + N.z * V.z, 0.0001f);
    const vec3f hs{V.x + L.x, V.y + L.y, V.z + L.z};
    const float hv = std::sqrt(hs.x * hs.x + hs.y * hs.y + hs.z * hs.z);
    const vec3f H{hs.x / hv, hs.y / hv, hs.z / hv};
    const float ndh = std::max(N.x * H.x + N.y * H.y + N.z * H.z, 0.0f);
    const float vdh = std::max(V.x * H.x + V.y * H.y + V.z * H.z, 0.0f);

    const float a = std::max(kRough[mat], 0.04f) * std::max(kRough[mat], 0.04f);
    const float a2 = a * a;
    const float hh = ndh * ndh * (a2 - 1.0f) + 1.0f;
    const float D = a2 / std::max(3.14159265f * hh * hh, 1e-7f);
    const float vis = 0.25f / std::max(ndl * ndv, 0.0001f);

    const float r = kRough[0];
    (void)r;
    std::array<float, 3> out{};
    for (int ch = 0; ch < 3; ++ch) {
        const float bcol = static_cast<float>(kBase[mat][ch]);
        const float metal = kMetal[mat];
        const float f0 = 0.04f + (bcol - 0.04f) * metal; // mix(0.04, bcol, metal)
        const float F = f0 + (1.0f - f0) * std::pow(1.0f - vdh, 5.0f);
        const float spec = D * vis * F;
        const float diff = bcol * (1.0f - metal) / 3.14159265f;
        const float c = 1.0f * ndl * (diff + spec) + kAmbient[ch] * kAmbient[3] * bcol;
        out[ch] = std::clamp(c, 0.0f, 1.0f);
    }
    return out;
}

// Triangle/eye-golden pipeline shape: 1 viewport (dynamic), depth off,
// cull none — quads face the camera squarely.
VkPipeline build_pipeline(const device& dev, VkPipelineLayout layout,
                          VkShaderModule vert, VkShaderModule frag) {
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
        .flags = 0, .viewportCount = 1, .pViewports = nullptr,
        .scissorCount = 1, .pScissors = nullptr,
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
    const VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
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
} // namespace

TEST_CASE("gfx_pbr_lite") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 PBR test skipped");
        return;
    }

    // 1. Cooked shader pair.
    const auto src = tmp_root("src");
    write_file(std::filesystem::path(src) / "pbr.vert", kVertSrc);
    write_file(std::filesystem::path(src) / "pbr.frag", kFragSrc);
    const auto out = tmp_root("out");
    REQUIRE(ve::cook::cook_incremental(src, out).ok);
    ve::cook::Unit vu, fu;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out + "/units/pbr.vert.ck", vu, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/pbr.frag.ck", fu, &err));

    auto inst = instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = device::create(*inst);
    REQUIRE(dev.has_value());
    auto vert = shader_module::create(*dev, vu.payload);
    auto frag = shader_module::create(*dev, fu.payload);
    REQUIRE(vert.has_value());
    REQUIRE(frag.has_value());

    // 2. One set, three UBO bindings (Cam | Mats | Lights) + push constants.
    const uniform_set_layout::ubo_binding binds[3]{
        {0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    auto set_layout = uniform_set_layout::create(*dev, std::span<const uniform_set_layout::ubo_binding>{binds, 3});
    REQUIRE(set_layout.has_value());
    const VkDescriptorSetLayout sets[1] = {set_layout->handle()};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 68};
    const VkPipelineLayoutCreateInfo lci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pNext = nullptr, .flags = 0,
        .setLayoutCount = 1, .pSetLayouts = sets,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lci, nullptr, &layout) == VK_SUCCESS);

    // Cam UBO: view (identity rig), proj (60 deg symmetric, aspect 1), camPos.
    camera_rig rig;
    eye_def eye{};
    eye.frust = symmetric_frustum(deg_to_rad(60.0f), 1.0f, 0.1f, 100.0f);
    eye.view = {0, 0, kW, kH};
    rig.add_eye(eye);
    const auto frames = rig.compute();
    REQUIRE(frames.size() == 1);

    std::vector<float> cam_ubo;
    for (int i = 0; i < 16; ++i) cam_ubo.push_back(frames[0].view.m[i]);
    for (int i = 0; i < 16; ++i) cam_ubo.push_back(frames[0].proj.m[i]);
    cam_ubo.insert(cam_ubo.end(), {0.f, 0.f, 0.f, 1.f}); // camPos (rig at origin)
    auto cam_buf = uniform_buffer::create(*dev, cam_ubo.size() * 4u);
    REQUIRE(cam_buf.has_value());
    cam_buf->write(cam_ubo.data(), cam_ubo.size() * 4u);

    // Mats UBO: base[2], emis[2], mr[2] — std140 float arrays.
    std::vector<float> mat_ubo;
    for (int m = 0; m < 2; ++m)
        for (int i = 0; i < 4; ++i) mat_ubo.push_back(kBase[m][i]);
    for (int m = 0; m < 2; ++m)
        mat_ubo.insert(mat_ubo.end(), {0.f, 0.f, 0.f, 0.f}); // emis
    for (int m = 0; m < 2; ++m)
        mat_ubo.insert(mat_ubo.end(), {kMetal[m], kRough[m], 0.f, 0.f});
    auto mat_buf = uniform_buffer::create(*dev, mat_ubo.size() * 4u);
    REQUIRE(mat_buf.has_value());
    mat_buf->write(mat_ubo.data(), mat_ubo.size() * 4u);

    std::vector<float> lit_ubo;
    for (int i = 0; i < 3; ++i) lit_ubo.push_back(kLightDir[i]);
    lit_ubo.push_back(0.f);
    for (int i = 0; i < 4; ++i) lit_ubo.push_back(kLightColor[i]);
    for (int i = 0; i < 4; ++i) lit_ubo.push_back(kAmbient[i]);
    auto lit_buf = uniform_buffer::create(*dev, lit_ubo.size() * 4u);
    REQUIRE(lit_buf.has_value());
    lit_buf->write(lit_ubo.data(), lit_ubo.size() * 4u);

    auto pool = descriptor_pool::create(*dev, 0, 4);
    REQUIRE(pool.has_value());
    auto dset = pool->allocate(set_layout->handle());
    REQUIRE(dset.has_value());
    pool->write_uniform(*dset, 0, cam_buf->handle(), cam_buf->size());
    pool->write_uniform(*dset, 1, mat_buf->handle(), mat_buf->size());
    pool->write_uniform(*dset, 2, lit_buf->handle(), lit_buf->size());

    auto pres = offscreen_presenter::create(*dev, kW, kH);
    REQUIRE(pres.has_value());
    auto pipe = build_pipeline(*dev, layout, vert->handle(), frag->handle());
    REQUIRE(pipe != VK_NULL_HANDLE);

    struct Push {
        alignas(16) float model[16];
        std::uint32_t mat_index;
    };
    Push p0{}, p1{};
    const mat4f m0 = translation(vec3f{-kQuadHalf, 0.f, kZ});
    const mat4f m1 = translation(vec3f{kQuadHalf, 0.f, kZ});
    for (int i = 0; i < 16; ++i) { p0.model[i] = m0.m[i]; p1.model[i] = m1.m[i]; }
    p0.mat_index = 0;
    p1.mat_index = 1;

    {
        VkCommandBuffer cmd = pres->begin_command();
        REQUIRE(cmd != VK_NULL_HANDLE);
        REQUIRE(pres->begin_pass(cmd));
        VkRenderingAttachmentInfo att{};
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = pres->color_view();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color.float32[0] = 1.0f; // magenta clear (B8G8R8A8)
        att.clearValue.color.float32[1] = 0.0f;
        att.clearValue.color.float32[2] = 1.0f;
        att.clearValue.color.float32[3] = 1.0f;
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {static_cast<std::uint32_t>(kW), static_cast<std::uint32_t>(kH)}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vpf{0.f, 0.f, static_cast<float>(kW), static_cast<float>(kH), 0.f, 1.f};
        const VkRect2D sc{{0, 0}, {static_cast<std::uint32_t>(kW), static_cast<std::uint32_t>(kH)}};
        vkCmdSetViewport(cmd, 0, 1, &vpf);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        const VkDescriptorSet ds = *dset;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ds, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 68, &p0);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 68, &p1);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdEndRendering(cmd);
        REQUIRE(pres->end_pass(cmd));
        REQUIRE(pres->submit(cmd));
    }

    const auto px = pres->capture_rgba8();
    REQUIRE(px.size() == static_cast<std::size_t>(kW) * kH * 4u);
    const auto bgra = [&](int x, int y) {
        const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4u;
        return std::array<std::uint8_t, 4>{px[o], px[o + 1], px[o + 2], px[o + 3]};
    };

    // Where the quad centers project (same math the UBO drives).
    const mat4f mvp = frames[0].proj * frames[0].view;
    const auto center_px = [&](const mat4f& model) {
        const vec4f clip = mvp * vec4f::point(transform_point(model, vec3f{0, 0, 0}));
        const float nx = clip.x / clip.w, ny = clip.y / clip.w;
        return std::pair{static_cast<int>(std::lround((nx + 1.f) * 0.5f * kW)),
                         static_cast<int>(std::lround((ny + 1.f) * 0.5f * kH))};
    };
    const auto [lx, ly] = center_px(m0);
    const auto [rx, ry] = center_px(m1);
    MESSAGE("quad centers project to (", lx, ",", ly, ") and (", rx, ",", ry, ")");

    // Pin A: CPU-referenced GGX color at the left quad center (mat 0).
    const auto ref = pbr_ref(transform_point(m0, vec3f{0, 0, 0}), 0);
    const auto got = bgra(lx, ly);
    MESSAGE("ref BGR(", std::lround(ref[2] * 255.f), ",", std::lround(ref[1] * 255.f), ",",
            std::lround(ref[0] * 255.f), ") got BGR(", got[0], ",", got[1], ",", got[2], ")");
    for (int ch = 0; ch < 3; ++ch) {
        const float expect = ref[2 - ch] * 255.f;
        CHECK_MESSAGE(std::abs(static_cast<float>(got[ch]) - expect) <= 2.0f,
                      "ch" << ch << " got " << static_cast<int>(got[ch]) << " expect " << expect);
    }

    // Pin B: material UBO indexing — right quad center gets material 1's
    // GGX result (blue base, rough 0.8): CPU-referenced, and distinct from
    // what material 0 outputs at its own center.
    const auto ref1 = pbr_ref(transform_point(m1, vec3f{0, 0, 0}), 1);
    const auto gotr = bgra(rx, ry);
    MESSAGE("ref1 BGR(", std::lround(ref1[2] * 255.f), ",", std::lround(ref1[1] * 255.f), ",",
            std::lround(ref1[0] * 255.f), ") got BGR(", gotr[0], ",", gotr[1], ",", gotr[2], ")");
    for (int ch = 0; ch < 3; ++ch) {
        const float expect = ref1[2 - ch] * 255.f;
        CHECK_MESSAGE(std::abs(static_cast<float>(gotr[ch]) - expect) <= 2.0f,
                      "mat1 ch" << ch << " got " << static_cast<int>(gotr[ch]) << " expect " << expect);
    }
    CHECK(gotr != bgra(lx, ly)); // material 1 != material 0 output

    // Pin C: background stays the exact clear color (no bleed, deep corner).
    CHECK(bgra(1, 1) == kClearBgra);

    // Golden: strict byte-exact on lavapipe (decision #35). The capture is
    // image-order (B8G8R8A8) — the golden PNG harness wants RGBA (same
    // swap as test_triangle_golden).
    std::vector<std::uint8_t> rgba(px.size());
    for (std::size_t i = 0; i < px.size(); i += 4u) {
        rgba[i + 0] = px[i + 2];
        rgba[i + 1] = px[i + 1];
        rgba[i + 2] = px[i + 0];
        rgba[i + 3] = px[i + 3];
    }
    const auto gv = gfx_golden::check_golden("pbr_lite_64.png", rgba.data(), kW, kH, {});
    CHECK_MESSAGE(gv.ok, gv.message);

    vkDestroyPipeline(dev->handle(), pipe, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);
    gfx_test::check_clean_validation(*inst);
}
