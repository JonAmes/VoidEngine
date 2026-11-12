// 8.6 terrain art-direction spike (#17d) — THROWAWAY demo mode, per the
// phase-8 checklist: it exists to produce the two looks for the user's
// decision gate, pins nothing, and is expected to be superseded by the real
// 8.18 streaming mesh path. It is deliberately NOT a ctest test.
//
// The surface is the 7.2 ANALYTIC sampler verbatim — terrain_height and
// terrain_normal with authored stamps (pad / two mesas / crater) over a
// noise field — meshed CPU-side from the sampler itself (zero baked meshes,
// #29). Both looks share one body, one stamp stack, one camera, one sun;
// only tessellation and shading differ (that is the whole point of the
// comparison). See assets/shaders/terrain.{vert,frag} for the looks.
#include "ve/demo/terrain_mode.h"

#include <ve/core/cooked.h>
#include <ve/core/log.h>
#include <ve/core/math/mat4.h>
#include <ve/core/math/quat.h>
#include <ve/core/sim/terrain.h>
#include <ve/demo/png_out.h>
#include <ve/gfx/camera.h>
#include <ve/gfx/descriptor.h>
#include <ve/gfx/device.h>
#include <ve/gfx/mesh.h>
#include <ve/gfx/pipeline_cache.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>
#include <ve/gfx/wsi_presenter.h>
#include <ve/platform/window.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ve::demo {
namespace {

namespace sim = ve::core::sim;
using ve::core::math::cross;
using ve::core::math::dquat;
using ve::core::math::dvec3;
using ve::core::math::dot;
using ve::core::math::mat4f;
using ve::core::math::normalize;
using ve::core::math::quatf;
using ve::core::math::trsf;
using ve::core::math::vec3f;
using ve::core::math::vec4f;
using ve::gfx::cooked_mesh;
using ve::gfx::mesh_vertex;

// Cam UBO: byte-for-byte the hull/terrain shader contract (std140: two
// mat4 + one vec4 = 144 B).
struct TerrainCamUbo {
    mat4f view;
    mat4f proj;
    vec4f camPos;
};

// Push block: model mat4 + look vec4 = 80 B (both stages read it).
struct TerrainPush {
    mat4f model;
    vec4f look;
};

// The sky clear color. terrain.frag's kSky and the driver script's PNG
// cross-check MUST agree with these bytes, or the painted gate measures
// the wrong background (comment links, like the shader's).
constexpr float kSkyR = 0.53f, kSkyG = 0.67f, kSkyB = 0.82f;

// Rotation matrix (columns = camera axes in world) -> quat. Trace method,
// copied from the 7.14 fixture (same helper, hand-rolled).
dquat quat_from_axes(dvec3 x, dvec3 y, dvec3 z) {
    const double m00 = x.x, m01 = y.x, m02 = z.x;
    const double m10 = x.y, m11 = y.y, m12 = z.y;
    const double m20 = x.z, m21 = y.z, m22 = z.z;
    const double tr = m00 + m11 + m22;
    double w, qx, qy, qz;
    if (tr > 0.0) {
        const double s4 = std::sqrt(tr + 1.0) * 2.0;
        w = 0.25 * s4; qx = (m21 - m12) / s4; qy = (m02 - m20) / s4; qz = (m10 - m01) / s4;
    } else if (m00 > m11 && m00 > m22) {
        const double s4 = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        w = (m21 - m12) / s4; qx = 0.25 * s4; qy = (m01 + m10) / s4; qz = (m02 + m20) / s4;
    } else if (m11 > m22) {
        const double s4 = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        w = (m02 - m20) / s4; qx = (m01 + m10) / s4; qy = 0.25 * s4; qz = (m12 + m21) / s4;
    } else {
        const double s4 = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        w = (m10 - m01) / s4; qx = (m02 + m20) / s4; qy = (m12 + m21) / s4; qz = 0.25 * s4;
    }
    return normalize(dquat{qx, qy, qz, w});
}

// Spike body: small enough that the horizon curves sharply into frame from
// a 12 m eye — relief and stamps read as LAND, which is what the art
// decision needs to see. fp64 like all sim positions; the mesh bakes fp32.
constexpr double kRadius = 3000.0;

sim::terrain_def spike_def() {
    sim::terrain_def d{};
    d.seed = 0x8C6E9A2F13D7B405ULL;
    d.relief_m = 140.0;
    d.base_frequency = 4.0;
    d.octaves = 6;
    d.lacunarity = 2.2;
    d.gain = 0.5;
    return d;
}

// Stamp stack (#29c): the pad the eye stands on, two mesas ahead, one
// crater mid-distance. All centers within ~0.6 rad of the eye's dir, so
// every authored landform is inside the horizon ring.
std::vector<sim::terrain_stamp> spike_stamps() {
    const dvec3 c0 = normalize(dvec3{0.0, 1.0, 0.12});            // eye dir
    const dvec3 t = normalize(cross(c0, dvec3{1.0, 0.0, 0.0}));   // view tangent
    const dvec3 side = normalize(cross(t, c0));                   // view right
    auto ahead = [&](double fwd, double lat) {
        return normalize(c0 + t * fwd + side * lat);
    };
    std::vector<sim::terrain_stamp> s;
    s.push_back({sim::stamp_type::flatten, c0, 0.09, 30.0, 0.30});      // landing pad
    s.push_back({sim::stamp_type::mesa, ahead(0.36, 0.12), 0.16, 230.0, 0.45});
    s.push_back({sim::stamp_type::mesa, ahead(0.55, -0.25), 0.20, 180.0, 0.50});
    s.push_back({sim::stamp_type::crater, ahead(0.19, -0.10), 0.075, -35.0, 0.40});
    return s;
}

// Height encoding for uv.x: the sampler's reachable band [-320, +520] m
// (relief*noise-sum ~= 272, mesa level 230 on top, crater -35) mapped to
// 0..1. Continuous for look 1; quantized to 5 cards for look 0.
double hnorm_of(double h) {
    return std::clamp((h + 320.0) / 840.0, 0.0, 1.0);
}
double slope_of(const dvec3& n, const dvec3& dir) {
    return std::clamp((1.0 - dot(n, dir)) / 0.30, 0.0, 1.0);
}

// The six cube faces: dominant axis + sign; the two non-dominant axes in
// ascending index order carry u, v. Sampled at the SAME uniform grid on
// every face, a shared edge yields the identical SET of 3D directions from
// both faces (possibly reversed), so the mesh is watertight with no seam
// stitching — the sampler's cube-face chart (#29a) and this construction
// agree because both key off the dominant axis.
struct face_basis {
    int dom;
    double sign;
    int ax_u, ax_v;
};
std::vector<face_basis> spike_faces() {
    std::vector<face_basis> f;
    for (int dom = 0; dom < 3; ++dom) {
        for (int sgn = 0; sgn < 2; ++sgn) {
            face_basis b{dom, sgn == 0 ? 1.0 : -1.0, -1, -1};
            int k = 0;
            for (int a = 0; a < 3; ++a) {
                if (a == dom) continue;
                if (k == 0) b.ax_u = a; else b.ax_v = a;
                ++k;
            }
            f.push_back(b);
        }
    }
    return f;
}

dvec3 face_dir(const face_basis& b, double u, double v) {
    double c[3] = {0.0, 0.0, 0.0};
    c[b.dom] = b.sign;
    c[b.ax_u] = u;
    c[b.ax_v] = v;
    return normalize(dvec3{c[0], c[1], c[2]});
}

// Look 1 mesh: smooth, one vertex per grid point; uv = (continuous height,
// continuous slope). Realistic tessellation: 132 per face.
sim::terrain_def g_def;
std::span<const sim::terrain_stamp> g_stamps;
std::uint32_t g_verts_smooth = 0, g_tris_smooth = 0;
std::uint32_t g_verts_flat = 0, g_tris_flat = 0;

cooked_mesh build_smooth(std::uint32_t N) {
    cooked_mesh m;
    m.name = "terrain_smooth";
    const std::uint32_t row = N + 1u;
    for (const auto& b : spike_faces()) {
        const std::uint32_t base = static_cast<std::uint32_t>(m.vertices.size());
        for (std::uint32_t i = 0; i <= N; ++i) {
            for (std::uint32_t j = 0; j <= N; ++j) {
                const double u = 2.0 * static_cast<double>(i) / static_cast<double>(N) - 1.0;
                const double v = 2.0 * static_cast<double>(j) / static_cast<double>(N) - 1.0;
                const dvec3 d = face_dir(b, u, v);
                const double h = sim::terrain_height(g_def, g_stamps, d);
                const dvec3 n = sim::terrain_normal(g_def, kRadius, g_stamps, d);
                const dvec3 p = d * (kRadius + h);
                mesh_vertex mv{};
                mv.pos[0] = static_cast<float>(p.x);
                mv.pos[1] = static_cast<float>(p.y);
                mv.pos[2] = static_cast<float>(p.z);
                mv.nrm[0] = static_cast<float>(n.x);
                mv.nrm[1] = static_cast<float>(n.y);
                mv.nrm[2] = static_cast<float>(n.z);
                mv.uv[0] = static_cast<float>(hnorm_of(h));
                mv.uv[1] = static_cast<float>(slope_of(n, d));
                m.vertices.push_back(mv);
            }
        }
        for (std::uint32_t i = 0; i < N; ++i) {
            for (std::uint32_t j = 0; j < N; ++j) {
                const std::uint32_t a = base + i * row + j;
                const std::uint32_t bb = base + i * row + j + 1u;
                const std::uint32_t c = base + (i + 1u) * row + j;
                const std::uint32_t d = base + (i + 1u) * row + j + 1u;
                m.indices.insert(m.indices.end(), {a, bb, d, a, d, c});
            }
        }
    }
    return m;
}

// Look 0 mesh: low-poly, one vertex SET per triangle (no sharing), flat
// face normal, uv = band centers (quantized height card + slope band).
cooked_mesh build_flat(std::uint32_t N) {
    cooked_mesh m;
    m.name = "terrain_flat";
    for (const auto& b : spike_faces()) {
        for (std::uint32_t i = 0; i < N; ++i) {
            for (std::uint32_t j = 0; j < N; ++j) {
                auto at = [&](std::uint32_t ii, std::uint32_t jj) {
                    const double u = 2.0 * static_cast<double>(ii) / static_cast<double>(N) - 1.0;
                    const double v = 2.0 * static_cast<double>(jj) / static_cast<double>(N) - 1.0;
                    const dvec3 d = face_dir(b, u, v);
                    const double h = sim::terrain_height(g_def, g_stamps, d);
                    return d * (kRadius + h);
                };
                const dvec3 p00 = at(i, j);
                const dvec3 p01 = at(i, j + 1u);
                const dvec3 p10 = at(i + 1u, j);
                const dvec3 p11 = at(i + 1u, j + 1u);
                const dvec3 tri0[3] = {p00, p01, p11};
                const dvec3 tri1[3] = {p00, p11, p10};
                const dvec3* tris[2] = {tri0, tri1};
                for (const dvec3* tri : tris) {
                    const std::uint32_t v0 = static_cast<std::uint32_t>(m.vertices.size());
                    m.indices.insert(m.indices.end(), {v0, v0 + 1u, v0 + 2u});
                    dvec3 fn = normalize(cross(tri[1] - tri[0], tri[2] - tri[0]));
                    const dvec3 cen = (tri[0] + tri[1] + tri[2]) / 3.0;
                    const dvec3 cd = normalize(cen);
                    if (dot(fn, cd) < 0.0) fn = fn * -1.0; // outward
                    // quantized cards: 5 height bands, 3 slope bands, each
                    // encoded at the band CENTER so floor() decodes exactly.
                    const double h = sim::terrain_height(g_def, g_stamps, cd);
                    const double hn = std::clamp(std::floor(hnorm_of(h) * 5.0) / 5.0 + 0.1, 0.0, 1.0);
                    const double sl = std::clamp(1.0 - dot(fn, cd), 0.0, 1.0);
                    const double sn = std::clamp(std::floor((sl / 0.30) * 3.0) / 3.0 + 0.16666, 0.0, 1.0);
                    for (int k = 0; k < 3; ++k) {
                        mesh_vertex mv{};
                        mv.pos[0] = static_cast<float>(tri[k].x);
                        mv.pos[1] = static_cast<float>(tri[k].y);
                        mv.pos[2] = static_cast<float>(tri[k].z);
                        mv.nrm[0] = static_cast<float>(fn.x);
                        mv.nrm[1] = static_cast<float>(fn.y);
                        mv.nrm[2] = static_cast<float>(fn.z);
                        mv.uv[0] = static_cast<float>(hn);
                        mv.uv[1] = static_cast<float>(sn);
                        m.vertices.push_back(mv);
                    }
                }
            }
        }
    }
    return m;
}

double painted_frac(const std::vector<std::uint8_t>& c, bool swap_rb, std::uint32_t w,
                    std::uint32_t h) {
    // Sky bytes in swapchain order; a pixel is "painted" if it differs from
    // the sky by more than the UNORM rounding wiggle in any RGB channel.
    const std::uint8_t sky[3] = {static_cast<std::uint8_t>(kSkyR * 255.0f + 0.5f),
                                 static_cast<std::uint8_t>(kSkyG * 255.0f + 0.5f),
                                 static_cast<std::uint8_t>(kSkyB * 255.0f + 0.5f)};
    const std::uint8_t r0 = sky[0], g0 = sky[1], b0 = sky[2];
    std::size_t painted = 0;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* px = c.data() + i * 4u;
        const std::uint8_t r = swap_rb ? px[2] : px[0];
        const std::uint8_t g = px[1];
        const std::uint8_t b = swap_rb ? px[0] : px[2];
        const bool skypx = std::abs(int(r) - int(r0)) <= 9 && std::abs(int(g) - int(g0)) <= 9 &&
                           std::abs(int(b) - int(b0)) <= 9;
        if (!skypx) ++painted;
    }
    return n == 0u ? 0.0 : double(painted) / double(n);
}

double diff_frac(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                 std::uint32_t w, std::uint32_t h) {
    std::size_t differ = 0;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* pa = a.data() + i * 4u;
        const std::uint8_t* pb = b.data() + i * 4u;
        if (std::abs(int(pa[0]) - int(pb[0])) > 8 || std::abs(int(pa[1]) - int(pb[1])) > 8 ||
            std::abs(int(pa[2]) - int(pb[2])) > 8) {
            ++differ;
        }
    }
    return n == 0u ? 0.0 : double(differ) / double(n);
}

} // namespace

int run_terrain_mode(ve::platform::window& win, ve::gfx::device& dev,
                     ve::gfx::wsi_presenter& pres,
                     const std::filesystem::path& units_dir,
                     const std::filesystem::path& out_dir,
                     std::uint32_t frames_per_look) {
    const auto bad = [](const char* why) {
        VE_LOG_ERROR("terrain spike: %s", why);
        return 2;
    };

    // --- cooked terrain shaders (#13) --------------------------------------
    auto find_unit = [](const std::filesystem::path& dir, const std::string& prefix) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!ec && e.is_regular_file() &&
                e.path().filename().string().rfind(prefix, 0) == 0) {
                return e.path();
            }
        }
        return std::filesystem::path{};
    };
    const auto v_path = find_unit(units_dir, "terrain.vert");
    const auto f_path = find_unit(units_dir, "terrain.frag");
    if (v_path.empty() || f_path.empty()) {
        VE_LOG_ERROR("terrain spike: no cooked terrain.vert/.frag under %s",
                     units_dir.string().c_str());
        return 2;
    }
    ve::core::cooked::Unit vu, fu;
    std::string err;
    if (!ve::core::cooked::read_unit_file(v_path.string(), vu, &err) ||
        !ve::core::cooked::read_unit_file(f_path.string(), fu, &err)) {
        VE_LOG_ERROR("terrain spike: reading a cooked unit: %s", err.c_str());
        return 2;
    }
    auto vert = ve::gfx::shader_module::create(dev, vu.payload);
    auto frag = ve::gfx::shader_module::create(dev, fu.payload);
    if (!vert || !frag) {
        VE_LOG_ERROR("terrain spike: shader module: %s",
                     (vert ? frag : vert).error().c_str());
        return 2;
    }

    // --- the two meshes, straight from the analytic sampler -----------------
    g_def = spike_def();
    const auto stamps = spike_stamps();
    g_stamps = std::span<const sim::terrain_stamp>{stamps};
    const std::uint32_t N_styl = 24u, N_real = 132u;
    auto flat_cm = build_flat(N_styl);
    auto smooth_cm = build_smooth(N_real);
    g_verts_flat = static_cast<std::uint32_t>(flat_cm.vertices.size());
    g_tris_flat = static_cast<std::uint32_t>(flat_cm.indices.size()) / 3u;
    g_verts_smooth = static_cast<std::uint32_t>(smooth_cm.vertices.size());
    g_tris_smooth = static_cast<std::uint32_t>(smooth_cm.indices.size()) / 3u;
    VE_LOG_INFO("terrain spike: stylized %u v / %u t, realistic %u v / %u t", g_verts_flat,
                g_tris_flat, g_verts_smooth, g_tris_smooth);
    auto gm_styl = ve::gfx::gpu_mesh::upload(dev, flat_cm);
    auto gm_real = ve::gfx::gpu_mesh::upload(dev, smooth_cm);
    if (!gm_styl || !gm_real) {
        VE_LOG_ERROR("terrain spike: mesh upload: %s",
                     (gm_styl ? gm_real : gm_styl).error().c_str());
        return 2;
    }

    // --- UBO, set layout, 80-byte push, pipeline ----------------------------
    auto cam_buf = ve::gfx::uniform_buffer::create(dev, sizeof(TerrainCamUbo));
    if (!cam_buf) {
        VE_LOG_ERROR("terrain spike: camera UBO: %s", cam_buf.error().c_str());
        return 2;
    }
    const ve::gfx::uniform_set_layout::ubo_binding bind0{
        0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    auto set_layout = ve::gfx::uniform_set_layout::create(
        dev, std::span<const ve::gfx::uniform_set_layout::ubo_binding>{&bind0, 1});
    if (!set_layout) {
        VE_LOG_ERROR("terrain spike: set layout: %s", set_layout.error().c_str());
        return 2;
    }
    const VkPushConstantRange push{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = static_cast<std::uint32_t>(sizeof(TerrainPush))};
    const VkDescriptorSetLayout sets[1] = {set_layout->handle()};
    const VkPipelineLayoutCreateInfo lci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = sets,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev.handle(), &lci, nullptr, &layout) != VK_SUCCESS) {
        return bad("pipeline layout");
    }
    ve::gfx::pipeline_cache cache(dev);
    ve::gfx::pipeline_desc bd{};
    bd.layout = layout;
    bd.stages = {{VK_SHADER_STAGE_VERTEX_BIT, vert->handle()},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, frag->handle()}};
    bd.bindings = {{0, sizeof(ve::gfx::mesh_vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    bd.attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24},
    };
    bd.cull = VK_CULL_MODE_NONE; // spike: winding-free by construction cost
    bd.depth_format = VK_FORMAT_D32_SFLOAT;
    bd.depth_test = true;
    bd.depth_write = true;
    bd.color_formats = {pres.color_format()};
    bd.blends = {ve::gfx::blend_attachment_state{}};
    bd.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);
    auto pipe = cache.get(bd);
    if (!pipe) {
        VE_LOG_ERROR("terrain spike: pipeline: %s", pipe.error().c_str());
        vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
        return 2;
    }
    auto pool = ve::gfx::descriptor_pool::create(dev, 4, 8);
    if (!pool) {
        vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
        return bad("descriptor pool");
    }
    auto dset = pool->allocate(set_layout->handle());
    if (!dset) {
        vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
        return bad("descriptor set");
    }
    pool->write_uniform(*dset, 0, cam_buf->handle(), cam_buf->size());

    // --- the fixed eye (#17d: both looks MUST see the identical frame) -----
    const dvec3 c0 = normalize(dvec3{0.0, 1.0, 0.12});
    const dvec3 t = normalize(cross(c0, dvec3{1.0, 0.0, 0.0}));
    const double h_pad = sim::terrain_height(g_def, g_stamps, c0);
    const dvec3 eye_d = c0 * (kRadius + h_pad + 12.0);
    const dvec3 fwd = normalize(t - 0.04 * c0); // 2.3 deg below horizontal
    const dvec3 z_cam = fwd * -1.0;
    const dvec3 x_cam = normalize(cross(c0, z_cam));
    const dvec3 y_cam = cross(z_cam, x_cam);
    const dquat att = quat_from_axes(x_cam, y_cam, z_cam);
    const quatf att_f{float(att.x), float(att.y), float(att.z), float(att.w)};
    const vec3f eye_f{float(eye_d.x), float(eye_d.y), float(eye_d.z)};

    ve::gfx::camera_rig rig;
    ve::gfx::eye_def mono{};
    mono.frust = ve::gfx::symmetric_frustum(ve::core::math::deg_to_rad(62.0f),
                                            float(pres.width()) / float(pres.height()), 0.3f,
                                            2500.0f);
    mono.frust.depth = ve::core::math::depth_range::reversed_1_to_0;
    mono.view = {0, 0, pres.width(), pres.height()};
    rig.add_eye(mono);

    const bool swap_rb = pres.color_format() == VK_FORMAT_B8G8R8A8_UNORM ||
                         pres.color_format() == VK_FORMAT_B8G8R8A8_SRGB;

    // --- frame loop: look 0 frames, capture; look 1 frames, capture --------
    const mat4f identity{};
    std::vector<std::uint8_t> cap[2];
    std::vector<double> disp_ms;
    bool closed = false;
    for (int look = 0; look < 2; ++look) {
        const ve::gfx::gpu_mesh& gm = look == 0 ? *gm_styl : *gm_real;
        for (std::uint32_t f = 0; f < frames_per_look; ++f) {
            win.poll_events();
            if (win.should_close()) {
                closed = true;
                break;
            }
            rig.set_rig(trsf{eye_f, att_f, {1.f, 1.f, 1.f}});
            const auto eyes = rig.compute();
            if (eyes.size() != 1u) {
                vkDestroyPipeline(dev.handle(), *pipe, nullptr);
                vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
                return bad("rig did not produce exactly one eye");
            }
            if (auto idx = pres.acquire(); !idx) {
                vkDestroyPipeline(dev.handle(), *pipe, nullptr);
                vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
                return bad("acquire: no swapchain image");
            }
            TerrainCamUbo cu{};
            cu.view = eyes[0].view;
            cu.proj = eyes[0].proj;
            cu.camPos = vec4f{eye_f.x, eye_f.y, eye_f.z, 1.f};
            cam_buf->write(&cu, sizeof(cu));

            VkCommandBuffer cmd = pres.begin_command();
            if (cmd == VK_NULL_HANDLE || !pres.begin_pass(cmd)) {
                vkDestroyPipeline(dev.handle(), *pipe, nullptr);
                vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
                return bad("command buffer / begin_pass");
            }
            VkRenderingAttachmentInfo catt{};
            catt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            catt.imageView = pres.color_view();
            catt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            catt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            catt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            catt.clearValue.color.float32[0] = kSkyR;
            catt.clearValue.color.float32[1] = kSkyG;
            catt.clearValue.color.float32[2] = kSkyB;
            catt.clearValue.color.float32[3] = 1.f;
            VkRenderingAttachmentInfo datt{};
            datt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            datt.imageView = pres.depth_view();
            datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            datt.clearValue.depthStencil.depth = 0.0f; // reversed-Z far
            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea = {{0, 0}, {pres.width(), pres.height()}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &catt;
            ri.pDepthAttachment = &datt;
            vkCmdBeginRendering(cmd, &ri);
            const VkViewport vpt{0.0f, 0.0f, float(pres.width()), float(pres.height()), 0.0f, 1.0f};
            const VkRect2D sc{{0, 0}, {pres.width(), pres.height()}};
            vkCmdSetViewport(cmd, 0, 1, &vpt);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &*dset, 0,
                                    nullptr);
            const TerrainPush tp{identity, vec4f{float(look), 0.f, 0.f, 0.f}};
            vkCmdPushConstants(cmd, layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(tp), &tp);
            const VkBuffer vb = gm.vertex_buffer();
            const VkDeviceSize o = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &o);
            vkCmdBindIndexBuffer(cmd, gm.index_buffer(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, gm.index_count(), 1, 0, 0, 0);
            vkCmdEndRendering(cmd);
            const auto t0 = std::chrono::steady_clock::now();
            if (!pres.end_pass(cmd) || !pres.present(cmd)) {
                vkDestroyPipeline(dev.handle(), *pipe, nullptr);
                vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
                return bad("end_pass / present");
            }
            const auto t1 = std::chrono::steady_clock::now();
            disp_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        if (closed) break;
        cap[look] = pres.capture_rgba8(); // between frames, never mid-pass
    }
    vkDestroyPipeline(dev.handle(), *pipe, nullptr);
    vkDestroyPipelineLayout(dev.handle(), layout, nullptr);

    if (closed || cap[0].empty() || cap[1].empty()) {
        VE_LOG_ERROR("terrain spike: a capture came back empty (window closed?)");
        return 4;
    }

    const std::uint32_t w = pres.width(), h = pres.height();
    const double painted[2] = {painted_frac(cap[0], swap_rb, w, h),
                               painted_frac(cap[1], swap_rb, w, h)};
    const double diff = diff_frac(cap[0], cap[1], w, h);
    std::vector<double> sorted = disp_ms;
    std::sort(sorted.begin(), sorted.end());
    const double median_ms = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];

    bool png_ok = true;
    if (!out_dir.empty()) {
        std::filesystem::create_directories(out_dir);
        png_ok =
            write_png_rgba8((out_dir / "terrain_stylized.png").string().c_str(), w, h,
                            cap[0].data(), swap_rb) &&
            write_png_rgba8((out_dir / "terrain_realistic.png").string().c_str(), w, h,
                            cap[1].data(), swap_rb);
        std::ofstream sum(out_dir / "terrain_summary.txt");
        sum << "8.6 terrain art-direction spike (#17d) - THROWAWAY; the look "
               "decision belongs to the user.\n"
            << "body_radius_m=3000 relief_m=140 octaves=6 base_frequency=4.0 "
               "lacunarity=2.2 gain=0.5 stamps=4 (pad, 2 mesas, crater)\n"
            << "grid_stylized=" << N_styl << " verts=" << g_verts_flat
            << " tris=" << g_tris_flat << "\n"
            << "grid_realistic=" << N_real << " verts=" << g_verts_smooth
            << " tris=" << g_tris_smooth << "\n"
            << "painted_stylized=" << painted[0] << " painted_realistic=" << painted[1]
            << " look_diff=" << diff << "\n"
            << "sky_rgb=135,171,209 size=" << w << "x" << h << "\n"
            << "frames_per_look=" << frames_per_look
            << " median_present_ms=" << median_ms << "\n"
            << "gates: painted>=0.30 each, look_diff>=0.05, non-empty captures -> "
            << ((painted[0] >= 0.30 && painted[1] >= 0.30 && diff >= 0.05) ? "PASS" : "FAIL")
            << "\n";
    }

    VE_LOG_INFO("terrain spike: painted stylized=%.3f realistic=%.3f diff=%.3f median %.1f ms",
                painted[0], painted[1], diff, median_ms);
    if (painted[0] < 0.30 || painted[1] < 0.30) {
        VE_LOG_ERROR("terrain spike: a look painted under the 0.30 floor (exit 4)");
        return 4;
    }
    if (diff < 0.05) {
        VE_LOG_ERROR("terrain spike: the two looks are visually identical (exit 4) - "
                     "the look switch is not reaching the image");
        return 4;
    }
    if (!out_dir.empty() && !png_ok) {
        VE_LOG_ERROR("terrain spike: a PNG write failed (exit 4)");
        return 4;
    }
    return 0;
}

} // namespace ve::demo
