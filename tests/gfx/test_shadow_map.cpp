// test: gfx_shadow_map — checklist 3.11: directional-light depth pass into
// its own D32 target (light rig from scene bounds via
// dir_light_shadow_rig), hard-sample in the PBR pass (reversed-Z compare +
// small constant bias). Pass A renders the cube into the 256x256 shadow
// presenter (depth-only, 0 color attachments); capture_depth_f32 pins the
// texels; expose_depth_for_sampling hands it to pass B as a sampler; pass B
// is the PBR-lite pass (floor + cube) with the shadow term.
//
// Pins:
//   * shadow texel at cube-top center ~= CPU lightVP depth (2e-3: texel-center
//     offset on an oblique face), flat
//     neighborhood, cleared corner texel == 0 exactly.
//   * coverage count == projected cube area in texels (+-5%) — pins the
//     fitted rig actually bounds the scene box.
//   * floor pins deep inside/outside the hand-verified shadow parallelogram:
//     color == CPU GGX with vis from the READ BACK texel (exact reference).
//   * cube-top pin lit; background corner == clear. Golden scene_64.png.
#include <doctest/doctest.h>
#include <ve/core/math/mat4.h>
#include <ve/core/math/projection.h>
#include <ve/gfx/camera.h>
#include <ve/gfx/descriptor.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>

#include "gfx_golden.h"
#include "gfx_test_env.h"

#include <algorithm>
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

constexpr int kW = 64, kH = 64, kS = 256; // scene px, shadow map texels
constexpr float kBias = 0.002f; // reversed-Z shadow bias (world ~0.011 units)

std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_sh_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

// ---- scene (world coords, RH y-up, camera looks -Z) --------------------
// floor: y = -1, x,z in [-3,3] around the cube. cube: x,z face +-0.5,
// y in [-1,0], z in [-4.5,-3.5]. camera eye (0,1.5,0), identity rotation.
constexpr float kCamY = 1.5f;
constexpr float kLightDir[3] = {-0.35f, -1.f, 0.35f}; // TRAVEL dir
constexpr float kLightColor[4] = {1.f, 0.95f, 0.85f, 1.4f}; // rgb*intensity(a)
constexpr float kAmbient[4] = {0.12f, 0.12f, 0.14f, 1.f};
constexpr float kBase[2][4] = {{0.3f, 0.45f, 0.3f, 1.f}, {0.75f, 0.75f, 0.8f, 1.f}};
constexpr float kMetal[2] = {0.1f, 0.f};
constexpr float kRough[2] = {0.6f, 0.35f};
constexpr std::array<std::uint8_t, 4> kClearBgra{255, 0, 255, 255}; // magenta

// Build the 36-vertex cube table (12 tris; positions + face normals).
std::vector<float> cube_table() {
    struct Face { vec3f n; std::array<vec3f, 4> c; };
    // Each face wound consistently (cull NONE anyway; depth is all that matters).
    const float x0 = -0.5f, x1 = 0.5f, y0 = -1.f, y1 = 0.f, z0 = -4.5f, z1 = -3.5f;
    const std::array<Face, 6> faces{{
        {vec3f{0, 1, 0}, {vec3f{x0, y1, z0}, vec3f{x0, y1, z1}, vec3f{x1, y1, z1}, vec3f{x1, y1, z0}}},   // top
        {vec3f{0, -1, 0}, {vec3f{x0, y0, z0}, vec3f{x1, y0, z0}, vec3f{x1, y0, z1}, vec3f{x0, y0, z1}}},  // bottom
        {vec3f{0, 0, -1}, {vec3f{x0, y0, z0}, vec3f{x0, y1, z0}, vec3f{x1, y1, z0}, vec3f{x1, y0, z0}}},  // -z
        {vec3f{0, 0, 1}, {vec3f{x0, y0, z1}, vec3f{x1, y0, z1}, vec3f{x1, y1, z1}, vec3f{x0, y1, z1}}},   // +z
        {vec3f{-1, 0, 0}, {vec3f{x0, y0, z0}, vec3f{x0, y0, z1}, vec3f{x0, y1, z1}, vec3f{x0, y1, z0}}},  // -x
        {vec3f{1, 0, 0}, {vec3f{x1, y0, z0}, vec3f{x1, y1, z0}, vec3f{x1, y1, z1}, vec3f{x1, y0, z1}}},   // +x
    }};
    std::vector<float> t;
    for (const auto& f : faces)
        for (int tri : {0, 1, 2, 0, 2, 3}) {
            const vec3f& p = f.c[tri];
            t.insert(t.end(), {p.x, p.y, p.z, 1.f}); // pos w=1: shadow.vert uses .pos directly as gl_Position input
            t.insert(t.end(), {f.n.x, f.n.y, f.n.z, 0.f});
        }
    return t; // 36 * 8 floats
}

// ---- shaders -------------------------------------------------------------
// Pass A: pull cube positions, project with lightVP from the Lights UBO.
const char* kShadowVert = R"GLSL(#version 450
struct CubeV { vec4 pos; vec4 nrm; };
layout(set = 0, binding = 2, std140) uniform Lights {
    vec4 dir; vec4 color; vec4 ambient; mat4 lightVP;
} lights;
layout(set = 0, binding = 3, std140) uniform CubeTable { CubeV v[36]; } cube;
void main() { gl_Position = lights.lightVP * cube.v[gl_VertexIndex].pos; }
)GLSL";

const char* kShadowFrag = R"GLSL(#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.0); } // color attachment is DONT_CARE'd;
// lavapipe drops draws whose pass writes no color output, so the shadow
// pass carries a sacrificial color attachment the result never reads.
)GLSL";

const char* kSceneVert = R"GLSL(#version 450
struct CubeV { vec4 pos; vec4 nrm; };
layout(set = 0, binding = 0, std140) uniform Cam {
    mat4 view; mat4 proj; vec4 camPos;
} cam;
layout(set = 0, binding = 3, std140) uniform CubeTable { CubeV v[36]; } cube;
layout(push_constant) uniform Draw { uint matIndex; uint geom; } draw; // geom 0=floor 1=cube
layout(location = 0) out vec3 wpos;
layout(location = 1) out vec3 wnorm;
layout(location = 2) flat out uint matid;
void main() {
    vec3 p; vec3 n;
    if (draw.geom == 0u) {
        vec2 q[6] = vec2[6](
            vec2(-3.0, -7.0), vec2(-3.0, -1.0), vec2(3.0, -1.0),
            vec2(-3.0, -7.0), vec2(3.0, -1.0), vec2(3.0, -7.0)); // x,z at y=-1
        p = vec3(q[gl_VertexIndex].x, -1.0, q[gl_VertexIndex].y);
        n = vec3(0.0, 1.0, 0.0);
    } else {
        p = cube.v[gl_VertexIndex].pos.xyz;
        n = cube.v[gl_VertexIndex].nrm.xyz;
    }
    wpos = p; wnorm = n; matid = draw.matIndex;
    gl_Position = cam.proj * cam.view * vec4(p, 1.0);
}
)GLSL";

// PBR-lite (identical math order to 3.10) + hard shadow sample. vis is 0
// inside shadow, 1 outside; ambient*baseColor survives being shadowed.
const char* kSceneFrag = R"GLSL(#version 450
layout(set = 0, binding = 0, std140) uniform Cam {
    mat4 view; mat4 proj; vec4 camPos;
} cam;
layout(set = 0, binding = 1, std140) uniform Mats {
    vec4 base[2]; vec4 emis[2]; vec4 mr[2];
} mats;
layout(set = 0, binding = 2, std140) uniform Lights {
    vec4 dir; vec4 color; vec4 ambient; mat4 lightVP;
} lights;
layout(set = 1, binding = 0) uniform sampler2D shadowMap; // D32, texelFetch
layout(location = 0) in vec3 wpos;
layout(location = 1) in vec3 wnorm;
layout(location = 2) flat in uint matid;
layout(location = 0) out vec4 outColor;
void main() {
    // shadow: reversed-Z compare with constant bias (scene has no cube self
    // -pins at risk; bias only kills fp eps speckle on first-hit faces).
    vec4 lc = lights.lightVP * vec4(wpos, 1.0);
    ivec2 st = clamp(ivec2((lc.xy * 0.5 + 0.5) * 256.0), ivec2(0), ivec2(255));
    float stored = texelFetch(shadowMap, st, 0).x;
    float vis = (lc.z + 0.002 < stored) ? 0.0 : 1.0;

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
    float vis_g = 0.25 / max(ndl * ndv, 0.0001);
    vec3 F0 = mix(vec3(0.04), bcol, metal);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - vdh, 5.0);
    vec3 spec = D * vis_g * F;
    vec3 diff = bcol * (1.0 - metal) / 3.14159265;

    vec3 li = lights.color.rgb * lights.color.a;
    vec3 amb = lights.ambient.rgb * lights.ambient.a;
    vec3 c = li * ndl * vis * (diff + spec) + amb * bcol + emis;
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)GLSL";

// ---- CPU mirror helpers --------------------------------------------------
vec3f light_travel() {
    const vec3f d{kLightDir[0], kLightDir[1], kLightDir[2]};
    return normalize(d);
}

// GGX color at world point p with normal N for material m; vis from caller.
std::array<float, 3> pbr_ref(const vec3f& p, const vec3f& N, int mat, float vis) {
    const vec3f L = -light_travel(); // direction toward the light
    const vec3f Vv = normalize(vec3f{0.f - p.x, kCamY - p.y, 0.f - p.z}); // cam at (0,kCamY,0)
    const vec3f Ln = L;
    float ndl = std::max(dot(N, Ln), 0.0f);
    float ndv = std::max(dot(N, Vv), 0.0001f);
    const vec3f H = normalize(Vv + Ln);
    float ndh = std::max(dot(N, H), 0.0f);
    float vdh = std::max(dot(Vv, H), 0.0f);
    const float r = std::max(kRough[mat], 0.04f);
    const float a = r * r, a2 = a * a;
    const float h = ndh * ndh * (a2 - 1.f) + 1.f;
    const float D = a2 / std::max(3.14159265f * h * h, 1e-7f);
    const float vg = 0.25f / std::max(ndl * ndv, 0.0001f);
    std::array<float, 3> out{};
    for (int ch = 0; ch < 3; ++ch) {
        const float b = kBase[mat][ch], m = kMetal[mat];
        const float f0 = 0.04f + (b - 0.04f) * m;
        const float F = f0 + (1.f - f0) * std::pow(1.f - vdh, 5.f);
        const float c = kLightColor[ch] * kLightColor[3] * ndl * vis *
                            (b * (1.f - m) / 3.14159265f + D * vg * F) +
                        kAmbient[ch] * kAmbient[3] * b;
        out[ch] = std::clamp(c, 0.f, 1.f);
    }
    return out;
}

VkPipeline build_pipe(const device& dev, VkPipelineLayout layout,
                      VkShaderModule vert, VkShaderModule frag,
                      std::uint32_t color_count, const VkFormat* color_formats,
                      VkFormat depth_format, bool depth_test_write) {
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
        .flags = 0, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .primitiveRestartEnable = VK_FALSE,
    };
    const VkPipelineViewportStateCreateInfo vp{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .viewportCount = 1, .pViewports = nullptr, .scissorCount = 1, .pScissors = nullptr,
    };
    const VkPipelineRasterizationStateCreateInfo rs{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .depthClampEnable = VK_FALSE, .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.f, .depthBiasClamp = 0.f, .depthBiasSlopeFactor = 0.f,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 1.f, .pSampleMask = nullptr, .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    const VkPipelineDepthStencilStateCreateInfo ds{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = depth_test_write ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = depth_test_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_GREATER, // reversed-Z (3.3 convention)
        .depthBoundsTestEnable = VK_FALSE, .stencilTestEnable = VK_FALSE,
        .front = VkStencilOpState{}, .back = VkStencilOpState{},
        .minDepthBounds = 0.f, .maxDepthBounds = 1.f,
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
        .attachmentCount = color_count, .pAttachments = color_count ? &blend : nullptr,
        .blendConstants = {0.f, 0.f, 0.f, 0.f},
    };
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dsy{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .pNext = nullptr,
        .flags = 0, .dynamicStateCount = 2, .pDynamicStates = dyn,
    };
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = nullptr,
        .viewMask = 0, .colorAttachmentCount = color_count,
        .pColorAttachmentFormats = color_formats, .depthAttachmentFormat = depth_format,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    const VkGraphicsPipelineCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .flags = 0, .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pTessellationState = nullptr, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
        .pColorBlendState = &cb, .pDynamicState = &dsy, .layout = layout,
        .renderPass = VK_NULL_HANDLE, .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE, .basePipelineIndex = -1,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev.handle(), VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipe;
}
} // namespace

TEST_CASE("gfx_shadow_map") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 shadow test skipped");
        return;
    }

    // 1. cooked shaders (4 units).
    const auto src = tmp_root("src");
    write_file(std::filesystem::path(src) / "shadow.vert", kShadowVert);
    write_file(std::filesystem::path(src) / "shadow.frag", kShadowFrag);
    write_file(std::filesystem::path(src) / "scene.vert", kSceneVert);
    write_file(std::filesystem::path(src) / "scene.frag", kSceneFrag);
    const auto out = tmp_root("out");
    REQUIRE(ve::cook::cook_incremental(src, out).ok);

    auto inst = instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = device::create(*inst);
    REQUIRE(dev.has_value());
    auto cook_load = [&](const char* name) {
        ve::cook::Unit u;
        std::string err;
        REQUIRE(ve::cook::read_unit_file(out + "/units/" + name, u, &err));
        auto m = shader_module::create(*dev, u.payload);
        REQUIRE(m.has_value());
        return m;
    };
    auto sh_v = cook_load("shadow.vert.ck");
    auto sh_f = cook_load("shadow.frag.ck");
    auto sc_v = cook_load("scene.vert.ck");
    auto sc_f = cook_load("scene.frag.ck");

    // 2. scene + light rig (the fitted rig IS the checklist's "light rig
    // from scene bounds").
    const vec3f bmin{-3.f, -1.f, -7.f}, bmax{3.f, 0.f, -1.f};
    const auto [lview, lproj] = dir_light_shadow_rig(bmin, bmax, light_travel(),
                                                     vec3f{0, 1, 0}, 1.f, 0.5f,
                                                     depth_range::reversed_1_to_0);
    const mat4f lightVP = lproj * lview;

    // 3. UBOs.
    const mat4f view = translation(vec3f{0.f, -kCamY, 0.f}); // camera at (0,1.5,0)
    const mat4f proj = perspective(deg_to_rad(75.0f), 1.f, 0.1f, 100.f,
                                   depth_range::reversed_1_to_0);
    std::vector<float> cam_ubo;
    for (int i = 0; i < 16; ++i) cam_ubo.push_back(view.m[i]);
    for (int i = 0; i < 16; ++i) cam_ubo.push_back(proj.m[i]);
    cam_ubo.insert(cam_ubo.end(), {0.f, kCamY, 0.f, 1.f});
    auto cam_buf = uniform_buffer::create(*dev, cam_ubo.size() * 4u);
    REQUIRE(cam_buf.has_value());
    cam_buf->write(cam_ubo.data(), cam_ubo.size() * 4u);

    std::vector<float> mat_ubo;
    for (int m = 0; m < 2; ++m)
        for (int i = 0; i < 4; ++i) mat_ubo.push_back(kBase[m][i]);
    for (int m = 0; m < 2; ++m)
        mat_ubo.insert(mat_ubo.end(), {0.f, 0.f, 0.f, 0.f});
    for (int m = 0; m < 2; ++m)
        mat_ubo.insert(mat_ubo.end(), {kMetal[m], kRough[m], 0.f, 0.f});
    auto mat_buf = uniform_buffer::create(*dev, mat_ubo.size() * 4u);
    REQUIRE(mat_buf.has_value());
    mat_buf->write(mat_ubo.data(), mat_ubo.size() * 4u);

    const vec3f f = light_travel();
    std::vector<float> lit_ubo;
    // dir = toward the light (3.10 convention); then color, ambient, lightVP.
    lit_ubo.insert(lit_ubo.end(), {-f.x, -f.y, -f.z, 0.f});
    for (int i = 0; i < 4; ++i) lit_ubo.push_back(kLightColor[i]);
    for (int i = 0; i < 4; ++i) lit_ubo.push_back(kAmbient[i]);
    for (int i = 0; i < 16; ++i) lit_ubo.push_back(lightVP.m[i]);
    auto lit_buf = uniform_buffer::create(*dev, lit_ubo.size() * 4u);
    REQUIRE(lit_buf.has_value());
    lit_buf->write(lit_ubo.data(), lit_ubo.size() * 4u);

    const auto table = cube_table();
    REQUIRE(table.size() == 36u * 8u);
    auto cube_buf = uniform_buffer::create(*dev, table.size() * 4u);
    REQUIRE(cube_buf.has_value());
    cube_buf->write(table.data(), table.size() * 4u);

    // 4. descriptor plumbing: one UBO set (0 Cam, 1 Mats, 2 Lights, 3 Cube),
    // one sampler set (0 shadow map).
    const uniform_set_layout::ubo_binding binds[4]{
        {0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
        {3, VK_SHADER_STAGE_VERTEX_BIT},
    };
    auto set_layout = uniform_set_layout::create(
        *dev, std::span<const uniform_set_layout::ubo_binding>{binds, 4});
    REQUIRE(set_layout.has_value());
    auto sset_layout = sampler_set_layout::create(*dev, 0, VK_SHADER_STAGE_FRAGMENT_BIT);
    REQUIRE(sset_layout.has_value());

    const VkDescriptorSetLayout setsA[1] = {set_layout->handle()};
    VkPipelineLayoutCreateInfo lciA{};
    lciA.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lciA.setLayoutCount = 1;
    lciA.pSetLayouts = setsA;
    VkPipelineLayout layoutA = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lciA, nullptr, &layoutA) == VK_SUCCESS);
    const VkDescriptorSetLayout setsB[2] = {set_layout->handle(), sset_layout->handle()};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = 8;
    VkPipelineLayoutCreateInfo lciB{};
    lciB.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lciB.setLayoutCount = 2;
    lciB.pSetLayouts = setsB;
    lciB.pushConstantRangeCount = 1;
    lciB.pPushConstantRanges = &push;
    VkPipelineLayout layoutB = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lciB, nullptr, &layoutB) == VK_SUCCESS);

    auto pool = descriptor_pool::create(*dev, 8, 8); // 4 UBO bindings + margin
    REQUIRE(pool.has_value());
    auto dset = pool->allocate(set_layout->handle());
    REQUIRE(dset.has_value());
    pool->write_uniform(*dset, 0, cam_buf->handle(), cam_buf->size());
    pool->write_uniform(*dset, 1, mat_buf->handle(), mat_buf->size());
    pool->write_uniform(*dset, 2, lit_buf->handle(), lit_buf->size());
    pool->write_uniform(*dset, 3, cube_buf->handle(), cube_buf->size());

    // 5. targets + sampler.
    auto sh_pres = offscreen_presenter::create(*dev, kS, kS);
    REQUIRE(sh_pres.has_value());
    auto pres = offscreen_presenter::create(*dev, kW, kH);
    REQUIRE(pres.has_value());
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler sampler = VK_NULL_HANDLE;
    REQUIRE(vkCreateSampler(dev->handle(), &sci, nullptr, &sampler) == VK_SUCCESS);

    const VkFormat bgra = VK_FORMAT_B8G8R8A8_UNORM;
    const VkFormat one_color = bgra;
    auto pipeA = build_pipe(*dev, layoutA, sh_v->handle(), sh_f->handle(),
                            1, &one_color, offscreen_presenter::kDepthFormat, true);
    auto pipeB = build_pipe(*dev, layoutB, sc_v->handle(), sc_f->handle(),
                            1, &one_color, offscreen_presenter::kDepthFormat, true);
    REQUIRE(pipeA != VK_NULL_HANDLE);
    REQUIRE(pipeB != VK_NULL_HANDLE);

    auto vp_scissor = [&](VkCommandBuffer cmd, int w, int h) {
        const VkViewport v{0.f, 0.f, float(w), float(h), 0.f, 1.f};
        const VkRect2D s{{0, 0}, {std::uint32_t(w), std::uint32_t(h)}};
        vkCmdSetViewport(cmd, 0, 1, &v);
        vkCmdSetScissor(cmd, 0, 1, &s);
    };

    // 6. pass A — cube depth into the shadow presenter (0 color attachments).
    {
        VkCommandBuffer cmd = sh_pres->begin_command();
        REQUIRE(cmd != VK_NULL_HANDLE);
        REQUIRE(sh_pres->begin_pass(cmd));
        VkRenderingAttachmentInfo catt{};
        catt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        catt.imageView = sh_pres->color_view();
        catt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        catt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        catt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; // content never read
        VkRenderingAttachmentInfo datt{};
        datt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        datt.imageView = sh_pres->depth_view();
        datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        datt.clearValue.depthStencil.depth = 0.0f; // reversed-Z far
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {kS, kS}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &catt;
        ri.pDepthAttachment = &datt;
        vkCmdBeginRendering(cmd, &ri);
        vp_scissor(cmd, kS, kS);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeA);
        const VkDescriptorSet ds = *dset;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layoutA, 0, 1, &ds, 0, nullptr);
        vkCmdDraw(cmd, 36, 1, 0, 0);
        vkCmdEndRendering(cmd);
        REQUIRE(sh_pres->end_pass(cmd));
        REQUIRE(sh_pres->submit(cmd));
    }

    // 7. shadow texel pins off the readback (checklist: depth readback pins).
    const auto sz = sh_pres->capture_depth_f32();
    REQUIRE(sz.size() == static_cast<std::size_t>(kS) * kS);
    const auto lvp = [&](const vec3f& p) {
        return lightVP * vec4f::point(p);
    };
    const auto shadow_texel = [&](const vec3f& p) {
        const vec4f lc = lvp(p);
        const int tx = std::clamp(static_cast<int>(std::floor((lc.x * 0.5f + 0.5f) * kS)), 0, kS - 1);
        const int ty = std::clamp(static_cast<int>(std::floor((lc.y * 0.5f + 0.5f) * kS)), 0, kS - 1);
        return std::pair{tx, ty};
    };
    // cube-top center texel == CPU reversed-ortho depth (the nearest surface
    // along that column), flat neighborhood, cleared corner == 0 exactly.
    const auto [tcx, tcy] = shadow_texel(vec3f{0.f, 0.f, -4.f});
    const float expect_top = lvp(vec3f{0.f, 0.f, -4.f}).z;
    const float got_top = sz[static_cast<std::size_t>(tcy) * kS + tcx];
    MESSAGE("shadow texel (", tcx, ",", tcy, ") stored ", got_top, " expect ", expect_top);
    // Texel CENTER's stored depth vs the query point's: the top face is
    // oblique to the light plane, so they differ by up to one texel of face
    // gradient (~0.9 m depth over the map span). fp32 exactness impossible.
    CHECK(std::abs(got_top - expect_top) < 2e-3f);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            CHECK(std::abs(sz[static_cast<std::size_t>(tcy + dy) * kS + tcx + dx] - got_top) < 5e-3f);
    CHECK(sz[0] == 0.0f); // cleared far corner (outside cube column)
    int coverage = 0;
    for (float v : sz)
        if (v > 1e-6f) ++coverage; // clear is exact 0; anything else = pass A wrote it
    MESSAGE("shadow coverage texels: ", coverage);
    CHECK(coverage >= 1050); // projected cube area ~1110 texels, +-5%
    CHECK(coverage <= 1170);

    // 8. pass B — PBR scene sampling the shadow map.
    auto dset_s = pool->allocate(sset_layout->handle());
    REQUIRE(dset_s.has_value());
    // Depth-aspect sampling: descriptor layout must match the exposed
    // DEPTH_READ_ONLY_OPTIMAL (image-layout matching rules, 13.2.5).
    pool->write_sampler(*dset_s, 0, sampler, sh_pres->depth_view(),
                        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    struct Push {
        std::uint32_t mat_index, geom;
    };
    {
        VkCommandBuffer cmd = pres->begin_command();
        REQUIRE(cmd != VK_NULL_HANDLE);
        REQUIRE(sh_pres->expose_depth_for_sampling(cmd)); // READ_ONLY for sampling
        REQUIRE(pres->begin_pass(cmd));
        VkRenderingAttachmentInfo catt{};
        catt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        catt.imageView = pres->color_view();
        catt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        catt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        catt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        catt.clearValue.color.float32[0] = 1.0f; // magenta
        catt.clearValue.color.float32[1] = 0.0f;
        catt.clearValue.color.float32[2] = 1.0f;
        catt.clearValue.color.float32[3] = 1.0f;
        VkRenderingAttachmentInfo datt{};
        datt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        datt.imageView = pres->depth_view();
        datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        datt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        datt.clearValue.depthStencil.depth = 0.0f;
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {kW, kH}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &catt;
        ri.pDepthAttachment = &datt;
        vkCmdBeginRendering(cmd, &ri);
        vp_scissor(cmd, kW, kH);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeB);
        const VkDescriptorSet ds = *dset, ds2 = *dset_s;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layoutB, 0, 1, &ds, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layoutB, 1, 1, &ds2, 0, nullptr);
        const Push floor_push{0u, 0u}, cube_push{1u, 1u};
        vkCmdPushConstants(cmd, layoutB, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, &floor_push);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdPushConstants(cmd, layoutB, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, &cube_push);
        vkCmdDraw(cmd, 36, 1, 0, 0);
        vkCmdEndRendering(cmd);
        REQUIRE(pres->end_pass(cmd));
        REQUIRE(pres->submit(cmd));
    }

    const auto px = pres->capture_rgba8();
    REQUIRE(px.size() == static_cast<std::size_t>(kW) * kH * 4u);
    const auto bgra_at = [&](int x, int y) {
        const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4u;
        return std::array<std::uint8_t, 4>{px[o], px[o + 1], px[o + 2], px[o + 3]};
    };
    const mat4f mvp = proj * view;
    const auto px_at = [&](const vec3f& p) {
        const vec4f clip = mvp * vec4f::point(p);
        return std::pair{static_cast<int>(std::lround((clip.x / clip.w + 1.f) * 0.5f * kW)),
                         static_cast<int>(std::lround((clip.y / clip.w + 1.f) * 0.5f * kH))};
    };
    // CPU vis exactly as GLSL: read back the GPU's own shadow texel.
    const auto vis_at = [&](const vec3f& p) {
        const vec4f lc = lvp(p);
        const auto [tx, ty] = shadow_texel(p);
        const float stored = sz[static_cast<std::size_t>(ty) * kS + tx];
        return (lc.z + kBias < stored) ? 0.f : 1.f;
    };
    auto pin = [&](const vec3f& p, const vec3f& N, int mat, const char* label) {
        const auto [x, y] = px_at(p);
        const float vis = vis_at(p);
        const auto ref = pbr_ref(p, N, mat, vis);
        const auto got = bgra_at(x, y);
        MESSAGE(label, " px(", x, ",", y, ") vis ", vis, " ref BGR(", std::lround(ref[2] * 255.f),
                ",", std::lround(ref[1] * 255.f), ",", std::lround(ref[0] * 255.f),
                ") got BGR(", got[0], ",", got[1], ",", got[2], ")");
        for (int ch = 0; ch < 3; ++ch) {
            const float expect = ref[2 - ch] * 255.f;
            CHECK_MESSAGE(std::abs(static_cast<float>(got[ch]) - expect) <= 2.0f,
                          label << " ch" << ch << " got " << static_cast<int>(got[ch])
                                << " expect " << expect);
        }
    };
    // Hand-verified pins: shadow parallelogram on the floor = points whose
    // ray toward the light enters the cube; pins deep inside that set and
    // deep outside (see analysis in phase-03 notes). Cube-top pin on the
    // lit top face.
    pin(vec3f{-0.75f, -1.f, -4.f}, vec3f{0, 1, 0}, 0, "shadowed-floor");
    pin(vec3f{1.4f, -1.f, -4.f}, vec3f{0, 1, 0}, 0, "lit-floor");
    pin(vec3f{0.15f, 0.f, -3.8f}, vec3f{0, 1, 0}, 1, "cube-top");
    CHECK(bgra_at(1, 1) == kClearBgra);

    // Analytic sanity: the two floor pins must disagree (shadow IS casting).
    const auto [sx, sy] = px_at(vec3f{-0.75f, -1.f, -4.f});
    const auto [lx, ly] = px_at(vec3f{1.4f, -1.f, -4.f});
    CHECK(bgra_at(sx, sy) != bgra_at(lx, ly));

    // Golden.
    std::vector<std::uint8_t> rgba(px.size());
    for (std::size_t i = 0; i < px.size(); i += 4u) {
        rgba[i + 0] = px[i + 2];
        rgba[i + 1] = px[i + 1];
        rgba[i + 2] = px[i + 0];
        rgba[i + 3] = px[i + 3];
    }
    const auto gv = gfx_golden::check_golden("scene_64.png", rgba.data(), kW, kH, {});
    CHECK_MESSAGE(gv.ok, gv.message);

    vkDestroySampler(dev->handle(), sampler, nullptr);
    vkDestroyPipeline(dev->handle(), pipeA, nullptr);
    vkDestroyPipeline(dev->handle(), pipeB, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layoutA, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layoutB, nullptr);
    gfx_test::check_clean_validation(*inst);
}
