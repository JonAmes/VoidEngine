// test: gfx_flight_visible — checklist 7.15 done-when (c) renderer leg.
// The 7.14 scripted flight's SIM poses (tests/core/flight_scenario.h, the
// SAME run the core harness pins) drive the Phase 3 scene path: cooked
// mesh by content id (#13) + per-frame MODEL push-constant transform +
// camera_rig eye. Pins:
//   * hull painted at frame center + painted-pixel floor for sampled
//     frames across takeoff / climb / cruise / glide / approach / post-
//     capture pad coast (a black frame or a broken transform dies);
//   * SILHOUETTE MASK DELTA between consecutive in-flight frames — the
//     sim attitude genuinely changes what the hull looks like (a frozen
//     transform dies);
//   * render determinism: the fixed cruise frame re-renders byte-identical;
//   * golden flight_hull_cruise.png byte-exact on lavapipe (Tier 2).
//
// Camera contract: one mono eye through camera_rig (no backbuffer/aspect
// assumptions). The eye chases the ship at a CONSTANT pad-frame offset with
// constant orientation, so hull attitude swings drive the silhouette.
#include <doctest/doctest.h>
#include <ve/core/math/mat4.h>
#include <ve/core/math/projection.h>
#include <ve/core/math/quat.h>
#include <ve/gfx/camera.h>
#include <ve/gfx/descriptor.h>
#include <ve/gfx/mesh.h>
#include <ve/gfx/pipeline_cache.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>

#include "gfx_golden.h"
#include "gfx_test_env.h"

#include "../core/flight_scenario.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <ve/cook/cook.h>

namespace {
using namespace ve::core::math;
using namespace ve::gfx;

constexpr int kW = 128, kH = 128;
constexpr float kFovY = deg_to_rad(60.0f);
constexpr float kNear = 0.5f, kFar = 120.f;

std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_fv_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}

// ---- scene-path shaders for this test (cooked at test start) -------------
// Model matrix arrives as a 64-byte push constant: the per-frame transform
// the sim pose drives. Camera state in set 0 (same UBO role as 3.15, mono
// single-slot). Simple key-light shading (3.10 convention: dir = toward
// light) so different hull faces shade differently -> rich silhouette.
constexpr const char* kHullVert = R"GLSL(#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec2 uvin;
layout(set = 0, binding = 0, std140) uniform Cam {
    mat4 view; mat4 proj; vec4 camPos;
} cam;
layout(push_constant) uniform Model { mat4 model; } mdl;
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec3 wnorm;
void main() {
    vec4 wp = mdl.model * vec4(pos, 1.0);
    wnorm = mat3(mdl.model) * nrm;   // rotation-only model: mat3 is exact
    gl_Position = cam.proj * (cam.view * wp);
}
)GLSL";

constexpr const char* kHullFrag = R"GLSL(#version 450
layout(location = 0) in vec3 wnorm;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 N = normalize(wnorm);
    vec3 L = normalize(vec3(-0.45, 0.85, 0.28));
    float ndl = max(dot(N, L), 0.0);
    vec3 bcol = vec3(0.72, 0.73, 0.76);
    vec3 c = bcol * (0.30 + 0.85 * ndl);
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)GLSL";

void cook_hull_assets(const std::string& src, const std::string& out) {
    const auto p = std::filesystem::path(src);
    { std::ofstream f(p / "hull.vert", std::ios::trunc); f << kHullVert; }
    { std::ofstream f(p / "hull.frag", std::ios::trunc); f << kHullFrag; }
    const std::string ship = std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb";
    REQUIRE_MESSAGE(std::filesystem::exists(ship), "missing test_ship.glb sample");
    std::filesystem::copy_file(ship, p / "test_ship.glb");
    REQUIRE_MESSAGE(ve::cook::cook_incremental(src, out).ok, "cook failed");
}

} // namespace

TEST_SUITE_BEGIN("gfx_flight_visible");

TEST_CASE("flight_hull_visible_silhouette_moves") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 flight-visibility test skipped");
        return;
    }

    // 1. THE flight: same scripted run as the core 7.14 harness -----------
    const auto A = ve_test::flight::run_scenario();
    CHECK(A.ok);
    REQUIRE(A.hashes.size() == ve_test::flight::kT_END);
    REQUIRE(A.engage_tick > 0);
    // Sampled frames straddle the capture: 14500 is pre-capture descent,
    // 15500/19000 are the post-capture pad coast. Load-bearing window.
    CHECK((A.engage_tick > 14'500 && A.engage_tick < 15'500));

    // 2. cooked hull by content id + device + shaders + gpu meshes -------
    const auto src = tmp_root("src");
    const auto out = tmp_root("out");
    cook_hull_assets(src, out);
    auto meshes = load_meshes_by_id(out, "test_ship.glb");
    REQUIRE_MESSAGE(meshes.has_value(), meshes.error());
    CHECK(meshes->size() >= 1u);

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
    auto h_v = cook_load("hull.vert.ck");
    auto h_f = cook_load("hull.frag.ck");

    std::vector<std::unique_ptr<gpu_mesh>> gm;
    for (const auto& m : *meshes) {
        auto g = gpu_mesh::upload(*dev, m);
        REQUIRE_MESSAGE(g.has_value(), g.error());
        gm.push_back(std::make_unique<gpu_mesh>(std::move(*g)));
    }

    auto pres = offscreen_presenter::create(*dev, kW, kH);
    REQUIRE(pres.has_value());

    // 3. one UBO (Cam), 64-byte model push constant ----------------------
    struct CamUboFill { mat4f view; mat4f proj; vec4f camPos; };
    auto cam_buf = uniform_buffer::create(*dev, sizeof(CamUboFill));
    REQUIRE(cam_buf.has_value());

    const uniform_set_layout::ubo_binding bind0{
        0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    auto set_layout = uniform_set_layout::create(
        *dev, std::span<const uniform_set_layout::ubo_binding>{&bind0, 1});
    REQUIRE(set_layout.has_value());

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = 64; // mat4 model
    const VkDescriptorSetLayout sets[1] = {set_layout->handle()};
    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.setLayoutCount = 1;
    lci.pSetLayouts = sets;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &push;
    VkPipelineLayout layout_hull = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lci, nullptr, &layout_hull) == VK_SUCCESS);

    auto pool = descriptor_pool::create(*dev, 4, 8);
    REQUIRE(pool.has_value());
    auto dset = pool->allocate(set_layout->handle());
    REQUIRE(dset.has_value());
    pool->write_uniform(*dset, 0, cam_buf->handle(), cam_buf->size());

    pipeline_cache cache(*dev);
    pipeline_desc bd{};
    bd.layout = layout_hull;
    bd.stages = {{VK_SHADER_STAGE_VERTEX_BIT, h_v->handle()},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, h_f->handle()}};
    bd.bindings = {{0, sizeof(mesh_vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    bd.attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},   // pos (MUST be R32G32B32)
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},  // nrm
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},     // uv
    };
    bd.cull = VK_CULL_MODE_NONE;
    bd.depth_format = VK_FORMAT_D32_SFLOAT;
    bd.depth_test = true;
    bd.depth_write = true;
    bd.color_formats = {VK_FORMAT_B8G8R8A8_UNORM};
    bd.blends = {blend_attachment_state{}};
    bd.viewport_count = 1;
    bd.scissor_count = 1;
    bd.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);
    auto pipe = cache.get(bd);
    REQUIRE_MESSAGE(pipe.has_value(), pipe.error());

    // 4. camera: mono eye, CONSTANT orientation, chase offset -------------
    // World up at the pad = radial (local_dir +X). The chase offset keeps a
    // 0.38 radial component, so view axis vs up never degenerates.
    const vec3f up{1.f, 0.f, 0.f};
    const vec3f off{10.f, 8.f, -10.f};
    const vec3f z = normalize(off);
    const vec3f y = normalize(up - z * dot(z, up));
    const vec3f x = cross(y, z);
    const dquat cam_att_d = ve_test::flight::quat_from_axes(
        {x.x, x.y, x.z}, {y.x, y.y, y.z}, {z.x, z.y, z.z});
    const quatf cam_att{float(cam_att_d.x), float(cam_att_d.y),
                        float(cam_att_d.z), float(cam_att_d.w)};

    camera_rig rig;
    eye_def mono{};
    mono.frust = symmetric_frustum(kFovY, 1.f, kNear, kFar);
    mono.frust.depth = depth_range::reversed_1_to_0;
    mono.view = {0, 0, kW, kH};
    rig.add_eye(mono);

    // 5. one frame: sim pose -> model push + chase eye; returns capture ---
    struct Caps { std::vector<std::uint8_t> color; std::vector<float> depth; };
    const ve::core::math::dvec3 anchor = A.pad_pos0;
    auto render_frame = [&](const ve::core::sim::rigid_state& s) -> Caps {
        const vec3f rel{float(s.pos.x - anchor.x), float(s.pos.y - anchor.y),
                        float(s.pos.z - anchor.z)};
        const quatf att{float(s.att.x), float(s.att.y), float(s.att.z), float(s.att.w)};
        const mat4f model = trs_compose(trsf{rel, att, {1.f, 1.f, 1.f}});
        const vec3f eye = rel + off;

        rig.set_rig(trsf{eye, cam_att, {1.f, 1.f, 1.f}});
        const auto frames = rig.compute();
        REQUIRE(frames.size() == 1u);
        CamUboFill cu{};
        cu.view = frames[0].view;
        cu.proj = frames[0].proj;
        cu.camPos = vec4f{eye.x, eye.y, eye.z, 1.f};
        cam_buf->write(&cu, sizeof(cu));

        VkCommandBuffer cmd = pres->begin_command();
        REQUIRE(cmd != VK_NULL_HANDLE);
        REQUIRE(pres->begin_pass(cmd));
        VkRenderingAttachmentInfo catt{};
        catt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        catt.imageView = pres->color_view();
        catt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        catt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        catt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        catt.clearValue.color.float32[0] = 1.f;  // white bg (B8G8R8A8)
        catt.clearValue.color.float32[1] = 1.f;
        catt.clearValue.color.float32[2] = 1.f;
        catt.clearValue.color.float32[3] = 1.f;
        VkRenderingAttachmentInfo datt{};
        datt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        datt.imageView = pres->depth_view();
        datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        datt.clearValue.depthStencil.depth = 0.0f;  // reversed-Z far
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {kW, kH}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &catt;
        ri.pDepthAttachment = &datt;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport v{0.f, 0.f, float(kW), float(kH), 0.f, 1.f};
        const VkRect2D sc{{0, 0}, {kW, kH}};
        vkCmdSetViewport(cmd, 0, 1, &v);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe);
        const VkDescriptorSet ds = *dset;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_hull, 0, 1,
                                &ds, 0, nullptr);
        for (auto& m : gm) {
            const VkBuffer vb = m->vertex_buffer();
            const VkDeviceSize o = 0;
            vkCmdPushConstants(cmd, layout_hull, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &model);
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &o);
            vkCmdBindIndexBuffer(cmd, m->index_buffer(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, m->index_count(), 1, 0, 0, 0);
        }
        vkCmdEndRendering(cmd);
        REQUIRE(pres->end_pass(cmd));
        REQUIRE(pres->submit(cmd));
        return Caps{pres->capture_rgba8(), pres->capture_depth_f32()};
    };

    // mask helpers: painted = any channel != white clear
    const auto painted_mask = [](const std::vector<std::uint8_t>& px) {
        std::vector<bool> m(px.size() / 4u);
        std::size_t n = 0;
        for (std::size_t i = 0; i < m.size(); ++i) {
            const std::size_t o = i * 4u;
            m[i] = px[o] != 255 || px[o + 1] != 255 || px[o + 2] != 255;
            n += m[i];
        }
        return std::pair{m, n};
    };
    const auto mask_xor = [](const std::vector<bool>& a, const std::vector<bool>& b) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < a.size(); ++i) n += (a[i] != b[i]);
        return n;
    };

    // 6. sampled frames across the flight ---------------------------------
    // pre-capture pairs drive the mask-delta pins; 30 is pad rest, 14500
    // final descent, 15500/19000 pad coast after capture.
    static constexpr std::array<ve::core::sim::tick_t, 9> kFrameTicks{
        30, 1'500, 2'400, 3'600, 4'600, 5'800, 6'600, 10'000, 14'500};
    struct Frame { std::vector<bool> mask; std::size_t count; };
    std::vector<Frame> frames;
    std::string diff_dump;
    for (const auto t : kFrameTicks) {
        const auto caps = render_frame(A.poses[std::size_t(t)]);
        const auto [m, n] = painted_mask(caps.color);
        // center ray hits the hull (chase cam keeps it centered)
        const std::size_t co = (std::size_t(kH / 2) * kW + kW / 2) * 4u;
        const bool center_painted = caps.color[co] != 255 || caps.color[co + 1] != 255 ||
                                    caps.color[co + 2] != 255;
        const bool center_depth = caps.depth[std::size_t(kH / 2) * kW + kW / 2] > 0.f;
        CHECK_MESSAGE(center_painted, "tick " << t << ": hull not at frame center");
        CHECK_MESSAGE(center_depth, "tick " << t << ": no depth at center");
        CHECK_MESSAGE(n >= 150, "tick " << t << ": painted pixels " << n);
        frames.push_back(Frame{m, n});
    }
    // silhouette genuinely MOVES between consecutive in-flight poses
    // (attitude-driven; pairs start after climb, where the ship pitched
    // from vertical-thrust pose to forward-flight pose).
    for (std::size_t i = 1; i < frames.size(); ++i) {
        const std::size_t d = mask_xor(frames[i - 1].mask, frames[i].mask);
        diff_dump += "t" + std::to_string(kFrameTicks[i - 1]) + "->t" +
                     std::to_string(kFrameTicks[i]) + "=" + std::to_string(d) + " ";
    }
    MESSAGE("mask deltas: ", diff_dump);
    // Frozen/broken transforms die on the min pin (all deltas collapse to
    // ~0); the strong-delta count pins that the sim attitude genuinely
    // swings the silhouette (stall excursion, glide, approach pairs).
    // Thresholds sit between the measured values and failure modes:
    // near-constant-attitude pairs (vertical thrust phase) measure 68-85,
    // attitude-swing pairs 143-686.
    std::size_t min_delta = ~0ull, strong = 0;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        const std::size_t d = mask_xor(frames[i - 1].mask, frames[i].mask);
        min_delta = std::min(min_delta, d);
        strong += (d >= 300);
    }
    CHECK(min_delta >= 50);
    CHECK(strong >= 4);

    // post-capture pad-coast frames: still visible (same scene path, no
    // continuous hold — the snapped stand just renders at its sim pose)
    for (const ve::core::sim::tick_t t : {ve::core::sim::tick_t{15'500}, ve::core::sim::tick_t{19'000}}) {
        const auto caps = render_frame(A.poses[std::size_t(t)]);
        const auto [m, n] = painted_mask(caps.color);
        (void)m;
        CHECK_MESSAGE(n >= 150, "post-capture tick " << t << ": painted " << n);
    }

    // 7. render determinism + fixed-pose golden ----------------------------
    const auto g1 = render_frame(A.poses[3'600]);
    const auto g2 = render_frame(A.poses[3'600]);
    CHECK(g1.color == g2.color);
    CHECK(g1.depth == g2.depth);

    std::vector<std::uint8_t> rgba(g1.color.size());
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = g1.color[i + 2];  // B8G8R8A8 -> RGBA
        rgba[i + 1] = g1.color[i + 1];
        rgba[i + 2] = g1.color[i + 0];
        rgba[i + 3] = g1.color[i + 3];
    }
    const auto gv = gfx_golden::check_golden("flight_hull_cruise.png", rgba.data(), kW, kH, {});
    CHECK_MESSAGE(gv.ok, gv.message);

    vkDeviceWaitIdle(dev->handle());
    vkDestroyPipelineLayout(dev->handle(), layout_hull, nullptr);
}

TEST_SUITE_END();
