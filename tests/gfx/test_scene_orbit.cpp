// test: gfx_scene_orbit / gfx_scene_stereo — checklist 3.15 "done-when
// scene": the PoC hull (assets/samples/test_ship.glb) + a checker-textured
// deck, lit by the 3.10 PBR-lite rig with the 3.11 shadow map, rendered
// through the 3.2 camera rig (orbit + converged-stereo), consuming cooked
// MAT0 materials (3.8) and a cooked TEX0 texture BY CONTENT ID (manifest,
// decision #13).
//
// gfx_scene_orbit pins:
//   * hull loads by content id; its embedded MAT0 materials load and match
//     the payload; every mesh material_index is in range.
//   * center-pixel silhouette (depth written + non-clear color) at 4 orbit
//     angles, and captures at 0 vs 90 deg DIFFER (the ship is visible and
//     changes with yaw).
//   * deck pixels: captured depth == CPU-unprojected deck hit (2e-3) and
//     color == CPU GGX with albedo from the checker TEXEL the uv selects
//     and shadow vis read back from the GPU's own shadow map (2/255).
//   * determinism: two identical frames byte-identical.
//   * golden scene_orbit_45_64.png (byte-exact on lavapipe).
//
// gfx_scene_stereo pins (Q4 answer, "switchable stereo"):
//   * converged off-axis frustum asymmetry: left r+l == +ipd/focal, right
//     == -ipd/focal, equal widths (camera.h derivation, pinned directly).
//   * one instanced pass (instanceCount = eyes) with gl_ViewportIndex =
//     gl_InstanceIndex into two 64x64 viewport halves of a 128x64 target:
//     both halves render (center depth non-cleared), halves differ,
//     per-half goldens scene_stereo_l_64.png / scene_stereo_r_64.png.
//   * fixation: the hull sits at the focal distance, so each half's center
//     ray sees nearly the same surface -> center depths agree within 12%.
//   * determinism: second render byte-identical (color AND depth).
#include <doctest/doctest.h>
#include <ve/core/cooked.h>
#include <ve/core/math/mat4.h>
#include <ve/core/math/projection.h>
#include <ve/core/math/quat.h>
#include <ve/gfx/camera.h>
#include <ve/gfx/descriptor.h>
#include <ve/gfx/mesh.h>
#include <ve/gfx/pipeline_cache.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>
#include <ve/gfx/texture.h>

#include "gfx_golden.h"
#include "gfx_test_env.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ve/cook/cook.h>

namespace {
using namespace ve::core::math;
using namespace ve::gfx;

constexpr int kW = 64, kH = 64, kS = 256; // scene px, shadow map texels
constexpr float kBias = 0.002f;          // reversed-Z shadow bias (as 3.11)
constexpr float kFovY = deg_to_rad(60.0f);
constexpr float kNear = 0.1f, kFar = 50.f;

std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_sc_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}

// ---- shaders (cooked at test start: the framework has no inline SPIR-V) --
constexpr const char* kSceneVert = R"GLSL(#version 450
#extension GL_ARB_shader_viewport_layer_array : require  // gl_ViewportIndex (3.9 precedent)
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec2 uvin;
layout(set = 0, binding = 0, std140) uniform Cam {
    mat4 view[2]; mat4 proj[2]; vec4 camPos[2];
} cam;
layout(push_constant) uniform Push { uint matIndex; } push;
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec2 vuv;
layout(location = 1) flat out uint vmat;
layout(location = 2) out vec3 wpos;
layout(location = 3) out vec3 wnorm;
layout(location = 4) flat out vec3 veye;
void main() {
    int e = gl_InstanceIndex;            // one instance per eye (3.9 path)
    gl_ViewportIndex = e;               // stereo: eye e -> viewport e (Q4)
    gl_Position = cam.proj[e] * (cam.view[e] * vec4(pos, 1.0));
    veye = cam.camPos[e].xyz;                // frag cannot read gl_InstanceIndex
    vuv = uvin;
    vmat = push.matIndex;
    wpos = pos; wnorm = nrm;            // TRS baked at cook: model is identity
}
)GLSL";

constexpr const char* kSceneFrag = R"GLSL(#version 450
layout(set = 0, binding = 0, std140) uniform Cam {
    mat4 view[2]; mat4 proj[2]; vec4 camPos[2];
} cam;
layout(set = 0, binding = 1, std140) uniform Mats {
    vec4 base[16]; vec4 emis[16]; vec4 mr[16];
} mats;
layout(set = 0, binding = 2, std140) uniform Lights {
    vec4 dir; vec4 color; vec4 ambient; mat4 lightVP;
} lights;
layout(set = 1, binding = 0) uniform sampler2D albedo;     // per material
layout(set = 2, binding = 0) uniform sampler2D shadowMap;  // D32, texelFetch
layout(location = 0) in vec2 vuv;
layout(location = 1) flat in uint vmat;
layout(location = 2) in vec3 wpos;
layout(location = 3) in vec3 wnorm;
layout(location = 4) flat in vec3 veye;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 lc = lights.lightVP * vec4(wpos, 1.0);
    ivec2 st = clamp(ivec2((lc.xy * 0.5 + 0.5) * 256.0), ivec2(0), ivec2(255));
    float stored = texelFetch(shadowMap, st, 0).x;
    float vis = (lc.z + 0.002 < stored) ? 0.0 : 1.0;

    vec3 bcol = mats.base[vmat].rgb * texture(albedo, vuv).rgb;
    vec3 emis = mats.emis[vmat].rgb;
    float metal = mats.mr[vmat].x;
    float rough = max(mats.mr[vmat].y, 0.04);

    vec3 N = normalize(wnorm);
    vec3 V = normalize(veye - wpos);              // per-eye via flat varyings (Q4)
    vec3 L = normalize(lights.dir.xyz);  // toward the light (3.10 convention)
    float ndl = max(dot(N, L), 0.0);
    float ndv = max(dot(N, V), 0.0001);
    vec3 Hv = normalize(V + L);
    float ndh = max(dot(N, Hv), 0.0);
    float vdh = max(dot(V, Hv), 0.0);
    float a = rough * rough; float a2 = a * a;
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

constexpr const char* kShadowVert = R"GLSL(#version 450
layout(location = 0) in vec3 pos;
layout(set = 0, binding = 2, std140) uniform Lights {
    vec4 dir; vec4 color; vec4 ambient; mat4 lightVP;
} lights;
out gl_PerVertex { vec4 gl_Position; };
void main() { gl_Position = lights.lightVP * vec4(pos, 1.0); }
)GLSL";

constexpr const char* kShadowFrag = R"GLSL(#version 450
void main() {}
)GLSL";

// ---- scene constants (CPU mirror pins use these EXACT values) ------------
vec3f light_travel() { return normalize(vec3f{0.45f, -0.85f, -0.28f}); }
constexpr float kLightColor[4] = {1.0f, 0.96f, 0.9f, 1.2f};
constexpr float kAmbient[4]    = {0.4f, 0.45f, 0.5f, 0.4f};

// checker TEX0: 64x64, 16-texel cells, white/dark grey (untextured hull
// gets a 1x1 white texture instead: bcol *= 1 keeps MAT0 base colors).
constexpr int kTexW = 64, kTexCell = 16;
std::vector<std::uint8_t> author_checker() {
    std::vector<std::uint8_t> rgba(kTexW * kTexW * 4);
    for (int y = 0; y < kTexW; ++y) {
        for (int x = 0; x < kTexW; ++x) {
            const bool light = (((x / kTexCell) + (y / kTexCell)) & 1) == 0;
            const std::uint8_t v = light ? 240 : 45;
            const std::size_t o = (static_cast<std::size_t>(y) * kTexW + x) * 4;
            rgba[o] = v; rgba[o + 1] = v; rgba[o + 2] = v; rgba[o + 3] = 255;
        }
    }
    return rgba;
}

// deck material (authored here, appended to the hull's MAT0 list)
constexpr vec3f kDeckBase{0.72f, 0.72f, 0.75f};
constexpr float kDeckRough = 0.55f;
constexpr float kDeckSize = 12.f; // 12 x 12 m, planar uv covers it

quatf quat_yaw(float rad) { return {0.f, std::sin(rad * 0.5f), 0.f, std::cos(rad * 0.5f)}; }

// deck quad (4 verts / 2 tris) as a cooked_mesh for gpu_mesh::upload
cooked_mesh deck_mesh(vec3f c, float floor_y, std::uint32_t mat_index) {
    cooked_mesh m;
    m.name = "deck";
    m.material_index = mat_index;
    const float h = kDeckSize * 0.5f;
    const vec3f corners[4] = {{c.x - h, floor_y, c.z - h}, {c.x + h, floor_y, c.z - h},
                              {c.x + h, floor_y, c.z + h}, {c.x - h, floor_y, c.z + h}};
    const float uvv[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
    for (int i = 0; i < 4; ++i) {
        mesh_vertex v{};
        v.pos[0] = corners[i].x; v.pos[1] = corners[i].y; v.pos[2] = corners[i].z;
        v.nrm[1] = 1.f;
        v.uv[0] = uvv[i][0]; v.uv[1] = uvv[i][1];
        v.tan[0] = 1.f; v.tan[3] = 1.f;
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

struct SceneGpu {
    std::optional<offscreen_presenter> sh_pres, pres;
    std::vector<std::unique_ptr<gpu_mesh>> meshes; // hull meshes, then deck
};

// Cook the four shaders + the .glb + the checker png into one out dir.
void cook_scene_assets(const std::string& src, const std::string& out) {
    const auto p = std::filesystem::path(src);
    {
        std::ofstream f(p / "scene.vert", std::ios::trunc); f << kSceneVert;
    }
    { std::ofstream f(p / "scene.frag", std::ios::trunc); f << kSceneFrag; }
    { std::ofstream f(p / "shadow.vert", std::ios::trunc); f << kShadowVert; }
    { std::ofstream f(p / "shadow.frag", std::ios::trunc); f << kShadowFrag; }
    std::string err;
    REQUIRE_MESSAGE(gfx_golden::write_png_rgba8((p / "checker.png").string(), author_checker().data(),
                                                kTexW, kTexW, err), err);
    const std::string ship = std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb";
    REQUIRE_MESSAGE(std::filesystem::exists(ship), "missing test_ship.glb sample");
    std::filesystem::copy_file(ship, p / "test_ship.glb");
    REQUIRE_MESSAGE(ve::cook::cook_incremental(src, out).ok, "cook failed");
}

} // namespace

TEST_CASE("gfx_scene_orbit") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 scene test skipped");
        return;
    }
    const auto src = tmp_root("src");
    const auto out = tmp_root("out");
    cook_scene_assets(src, out);

    // 1. cooked scene data BY CONTENT ID (decision #13) ------------------
    auto meshes = load_meshes_by_id(out, "test_ship.glb");
    REQUIRE_MESSAGE(meshes.has_value(), meshes.error());
    CHECK(meshes->size() >= 1u);
    auto mats = load_materials_by_id(out, "test_ship.glb");
    REQUIRE_MESSAGE(mats.has_value(), mats.error());
    CHECK(mats->size() >= 1u);
    for (const auto& m : *meshes) {
        CHECK_MESSAGE(m.material_index < mats->size(), m.name << " material_index out of range");
    }
    for (const auto& mt : *mats) {
        CHECK(mt.metallic >= 0.f); CHECK(mt.metallic <= 1.f);
        CHECK(mt.roughness >= 0.f); CHECK(mt.roughness <= 1.f);
        for (int i = 0; i < 3; ++i) CHECK(mt.emissive[i] >= 0.f);
    }
    CHECK(mats->size() + 1u < 16u); // deck material appended; shader array cap
    auto tex = load_texture_by_id(out, "checker.png");
    REQUIRE_MESSAGE(tex.has_value(), tex.error());
    REQUIRE(tex->width == kTexW);
    const auto want_rgba = author_checker();
    CHECK(tex->pixels == want_rgba); // cooked TEX0 == authored pattern
    // Manifest authority: an id the cook never made is not loadable...
    CHECK_FALSE(load_texture_by_id(out, "nope.png").has_value());
    // ...and a MESH id is not a texture id.
    CHECK_FALSE(load_texture_by_id(out, "test_ship.glb").has_value());

    // hull AABB -> deck floor, orbit radius, light rig bounds
    vec3f bmin{1e9f, 1e9f, 1e9f}, bmax{-1e9f, -1e9f, -1e9f};
    for (const auto& m : *meshes) {
        for (const auto& v : m.vertices) {
            bmin.x = std::min(bmin.x, v.pos[0]); bmax.x = std::max(bmax.x, v.pos[0]);
            bmin.y = std::min(bmin.y, v.pos[1]); bmax.y = std::max(bmax.y, v.pos[1]);
            bmin.z = std::min(bmin.z, v.pos[2]); bmax.z = std::max(bmax.z, v.pos[2]);
        }
    }
    const vec3f c{(bmin.x + bmax.x) * .5f, (bmin.y + bmax.y) * .5f, (bmin.z + bmax.z) * .5f};
    const float extent = std::max({bmax.x - bmin.x, bmax.y - bmin.y, bmax.z - bmin.z});
    const float floor_y = bmin.y;
    const float orbit_r = std::max(3.f, extent * 2.1f + 1.f);
    CHECK(orbit_r < kFar * 0.4f);
    MESSAGE("HULL bmin(", bmin.x, ",", bmin.y, ",", bmin.z, ") bmax(", bmax.x, ",", bmax.y,
            ",", bmax.z, ") extent ", extent, " orbit_r ", orbit_r);

    // 2. device, shaders, GPU meshes, textures ---------------------------
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

    SceneGpu sc;
    for (const auto& m : *meshes) {
        auto g = gpu_mesh::upload(*dev, m);
        REQUIRE_MESSAGE(g.has_value(), g.error());
        sc.meshes.push_back(std::make_unique<gpu_mesh>(std::move(*g)));
    }
    const auto deck = deck_mesh(c, floor_y, static_cast<std::uint32_t>(mats->size()));
    {
        auto g = gpu_mesh::upload(*dev, deck);
        REQUIRE_MESSAGE(g.has_value(), g.error());
        sc.meshes.push_back(std::make_unique<gpu_mesh>(std::move(*g)));
    }
    auto deck_tex = gpu_texture::create(*dev, *tex);
    REQUIRE(deck_tex.has_value());
    CpuTexture white;
    white.width = white.height = 1;
    white.format = ve::core::cooked::kTexFmtRgba8;
    white.pixels = {255, 255, 255, 255};
    auto white_tex = gpu_texture::create(*dev, white);
    REQUIRE(white_tex.has_value());

    auto shp = offscreen_presenter::create(*dev, kS, kS);
    REQUIRE(shp.has_value());
    sc.sh_pres.emplace(std::move(*shp));
    auto pp = offscreen_presenter::create(*dev, kW, kH);
    REQUIRE(pp.has_value());
    sc.pres.emplace(std::move(*pp));

    // 3. light rig from the hull bounds (3.11) ----------------------------
    const auto [lview, lproj] = dir_light_shadow_rig(bmin, bmax, light_travel(),
                                                     vec3f{0, 1, 0}, 1.f, 0.5f,
                                                     depth_range::reversed_1_to_0);
    const mat4f lightVP = lproj * lview;
    const vec3f lt = light_travel();

    // 4. UBOs. Cam carries TWO view/proj/camPos slots (eye-indexed) — the
    // stereo case fills both; mono duplicates eye 0.
    struct CamUboFill { mat4f view[2]; mat4f proj[2]; vec4f camPos[2]; };
    auto cam_buf = uniform_buffer::create(*dev, sizeof(CamUboFill));
    REQUIRE(cam_buf.has_value());
    std::vector<float> mat_ubo(16 * 4 * 3, 0.f); // base[16], emis[16], mr[16]
    for (std::size_t m = 0; m < mats->size(); ++m) {
        const auto& mt = (*mats)[m];
        for (int i = 0; i < 3; ++i) mat_ubo[(m * 4) + i] = mt.base_color[i];
        mat_ubo[m * 4 + 3] = mt.base_color[3];
        mat_ubo[16 * 4 + m * 4 + 0] = mt.emissive[0];
        mat_ubo[16 * 4 + m * 4 + 1] = mt.emissive[1];
        mat_ubo[16 * 4 + m * 4 + 2] = mt.emissive[2];
        mat_ubo[2 * 16 * 4 + m * 4 + 0] = mt.metallic;
        mat_ubo[2 * 16 * 4 + m * 4 + 1] = mt.roughness;
    }
    { // deck material
        const std::size_t m = mats->size();
        mat_ubo[m * 4 + 0] = kDeckBase.x; mat_ubo[m * 4 + 1] = kDeckBase.y;
        mat_ubo[m * 4 + 2] = kDeckBase.z; mat_ubo[m * 4 + 3] = 1.f;
        mat_ubo[2 * 16 * 4 + m * 4 + 0] = 0.f;             // metallic
        mat_ubo[2 * 16 * 4 + m * 4 + 1] = kDeckRough;      // roughness
    }
    auto mat_buf = uniform_buffer::create(*dev, mat_ubo.size() * 4u);
    REQUIRE(mat_buf.has_value());
    mat_buf->write(mat_ubo.data(), mat_ubo.size() * 4u);

    std::vector<float> lit_ubo;
    lit_ubo.insert(lit_ubo.end(), {-lt.x, -lt.y, -lt.z, 0.f}); // dir = toward light
    for (int i = 0; i < 4; ++i) lit_ubo.push_back(kLightColor[i]);
    for (int i = 0; i < 4; ++i) lit_ubo.push_back(kAmbient[i]);
    for (int i = 0; i < 16; ++i) lit_ubo.push_back(lightVP.m[i]);
    auto lit_buf = uniform_buffer::create(*dev, lit_ubo.size() * 4u);
    REQUIRE(lit_buf.has_value());
    lit_buf->write(lit_ubo.data(), lit_ubo.size() * 4u);

    // 5. descriptor + pipeline plumbing -----------------------------------
    const uniform_set_layout::ubo_binding binds[3]{
        {0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT}, // Cam
        {1, VK_SHADER_STAGE_FRAGMENT_BIT},                              // Mats
        {2, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT}, // Lights
    };
    auto set_layout = uniform_set_layout::create(
        *dev, std::span<const uniform_set_layout::ubo_binding>{binds, 3});
    REQUIRE(set_layout.has_value());
    auto albedo_layout = sampler_set_layout::create(*dev, 0, VK_SHADER_STAGE_FRAGMENT_BIT);
    REQUIRE(albedo_layout.has_value());

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = 4;
    const VkDescriptorSetLayout setsS[1] = {set_layout->handle()};
    VkPipelineLayoutCreateInfo lciS{};
    lciS.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lciS.setLayoutCount = 1;
    lciS.pSetLayouts = setsS;
    VkPipelineLayout layout_shadow = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lciS, nullptr, &layout_shadow) == VK_SUCCESS);
    const VkDescriptorSetLayout setsB[3] = {set_layout->handle(), albedo_layout->handle(),
                                            albedo_layout->handle()};
    VkPipelineLayoutCreateInfo lciB{};
    lciB.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lciB.setLayoutCount = 3;
    lciB.pSetLayouts = setsB;
    lciB.pushConstantRangeCount = 1;
    lciB.pPushConstantRanges = &push;
    VkPipelineLayout layout_scene = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lciB, nullptr, &layout_scene) == VK_SUCCESS);

    // sampler: LOD range [0,0] — the deck is un-mip'd in this test so pins
    // read the mip0 texel exactly (minFilter NEAREST + clamp = nearest texel).
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.minLod = 0.f;
    sci.maxLod = 0.f;
    VkSampler sampler = VK_NULL_HANDLE;
    REQUIRE(vkCreateSampler(dev->handle(), &sci, nullptr, &sampler) == VK_SUCCESS);

    auto pool = descriptor_pool::create(*dev, 8, 16);
    REQUIRE(pool.has_value());
    auto dset = pool->allocate(set_layout->handle());
    REQUIRE(dset.has_value());
    pool->write_uniform(*dset, 0, cam_buf->handle(), cam_buf->size());
    pool->write_uniform(*dset, 1, mat_buf->handle(), mat_buf->size());
    pool->write_uniform(*dset, 2, lit_buf->handle(), lit_buf->size());
    auto dset_shadow = pool->allocate(albedo_layout->handle());
    REQUIRE(dset_shadow.has_value());
    pool->write_sampler(*dset_shadow, 0, sampler, sc.sh_pres->depth_view(),
                        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    std::vector<VkDescriptorSet> dset_albedo(sc.meshes.size());
    for (std::size_t i = 0; i < sc.meshes.size(); ++i) {
        auto s = pool->allocate(albedo_layout->handle());
        REQUIRE(s.has_value());
        const bool is_deck = i + 1 == sc.meshes.size();
        pool->write_sampler(*s, 0, sampler,
                            is_deck ? deck_tex->view() : white_tex->view(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dset_albedo[i] = *s;
    }

    pipeline_cache cache(*dev);
    pipeline_desc sd{};
    sd.layout = layout_shadow;
    sd.stages = {{VK_SHADER_STAGE_VERTEX_BIT, sh_v->handle()},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, sh_f->handle()}};
    sd.bindings = {{0, sizeof(mesh_vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    sd.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}}; // pos ONLY
    sd.cull = VK_CULL_MODE_NONE;
    sd.depth_format = VK_FORMAT_D32_SFLOAT;  // cache bakes this into
    //    VkPipelineDepthStencilStateCreateInfo — UNDEFINED silently drops
    //    all depth writes (3.11 sets it explicitly; 3.3 builds by hand).
    sd.depth_test = true;
    sd.depth_write = true;
    // Sacrificial color attachment: lavapipe drops draws in zero-color
    // passes (3.11 gotcha) — the pipeline must declare the 1 format the
    // pass actually attaches.
    sd.color_formats = {VK_FORMAT_B8G8R8A8_UNORM};
    sd.blends = {blend_attachment_state{}};
    sd.viewport_count = 1;
    sd.scissor_count = 1;
    sd.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);
    auto pipe_shadow = cache.get(sd);
    REQUIRE_MESSAGE(pipe_shadow.has_value(), pipe_shadow.error());

    pipeline_desc bd{};
    bd.layout = layout_scene;
    bd.stages = {{VK_SHADER_STAGE_VERTEX_BIT, sc_v->handle()},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, sc_f->handle()}};
    bd.bindings = {{0, sizeof(mesh_vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    // pos MUST be R32G32B32: the 3.14 default-fill gotcha (a narrow pos
    // attribute silently gets z=1 -> clip.w=-1 -> whole mesh vanishes).
    bd.attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},   // pos
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},  // nrm
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},     // uv
    };
    bd.cull = VK_CULL_MODE_NONE; // hull winding not verified here (cook 3.5 scope)
    bd.depth_format = VK_FORMAT_D32_SFLOAT;  // see sd note (3.15 gotcha)
    bd.depth_test = true;
    bd.depth_write = true;
    bd.color_formats = {VK_FORMAT_B8G8R8A8_UNORM};
    bd.blends = {blend_attachment_state{}}; // opaque (NOT clear(): see 3.14)
    bd.viewport_count = 1;
    bd.scissor_count = 1;
    bd.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);
    auto pipe_scene = cache.get(bd);
    REQUIRE_MESSAGE(pipe_scene.has_value(), pipe_scene.error());

    // camera rig: desktop centered eye (same path as stereo)
    camera_rig rig;
    eye_def mono{};
    mono.frust = symmetric_frustum(kFovY, 1.f, kNear, kFar);
    mono.frust.depth = depth_range::reversed_1_to_0;
    mono.view = {0, 0, kW, kH};
    rig.add_eye(mono);

    // 6. one full scene render at orbit yaw; returns captures -------------
    struct Caps { std::vector<std::uint8_t> color; std::vector<float> depth, shadow; };
    std::vector<float> shd;
    auto render_scene = [&](float yaw, trsf& rig_out) {
        const vec3f pos{c.x + orbit_r * std::sin(yaw), c.y, c.z + orbit_r * std::cos(yaw)};
        rig_out = trsf{pos, quat_yaw(yaw), {1.f, 1.f, 1.f}};
        rig.set_rig(rig_out);
        const auto frames = rig.compute();
        REQUIRE(frames.size() == 1u);

        CamUboFill cu{};
        cu.view[0] = cu.view[1] = frames[0].view;
        cu.proj[0] = cu.proj[1] = frames[0].proj;
        cu.camPos[0] = cu.camPos[1] = vec4f{pos.x, pos.y, pos.z, 1.f};
        cam_buf->write(&cu, sizeof(cu));

        // pass A: hull depth into the shadow map (casters = hull only)
        {
            VkCommandBuffer cmd = sc.sh_pres->begin_command();
            REQUIRE(cmd != VK_NULL_HANDLE);
            REQUIRE(sc.sh_pres->begin_pass(cmd));
            VkRenderingAttachmentInfo catt{};
            catt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            catt.imageView = sc.sh_pres->color_view();
            catt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            catt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            catt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            VkRenderingAttachmentInfo datt{};
            datt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            datt.imageView = sc.sh_pres->depth_view();
            datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            datt.clearValue.depthStencil.depth = 0.0f; // reversed-Z far
            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea = {{0, 0}, {kS, kS}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &catt; // sacrificial (lavapipe 3.11 gotcha)
            ri.pDepthAttachment = &datt;
            vkCmdBeginRendering(cmd, &ri);
            const VkViewport v{0.f, 0.f, float(kS), float(kS), 0.f, 1.f};
            const VkRect2D s{{0, 0}, {kS, kS}};
            vkCmdSetViewport(cmd, 0, 1, &v);
            vkCmdSetScissor(cmd, 0, 1, &s);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe_shadow);
            const VkDescriptorSet ds = *dset;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_shadow, 0, 1,
                                    &ds, 0, nullptr);
            for (std::size_t i = 0; i + 1 < sc.meshes.size(); ++i) {
                const VkBuffer vb = sc.meshes[i]->vertex_buffer();
                const VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
                vkCmdBindIndexBuffer(cmd, sc.meshes[i]->index_buffer(), 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, sc.meshes[i]->index_count(), 1, 0, 0, 0);
            }
            vkCmdEndRendering(cmd);
            REQUIRE(sc.sh_pres->end_pass(cmd));
            REQUIRE(sc.sh_pres->submit(cmd));
            // Read the shadow map back HERE: end_pass left it TRANSFER_SRC;
            // pass B will re-expose it READ_ONLY afterwards.
            shd = sc.sh_pres->capture_depth_f32();
        }

        // pass B: PBR-lite scene with shadow + albedo terms
        {
            VkCommandBuffer cmd = sc.pres->begin_command();
            REQUIRE(cmd != VK_NULL_HANDLE);
            REQUIRE(sc.sh_pres->expose_depth_for_sampling(cmd));
            REQUIRE(sc.pres->begin_pass(cmd));
            VkRenderingAttachmentInfo catt{};
            catt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            catt.imageView = sc.pres->color_view();
            catt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            catt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            catt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            catt.clearValue.color.float32[0] = 1.f; // white bg (B8G8R8A8)
            catt.clearValue.color.float32[1] = 1.f;
            catt.clearValue.color.float32[2] = 1.f;
            catt.clearValue.color.float32[3] = 1.f;
            VkRenderingAttachmentInfo datt{};
            datt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            datt.imageView = sc.pres->depth_view();
            datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // depth captured for pins
            datt.clearValue.depthStencil.depth = 0.0f;
            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea = {{0, 0}, {kW, kH}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &catt;
            ri.pDepthAttachment = &datt;
            vkCmdBeginRendering(cmd, &ri);
            const VkViewport v{0.f, 0.f, float(kW), float(kH), 0.f, 1.f};
            const VkRect2D s{{0, 0}, {kW, kH}};
            vkCmdSetViewport(cmd, 0, 1, &v);
            vkCmdSetScissor(cmd, 0, 1, &s);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe_scene);
            const VkDescriptorSet ds = *dset, dss = *dset_shadow;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_scene, 0, 1,
                                    &ds, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_scene, 2, 1,
                                    &dss, 0, nullptr);
            for (std::size_t i = 0; i < sc.meshes.size(); ++i) {
                const std::uint32_t mat =
                    i + 1 == sc.meshes.size() ? deck.material_index
                                              : (*meshes)[i].material_index;
                const VkBuffer vb = sc.meshes[i]->vertex_buffer();
                const VkDeviceSize off = 0;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_scene, 1, 1,
                                        &dset_albedo[i], 0, nullptr);
                vkCmdPushConstants(cmd, layout_scene, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &mat);
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
                vkCmdBindIndexBuffer(cmd, sc.meshes[i]->index_buffer(), 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, sc.meshes[i]->index_count(), 1, 0, 0, 0);
            }
            vkCmdEndRendering(cmd);
            REQUIRE(sc.pres->end_pass(cmd));
            REQUIRE(sc.pres->submit(cmd));
        }
        return Caps{sc.pres->capture_rgba8(), sc.pres->capture_depth_f32(), shd};
    };

    // 7. orbit visibility: silhouette at 4 angles; yaw changes the frame ---
    const auto rgba_at = [](const std::vector<std::uint8_t>& px, int x, int y) {
        const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4u;
        return std::array<std::uint8_t, 4>{px[o], px[o + 1], px[o + 2], px[o + 3]};
    };
    std::vector<std::uint8_t> first_c0, first_c90;
    for (int i = 0; i < 4; ++i) {
        trsf rig_out;
        auto caps = render_scene(i * 0.25f * 3.14159265f * 2.f, rig_out);
        const auto& px = caps.color;
        const auto& dz = caps.depth;
        const auto center = rgba_at(px, kW / 2, kH / 2);
        const bool painted = center[0] != 255 || center[1] != 255 || center[2] != 255;
        const bool depth_written = dz[static_cast<std::size_t>(kH / 2) * kW + kW / 2] > 0.f;
        CHECK_MESSAGE(painted, "yaw " << i << "*90deg: hull not at frame center");
        CHECK_MESSAGE(depth_written, "yaw " << i << "*90deg: no depth at center");
        if (i == 0) first_c0 = px;
        if (i == 1) first_c90 = px;
    }
    CHECK(first_c0 != first_c90); // the orbit actually changes the view

    // 8. deck pin: full CPU mirror at real deck pixels --------------------
    trsf rig_out;
    const auto caps = render_scene(0.7f, rig_out);
    const auto& px = caps.color;
    const auto& dz = caps.depth;
    // Backproject with the EXACT matrices the GPU used (rig.compute() ==
    // invert_rigid(compose(rig)*compose(eye_local)) + perspective_offaxis).
    const auto frames = rig.compute();
    REQUIRE(frames.size() == 1u);
    const mat4f PV = frames[0].proj * frames[0].view;
    const auto invPV_opt = invert(PV);
    REQUIRE(invPV_opt.has_value());
    const mat4f invPV = *invPV_opt;
    const vec3f cam_pos = rig_out.position;

    const auto vis_at = [&](const vec3f& p) {
        const vec4f lc = lightVP * vec4f::point(p);
        const auto tx = std::clamp(static_cast<int>((lc.x * .5f + .5f) * kS), 0, kS - 1);
        const auto ty = std::clamp(static_cast<int>((lc.y * .5f + .5f) * kS), 0, kS - 1);
        const float stored = shd[static_cast<std::size_t>(ty) * kS + tx];
        return (lc.z + kBias < stored) ? 0.f : 1.f;
    };
    const auto win_z = [&](const vec3f& p) {
        const vec4f clip = PV * vec4f::point(p);
        return clip.z / clip.w; // reversed_1_to_0: clip.z/clip.w IS the window
                                // depth (near 1, far 0); no [-1,1] remap.
    };
    // CPU GGX mirror (expression order == kSceneFrag)
    const auto pbr_ref = [&](const vec3f& p, const vec3f& N, const vec3f& bcol,
                             float metal, float rough, float vis) {
        rough = std::max(rough, 0.04f);
        const float bc[3] = {bcol.x, bcol.y, bcol.z};
        const vec3f L = normalize(vec3f{-lt.x, -lt.y, -lt.z});
        const vec3f Vv = normalize(cam_pos - p);
        const float ndl = std::max(dot(N, L), 0.f);
        const float ndv = std::max(dot(N, Vv), 0.0001f);
        const vec3f H = normalize(Vv + L);
        const float ndh = std::max(dot(N, H), 0.f);
        const float vdh = std::max(dot(Vv, H), 0.f);
        const float a = rough * rough, a2 = a * a;
        const float h = ndh * ndh * (a2 - 1.f) + 1.f;
        const float D = a2 / std::max(3.14159265f * h * h, 1e-7f);
        const float vg = 0.25f / std::max(ndl * ndv, 0.0001f);
        float F0[3], outv[3];
        for (int ch = 0; ch < 3; ++ch) F0[ch] = 0.04f + (bc[ch] - 0.04f) * metal;
        for (int ch = 0; ch < 3; ++ch) {
            const float F = F0[ch] + (1.f - F0[ch]) * std::pow(1.f - vdh, 5.f);
            const float spec = D * vg * F;
            const float diff = bc[ch] * (1.f - metal) / 3.14159265f;
            const float li = kLightColor[ch] * kLightColor[3];
            const float amb = kAmbient[ch] * kAmbient[3];
            const float base = li * ndl * vis * (diff + spec) + amb * bc[ch];
            outv[ch] = std::clamp(base, 0.f, 1.f);
        }
        return vec3f{outv[0], outv[1], outv[2]};
    };

    int deck_pins = 0;
    for (int py = kH - 2; py >= kH / 2 && deck_pins < 2; --py) {
        for (int px_x = kW / 2 - 8; px_x < kW / 2 + 8 && deck_pins < 2; ++px_x) {
            const float zcap = dz[static_cast<std::size_t>(py) * kW + px_x];
            if (zcap <= 1e-4f) continue; // cleared = sky
            // backproject to a world ray (pixel center; NDC y-down == Vulkan)
            const float nx = (px_x + 0.5f) / kW * 2.f - 1.f;
            const float ny = (py + 0.5f) / kH * 2.f - 1.f;
            const vec4f fa = invPV * vec4f{nx, ny, -1.f, 1.f};
            const vec4f fb = invPV * vec4f{nx, ny, 1.f, 1.f};
            const vec3f pa{fa.x / fa.w, fa.y / fa.w, fa.z / fa.w};
            const vec3f pb{fb.x / fb.w, fb.y / fb.w, fb.z / fb.w};
            const vec3f rd = pb - pa;
            if (std::abs(rd.y) < 1e-6f) continue;
            const float s = (floor_y - pa.y) / rd.y;
            if (s <= 0.f) continue;
            const vec3f hit = pa + rd * s;
            if (std::abs(hit.x - c.x) > kDeckSize * .45f || std::abs(hit.z - c.z) > kDeckSize * .45f)
                continue; // outside the deck quad
            if (std::abs(zcap - win_z(hit)) > 2e-3f) continue; // occluded by hull
            // deck uv -> checker texel
            const float uu = (hit.x - (c.x - kDeckSize * .5f)) / kDeckSize;
            const float vv = (hit.z - (c.z - kDeckSize * .5f)) / kDeckSize;
            const auto ix = static_cast<int>(uu * kTexW);
            const auto iy = static_cast<int>(vv * kTexW);
            if (ix < 1 || ix > kTexW - 2 || iy < 1 || iy > kTexW - 2) continue;
            if (ix % kTexCell == 0 || ix % kTexCell == kTexCell - 1) continue;
            if (iy % kTexCell == 0 || iy % kTexCell == kTexCell - 1) continue;
            const std::size_t to = (static_cast<std::size_t>(iy) * kTexW + ix) * 4u;
            const float texel = want_rgba[to] / 255.f;
            const vec3f bcol{kDeckBase.x * texel, kDeckBase.y * texel, kDeckBase.z * texel};
            const float vis = vis_at(hit);
            const auto ref = pbr_ref(hit, vec3f{0, 1, 0}, bcol, 0.f, kDeckRough, vis);
            const auto got = rgba_at(px, px_x, py);
            const float rf[3] = {ref.x, ref.y, ref.z};
            bool ok = true;
            for (int ch = 0; ch < 3; ++ch) {
                if (std::abs(static_cast<float>(got[ch]) - rf[2 - ch] * 255.f) > 2.5f) ok = false;
            }
            CHECK_MESSAGE(ok, "deck pin (" << px_x << "," << py << ") color mismatch vis=" << vis);
            ++deck_pins;
        }
    }
    { // DEBUG: ascii map of pinning frame (clear='.', painted='#', deck-hit='D')
        std::string map;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const auto& pc = px[(static_cast<std::size_t>(y) * 64 + x) * 4u];
                const float d = dz[static_cast<std::size_t>(y) * 64 + x];
                map += (pc == 0 && d <= 1e-6f) ? '.' : '#';
            }
            map += '\n';
        }
        MESSAGE("PIN FRAME MAP\n", map);
    }
    CHECK_MESSAGE(deck_pins >= 2, "no deck pixels matched the CPU backprojection");

    // 9. determinism + golden ---------------------------------------------
    const auto caps2 = render_scene(0.7f, rig_out);
    CHECK(px == caps2.color);
    CHECK(dz == caps2.depth);
    CHECK(shd == caps2.shadow);

    std::vector<std::uint8_t> rgba(px.size());
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = px[i + 2];
        rgba[i + 1] = px[i + 1];
        rgba[i + 2] = px[i + 0];
        rgba[i + 3] = px[i + 3];
    }
    const auto gv = gfx_golden::check_golden("scene_orbit_64.png", rgba.data(), kW, kH, {});
    CHECK_MESSAGE(gv.ok, gv.message);

    vkDeviceWaitIdle(dev->handle());
    vkDestroySampler(dev->handle(), sampler, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout_shadow, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout_scene, nullptr);
}

TEST_CASE("gfx_scene_stereo") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 stereo scene test skipped");
        return;
    }
    const auto src = tmp_root("ssrc");
    const auto out = tmp_root("sout");
    cook_scene_assets(src, out);

    auto meshes = load_meshes_by_id(out, "test_ship.glb");
    REQUIRE_MESSAGE(meshes.has_value(), meshes.error());
    auto mats = load_materials_by_id(out, "test_ship.glb");
    REQUIRE_MESSAGE(mats.has_value(), mats.error());
    auto tex = load_texture_by_id(out, "checker.png");
    REQUIRE_MESSAGE(tex.has_value(), tex.error());

    vec3f bmin{1e9f, 1e9f, 1e9f}, bmax{-1e9f, -1e9f, -1e9f};
    for (const auto& m : *meshes)
        for (const auto& v : m.vertices) {
            bmin.x = std::min(bmin.x, v.pos[0]); bmax.x = std::max(bmax.x, v.pos[0]);
            bmin.y = std::min(bmin.y, v.pos[1]); bmax.y = std::max(bmax.y, v.pos[1]);
            bmin.z = std::min(bmin.z, v.pos[2]); bmax.z = std::max(bmax.z, v.pos[2]);
        }
    const vec3f c{(bmin.x + bmax.x) * .5f, (bmin.y + bmax.y) * .5f, (bmin.z + bmax.z) * .5f};
    const float extent = std::max({bmax.x - bmin.x, bmax.y - bmin.y, bmax.z - bmin.z});
    const float floor_y = bmin.y;
    const float orbit_r = std::max(3.f, extent * 2.1f + 1.f);

    // Q4 answer: converged off-axis frustums (no toe-in) — pin the
    // asymmetry directly from the helper (camera.h derivation).
    constexpr float kIpd = 0.062f;
    const float focal = orbit_r; // hull center sits on the fixation axis
    auto stereo = converged_stereo_frustums(kFovY, 1.f, kNear, kFar, kIpd, focal);
    CHECK(stereo.first.r + stereo.first.l > 0.f);  // left eye: +ipd/focal
    CHECK(stereo.second.r + stereo.second.l < 0.f); // right eye: -ipd/focal
    CHECK(std::abs((stereo.first.r + stereo.first.l) - kIpd / focal) < 1e-5f);
    CHECK(std::abs((stereo.second.r + stereo.second.l) + kIpd / focal) < 1e-5f);
    CHECK(std::abs((stereo.first.r - stereo.first.l) - (stereo.second.r - stereo.second.l)) < 1e-5f);

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

    SceneGpu sc;
    for (const auto& m : *meshes) {
        auto g = gpu_mesh::upload(*dev, m);
        REQUIRE_MESSAGE(g.has_value(), g.error());
        sc.meshes.push_back(std::make_unique<gpu_mesh>(std::move(*g)));
    }
    const auto deck = deck_mesh(c, floor_y, static_cast<std::uint32_t>(mats->size()));
    {
        auto g = gpu_mesh::upload(*dev, deck);
        REQUIRE_MESSAGE(g.has_value(), g.error());
        sc.meshes.push_back(std::make_unique<gpu_mesh>(std::move(*g)));
    }
    auto deck_tex = gpu_texture::create(*dev, *tex);
    REQUIRE(deck_tex.has_value());
    CpuTexture white;
    white.width = white.height = 1;
    white.format = ve::core::cooked::kTexFmtRgba8;
    white.pixels = {255, 255, 255, 255};
    auto white_tex = gpu_texture::create(*dev, white);
    REQUIRE(white_tex.has_value());

    auto shp = offscreen_presenter::create(*dev, kS, kS);
    REQUIRE(shp.has_value());
    sc.sh_pres.emplace(std::move(*shp));
    constexpr int kRW = 128; // two 64x64 halves
    auto pp = offscreen_presenter::create(*dev, kRW, kH);
    REQUIRE(pp.has_value());
    sc.pres.emplace(std::move(*pp));

    const auto [lview, lproj] = dir_light_shadow_rig(bmin, bmax, light_travel(),
                                                     vec3f{0, 1, 0}, 1.f, 0.5f,
                                                     depth_range::reversed_1_to_0);
    const mat4f lightVP = lproj * lview;
    const vec3f lt = light_travel();

    struct CamUboFill { mat4f view[2]; mat4f proj[2]; vec4f camPos[2]; };
    auto cam_buf = uniform_buffer::create(*dev, sizeof(CamUboFill));
    REQUIRE(cam_buf.has_value());
    std::vector<float> mat_ubo(16 * 4 * 3, 0.f);
    for (std::size_t m = 0; m < mats->size(); ++m) {
        const auto& mt = (*mats)[m];
        for (int i = 0; i < 3; ++i) {
            mat_ubo[(m * 4) + i] = mt.base_color[i];
            mat_ubo[16 * 4 + m * 4 + i] = mt.emissive[i];
        }
        mat_ubo[m * 4 + 3] = mt.base_color[3];
        mat_ubo[2 * 16 * 4 + m * 4 + 0] = mt.metallic;
        mat_ubo[2 * 16 * 4 + m * 4 + 1] = mt.roughness;
    }
    {
        const std::size_t m = mats->size();
        mat_ubo[m * 4 + 0] = kDeckBase.x; mat_ubo[m * 4 + 1] = kDeckBase.y;
        mat_ubo[m * 4 + 2] = kDeckBase.z; mat_ubo[m * 4 + 3] = 1.f;
        mat_ubo[2 * 16 * 4 + m * 4 + 1] = kDeckRough;
    }
    auto mat_buf = uniform_buffer::create(*dev, mat_ubo.size() * 4u);
    REQUIRE(mat_buf.has_value());
    mat_buf->write(mat_ubo.data(), mat_ubo.size() * 4u);
    std::vector<float> lit_ubo;
    lit_ubo.insert(lit_ubo.end(), {-lt.x, -lt.y, -lt.z, 0.f});
    for (int i = 0; i < 4; ++i) lit_ubo.push_back(kLightColor[i]);
    for (int i = 0; i < 4; ++i) lit_ubo.push_back(kAmbient[i]);
    for (int i = 0; i < 16; ++i) lit_ubo.push_back(lightVP.m[i]);
    auto lit_buf = uniform_buffer::create(*dev, lit_ubo.size() * 4u);
    REQUIRE(lit_buf.has_value());
    lit_buf->write(lit_ubo.data(), lit_ubo.size() * 4u);

    const uniform_set_layout::ubo_binding binds[3]{
        {0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    auto set_layout = uniform_set_layout::create(
        *dev, std::span<const uniform_set_layout::ubo_binding>{binds, 3});
    REQUIRE(set_layout.has_value());
    auto albedo_layout = sampler_set_layout::create(*dev, 0, VK_SHADER_STAGE_FRAGMENT_BIT);
    REQUIRE(albedo_layout.has_value());

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = 4;
    const VkDescriptorSetLayout setsS[1] = {set_layout->handle()};
    VkPipelineLayoutCreateInfo lciS{};
    lciS.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lciS.setLayoutCount = 1;
    lciS.pSetLayouts = setsS;
    VkPipelineLayout layout_shadow = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lciS, nullptr, &layout_shadow) == VK_SUCCESS);
    const VkDescriptorSetLayout setsB[3] = {set_layout->handle(), albedo_layout->handle(),
                                            albedo_layout->handle()};
    VkPipelineLayoutCreateInfo lciB{};
    lciB.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lciB.setLayoutCount = 3;
    lciB.pSetLayouts = setsB;
    lciB.pushConstantRangeCount = 1;
    lciB.pPushConstantRanges = &push;
    VkPipelineLayout layout_scene = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lciB, nullptr, &layout_scene) == VK_SUCCESS);

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.minLod = 0.f;
    sci.maxLod = 0.f;
    VkSampler sampler = VK_NULL_HANDLE;
    REQUIRE(vkCreateSampler(dev->handle(), &sci, nullptr, &sampler) == VK_SUCCESS);

    auto pool = descriptor_pool::create(*dev, 8, 16);
    REQUIRE(pool.has_value());
    auto dset = pool->allocate(set_layout->handle());
    REQUIRE(dset.has_value());
    pool->write_uniform(*dset, 0, cam_buf->handle(), cam_buf->size());
    pool->write_uniform(*dset, 1, mat_buf->handle(), mat_buf->size());
    pool->write_uniform(*dset, 2, lit_buf->handle(), lit_buf->size());
    auto dset_shadow = pool->allocate(albedo_layout->handle());
    REQUIRE(dset_shadow.has_value());
    pool->write_sampler(*dset_shadow, 0, sampler, sc.sh_pres->depth_view(),
                        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    std::vector<VkDescriptorSet> dset_albedo(sc.meshes.size());
    for (std::size_t i = 0; i < sc.meshes.size(); ++i) {
        auto s = pool->allocate(albedo_layout->handle());
        REQUIRE(s.has_value());
        const bool is_deck = i + 1 == sc.meshes.size();
        pool->write_sampler(*s, 0, sampler,
                            is_deck ? deck_tex->view() : white_tex->view(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dset_albedo[i] = *s;
    }

    pipeline_cache cache(*dev);
    pipeline_desc sd{};
    sd.layout = layout_shadow;
    sd.stages = {{VK_SHADER_STAGE_VERTEX_BIT, sh_v->handle()},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, sh_f->handle()}};
    sd.bindings = {{0, sizeof(mesh_vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    sd.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}};
    sd.cull = VK_CULL_MODE_NONE;
    sd.depth_format = VK_FORMAT_D32_SFLOAT;  // cache bakes this into
    //    VkPipelineDepthStencilStateCreateInfo — UNDEFINED silently drops
    //    all depth writes (3.11 sets it explicitly; 3.3 builds by hand).
    sd.depth_test = true;
    sd.depth_write = true;
    sd.color_formats = {};
    sd.blends = {};
    sd.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);
    auto pipe_shadow = cache.get(sd);
    REQUIRE_MESSAGE(pipe_shadow.has_value(), pipe_shadow.error());

    pipeline_desc bd{};
    bd.layout = layout_scene;
    bd.stages = {{VK_SHADER_STAGE_VERTEX_BIT, sc_v->handle()},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, sc_f->handle()}};
    bd.bindings = {{0, sizeof(mesh_vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    bd.attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
    };
    bd.cull = VK_CULL_MODE_BACK_BIT;
    bd.depth_format = VK_FORMAT_D32_SFLOAT;  // see sd note (3.15 gotcha)
    bd.depth_test = true;
    bd.depth_write = true;
    bd.color_formats = {VK_FORMAT_B8G8R8A8_UNORM};
    bd.blends = {blend_attachment_state{}};
    bd.viewport_count = 2; // two eyes in one instanced pass (3.9 pattern)
    bd.scissor_count = 2;
    bd.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);
    auto pipe_scene = cache.get(bd);
    REQUIRE_MESSAGE(pipe_scene.has_value(), pipe_scene.error());

    // converged stereo rig (the Q4 "switchable stereo" shape): eyes sit at
    // +/-ipd/2 in rig space (eye_def.rig_local) on off-axis frustums.
    stereo.first.depth = stereo.second.depth = depth_range::reversed_1_to_0;
    camera_rig rig;
    eye_def le{};
    le.rig_local.position = {-kIpd * .5f, 0.f, 0.f};
    le.frust = stereo.first;
    le.view = {0, 0, 64, kH};
    eye_def re{};
    re.rig_local.position = {kIpd * .5f, 0.f, 0.f};
    re.frust = stereo.second;
    re.view = {64, 0, 64, kH};
    rig.add_eye(le);
    rig.add_eye(re);
    REQUIRE(rig.eye_count() == 2u);
    CHECK(rig.eye(0).frust.r - rig.eye(0).frust.l > 0.f);

    auto render_stereo = [&](float yaw, trsf& rig_out) {
        const vec3f pos{c.x + orbit_r * std::sin(yaw), c.y, c.z + orbit_r * std::cos(yaw)};
        rig_out = trsf{pos, quat_yaw(yaw), {1.f, 1.f, 1.f}};
        rig.set_rig(rig_out);
        const auto frames = rig.compute();
        REQUIRE(frames.size() == 2u);

        CamUboFill cu{};
        cu.view[0] = frames[0].view; cu.view[1] = frames[1].view;
        cu.proj[0] = frames[0].proj; cu.proj[1] = frames[1].proj;
        const vec3f eye_off[2] = {{-kIpd * .5f, 0.f, 0.f}, {kIpd * .5f, 0.f, 0.f}};
        for (int e = 0; e < 2; ++e) {
            const vec3f ep = rig_out.position + rotate(rig_out.rotation, eye_off[e]);
            cu.camPos[e] = vec4f{ep.x, ep.y, ep.z, 1.f};
        }
        cam_buf->write(&cu, sizeof(cu));

        { // pass A (shadow)
            VkCommandBuffer cmd = sc.sh_pres->begin_command();
            REQUIRE(cmd != VK_NULL_HANDLE);
            REQUIRE(sc.sh_pres->begin_pass(cmd));
            VkRenderingAttachmentInfo catt{};
            catt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            catt.imageView = sc.sh_pres->color_view();
            catt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            catt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            catt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            VkRenderingAttachmentInfo datt{};
            datt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            datt.imageView = sc.sh_pres->depth_view();
            datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            datt.clearValue.depthStencil.depth = 0.0f;
            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea = {{0, 0}, {kS, kS}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &catt;
            ri.pDepthAttachment = &datt;
            vkCmdBeginRendering(cmd, &ri);
            const VkViewport v{0.f, 0.f, float(kS), float(kS), 0.f, 1.f};
            const VkRect2D s{{0, 0}, {kS, kS}};
            vkCmdSetViewport(cmd, 0, 1, &v);
            vkCmdSetScissor(cmd, 0, 1, &s);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe_shadow);
            const VkDescriptorSet ds = *dset;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_shadow, 0, 1,
                                    &ds, 0, nullptr);
            for (std::size_t i = 0; i + 1 < sc.meshes.size(); ++i) {
                const VkBuffer vb = sc.meshes[i]->vertex_buffer();
                const VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
                vkCmdBindIndexBuffer(cmd, sc.meshes[i]->index_buffer(), 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, sc.meshes[i]->index_count(), 1, 0, 0, 0);
            }
            vkCmdEndRendering(cmd);
            REQUIRE(sc.sh_pres->end_pass(cmd));
            REQUIRE(sc.sh_pres->submit(cmd));
        }
        { // pass B — ONE instanced pass, eye = instance, viewport per eye
            VkCommandBuffer cmd = sc.pres->begin_command();
            REQUIRE(cmd != VK_NULL_HANDLE);
            REQUIRE(sc.sh_pres->expose_depth_for_sampling(cmd));
            REQUIRE(sc.pres->begin_pass(cmd));
            VkRenderingAttachmentInfo catt{};
            catt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            catt.imageView = sc.pres->color_view();
            catt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            catt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            catt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            catt.clearValue.color.float32[0] = 1.f;
            catt.clearValue.color.float32[1] = 1.f;
            catt.clearValue.color.float32[2] = 1.f;
            catt.clearValue.color.float32[3] = 1.f;
            VkRenderingAttachmentInfo datt{};
            datt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            datt.imageView = sc.pres->depth_view();
            datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            datt.clearValue.depthStencil.depth = 0.0f;
            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea = {{0, 0}, {kRW, kH}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &catt;
            ri.pDepthAttachment = &datt;
            vkCmdBeginRendering(cmd, &ri);
            const viewport vps[2] = {{0, 0, 64, kH}, {64, 0, 64, kH}};
            VkViewport vkvp[2];
            VkRect2D vksc[2];
            for (int e = 0; e < 2; ++e) {
                vkvp[e] = {float(vps[e].x), float(vps[e].y), float(vps[e].width),
                           float(vps[e].height), 0.f, 1.f};
                vksc[e] = {{vps[e].x, vps[e].y}, {vps[e].width, vps[e].height}};
            }
            vkCmdSetViewport(cmd, 0, 2, vkvp);
            vkCmdSetScissor(cmd, 0, 2, vksc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe_scene);
            const VkDescriptorSet ds = *dset, dss = *dset_shadow;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_scene, 0, 1,
                                    &ds, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_scene, 2, 1,
                                    &dss, 0, nullptr);
            for (std::size_t i = 0; i < sc.meshes.size(); ++i) {
                const std::uint32_t mat =
                    i + 1 == sc.meshes.size() ? deck.material_index
                                              : (*meshes)[i].material_index;
                const VkBuffer vb = sc.meshes[i]->vertex_buffer();
                const VkDeviceSize off = 0;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_scene, 1, 1,
                                        &dset_albedo[i], 0, nullptr);
                vkCmdPushConstants(cmd, layout_scene, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &mat);
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
                vkCmdBindIndexBuffer(cmd, sc.meshes[i]->index_buffer(), 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, sc.meshes[i]->index_count(), 2, 0, 0, 0); // 2 instances
            }
            vkCmdEndRendering(cmd);
            REQUIRE(sc.pres->end_pass(cmd));
            REQUIRE(sc.pres->submit(cmd));
        }
        return std::pair{sc.pres->capture_rgba8(), sc.pres->capture_depth_f32()};
    };

    trsf rig_out;
    const auto [px, dz] = render_stereo(0.7f, rig_out);
    const auto [px2, dz2] = render_stereo(0.7f, rig_out);
    CHECK(px == px2);
    CHECK(dz == dz2);

    const auto depth_at = [&](int x, int y) {
        return dz[static_cast<std::size_t>(y) * kRW + x];
    };
    // both viewports actually drew (gl_ViewportIndex = instance works)
    const float dl = depth_at(32, 32), dr = depth_at(64 + 32, 32);
    CHECK(dl > 0.f);
    CHECK(dr > 0.f);
    // fixation: hull at the focal distance -> both center rays see nearly
    // the same surface (depths agree)
    CHECK(std::abs(dl - dr) < 0.12f * std::max(dl, dr) + 1e-4f);

    // halves differ (off-axis asymmetry / parallax)
    bool differ = false;
    for (int y = 0; y < kH && !differ; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::size_t lo = (static_cast<std::size_t>(y) * kRW + x) * 4u;
            const std::size_t ro = lo + 64 * 4u;
            if (px[lo] != px[ro] || px[lo + 1] != px[ro + 1] || px[lo + 2] != px[ro + 2]) {
                differ = true;
                break;
            }
        }
    }
    CHECK(differ);

    // per-eye goldens
    auto half_rgba = [&](int half) {
        std::vector<std::uint8_t> rgba(kRW * kH * 4 / 2);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < 64; ++x) {
                const std::size_t s = (static_cast<std::size_t>(y) * kRW + half * 64 + x) * 4u;
                const std::size_t d = (static_cast<std::size_t>(y) * 64 + x) * 4u;
                rgba[d + 0] = px[s + 2];
                rgba[d + 1] = px[s + 1];
                rgba[d + 2] = px[s + 0];
                rgba[d + 3] = px[s + 3];
            }
        }
        return rgba;
    };
    const auto left = half_rgba(0), right = half_rgba(1);
    const auto gl_ = gfx_golden::check_golden("scene_stereo_l_64.png", left.data(), 64, kH, {});
    CHECK_MESSAGE(gl_.ok, gl_.message);
    const auto gr_ = gfx_golden::check_golden("scene_stereo_r_64.png", right.data(), 64, kH, {});
    CHECK_MESSAGE(gr_.ok, gr_.message);

    vkDeviceWaitIdle(dev->handle());
    vkDestroySampler(dev->handle(), sampler, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout_shadow, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout_scene, nullptr);
}
