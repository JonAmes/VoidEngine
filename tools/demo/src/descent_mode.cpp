// 8.18 descent segment (#29 streaming + #49 hybrid look, HUMAN-EYES half of
// the checklist box): the terrain_descent_golden ctest test pins the same
// streaming path at four fixed poses; this mode PLAYS it — the camera
// descends on the +X nadir axis from 20 km to ~1.5 km, and every frame
// re-selects quadtree chunks by the screen-size rule and rebuilds the mesh
// from the shared sampler (sim::gen_chunk, 8.17). The log-zoom reversed-Z
// spine (8.10) carries 20 km -> 1.5 m of depth in ONE projection, so no
// near/far refit, no Z-fighting, no pops — the frame-delta proof lives in
// the ctest test; this is what a person watches.
//
// NOT a ctest test (a window by design; the Xvfb runner is
// tools/dev/descent_run.sh, which commits its PNGs as evidence).
// Self-attests: exit 0 = every frame painted >= 0.90 non-blue (the blue
// clear shows any chunk-swap hole as a blue pixel, same contract as the
// golden test); 2 = setup refusal; 4 = paint/PNG gate failure.
#include "ve/demo/descent_mode.h"

#include <ve/core/cooked.h>
#include <ve/core/log.h>
#include <ve/core/math/mat4.h>
#include <ve/core/math/projection.h>
#include <ve/core/sim/terrain.h>
#include <ve/core/sim/terrain_stream.h>
#include <ve/demo/png_out.h>
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ve::demo {
namespace {

namespace sim = ve::core::sim;
namespace math = ve::core::math;
using math::dvec3;
using math::mat4f;
using ve::gfx::cooked_mesh;
using ve::gfx::mesh_vertex;

// Same machinery as the golden test, deliberately DIFFERENT terrain data
// (documented deviation): the twin's field (300 m relief at ~1900 km
// wavelengths, 150 m crater at 720 km) is a 0.05% slope plane at every
// descent altitude — locally invisible, so human-eyes frames would be flat
// fills no matter how the pipeline renders them. The descent pass uses a
// DRAMATIZED world: 8 km relief at ~190 km wavelengths (octaves to ~12 km),
// a 12 km crater 1 km deep. The pinned goldens are untouched; the runner
// summary states the deviation.
constexpr double kR = 6.0e6;
constexpr double kN = 0.5, kF = 1.0e12;
constexpr int kLmax = 12;  // demo-only: 4.7 km cells so the low-altitude
//                     frames resolve terrain; goldens stay pinned at 9
constexpr double kPi = 3.14159265358979323846;

// Crater center_dir (shared by relief_stamps() and the camera axis).
const dvec3 kCraterC = math::normalize(dvec3{1.0, 0.1, 0.0});

sim::terrain_def relief_def() {
    sim::terrain_def def;
    def.seed = 0xB17E5EEDULL;
    def.relief_m = 8000.0;  // demo dramatization (see header note)
    def.base_frequency = 300.0;
    def.octaves = 5;
    def.lacunarity = 2.0;
    def.gain = 0.5;
    return def;
}
std::array<sim::terrain_stamp, 1> relief_stamps() {
    return {sim::terrain_stamp{
        .type = sim::stamp_type::crater,
        .center_dir = kCraterC,
        .radius_rad = 0.02,   // ~120 km crater, ~1 km deep
        .level_m = -1000.0,
        .blend = 0.3}};
}
std::array<sim::surface_band, 2> relief_bands() {
    return {sim::surface_band{-100000.0, -200.0, 0.0, kPi},
            sim::surface_band{-200.0, 100000.0, 0.0, kPi}};
}

// Chunk selection: the golden test's rule verbatim (leaf when the node's
// half-size subtends <= 1/16 of its distance, quadtree cap kLmax; screen-
// margin cull at ndc +-1.6; edge deltas from outward probes).
struct sel_node {
    sim::chunk_key key{};
    std::array<std::int8_t, 4> delta = {0, 0, 0, 0};
};

// Camera axis = a point ON THE CRATER SLOPE, 0.105 rad off the crater
// center (the blend zone is 0.09..0.12 rad). Landing dead-center gives a
// flat bowl floor = featureless frame no matter the octaves; landing on the
// slope puts the rising rim + the added high-octave relief in the descent
// view, so parallax and the height ramp have something to work on.
const dvec3 kAxis = [] {
    const double a = 0.017;  // slope zone of the 0.02 rad crater
    return math::normalize(std::cos(a) * kCraterC +
                           std::sin(a) * dvec3{0.0, 0.0, 1.0});
}();

std::vector<sel_node> select_chunks(double alt) {
    const dvec3 cam = (kR + alt) * kAxis;
    // Screen basis EXACTLY as the view matrix builds it (look_at_rh, up=+Z):
    // f=forward, s=right, u=up. The cull below projects quad corners into
    // real view ndc — the golden test's axis-form shortcut assumes a camera
    // on the face normal and would cull this tilted axis's nadir quads.
    const dvec3 f = -kAxis;
    const dvec3 s = math::normalize(math::cross(f, dvec3{0.0, 0.0, 1.0}));
    const dvec3 u = math::cross(-f, s);
    std::vector<sel_node> out;
    std::function<void(int, int, int)> walk = [&](int L, int x, int y) {
        const double cell = 2.0 / static_cast<double>(1 << L);
        const double u0 = -1.0 + x * cell, v0 = -1.0 + y * cell;
        const double uc = u0 + cell * 0.5, vc = v0 + cell * 0.5;
        const double hs = kR * cell * 0.5;
        {
            double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
            for (const double cu : {u0, u0 + cell})
                for (const double cv : {v0, v0 + cell}) {
                    const dvec3 dir = math::normalize(dvec3{1.0, cu, cv});
                    const dvec3 rel = kR * dir - cam;
                    const double w = math::dot(rel, -f);  // > 0 above surface
                    const double nx = math::dot(rel, s) / w;
                    const double ny = math::dot(rel, u) / w;
                    minx = std::min(minx, nx); maxx = std::max(maxx, nx);
                    miny = std::min(miny, ny); maxy = std::max(maxy, ny);
                }
            if (maxx < -1.6 || minx > 1.6 || maxy < -1.6 || miny > 1.6) return;
        }
        const dvec3 dir = math::normalize(dvec3{1.0, uc, vc});
        const double dc = math::length(cam - kR * dir);
        if (L == kLmax || hs <= dc / 16.0) {
            out.push_back({sim::chunk_key{0, static_cast<std::uint8_t>(L), x, y}, {}});
            return;
        }
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) walk(L + 1, 2 * x + dx, 2 * y + dy);
    };
    walk(0, 0, 0);
    std::sort(out.begin(), out.end(), [](const sel_node& a, const sel_node& b) {
        if (a.key.level != b.key.level) return a.key.level < b.key.level;
        if (a.key.x != b.key.x) return a.key.x < b.key.x;
        return a.key.y < b.key.y;
    });
    auto contains = [&](int M, double pu, double pv, sel_node* found) {
        const double cellM = 2.0 / static_cast<double>(1 << M);
        const int xm = static_cast<int>(std::floor((pu + 1.0) / cellM));
        const int ym = static_cast<int>(std::floor((pv + 1.0) / cellM));
        if (xm < 0 || xm >= (1 << M) || ym < 0 || ym >= (1 << M)) return false;
        for (const auto& n : out) {
            if (n.key.level == M && n.key.x == xm && n.key.y == ym) {
                if (found) *found = n;
                return true;
            }
        }
        return false;
    };
    for (auto& n : out) {
        const int L = n.key.level;
        const double cell = 2.0 / static_cast<double>(1 << L);
        const double u0 = -1.0 + n.key.x * cell, v0 = -1.0 + n.key.y * cell;
        const double um = u0 + cell * 0.5, vm = v0 + cell * 0.5;
        const double eps = cell * 0.05;
        for (int e = 0; e < 4; ++e) {
            double pu = um, pv = vm;
            if (e == 0) { if (u0 <= -1.0 + 1e-12) continue; pu = u0 - eps; }
            if (e == 1) { if (u0 + cell >= 1.0 - 1e-12) continue; pu = u0 + cell + eps; }
            if (e == 2) { if (v0 <= -1.0 + 1e-12) continue; pv = v0 - eps; }
            if (e == 3) { if (v0 + cell >= 1.0 - 1e-12) continue; pv = v0 + cell + eps; }
            for (int M = std::min(kLmax, L + 2); M >= 0; --M) {
                sel_node nb{};
                if (contains(M, pu, pv, &nb)) {
                    const int d = std::clamp(M - L, 0, 2);
                    n.delta[e] = static_cast<std::int8_t>(d);
                    break;
                }
            }
        }
    }
    return out;
}

// std140 push: mat4 mvp (64 B). VERTEX-only range — declaring it for a
// stage with no push block SEGVs lavapipe's JIT (gate gotcha 8.18).
struct alignas(16) PushTerrain {
    float mvp[16];
};

constexpr float kBandCol[2][3] = {{0.45f, 0.30f, 0.20f}, {0.15f, 0.45f, 0.20f}};

// One streamed frame: selected chunks stitched into ONE camera-relative
// mesh (fp64 composition -> fp32, 8.9). Returns triangle count.
std::uint32_t build_stream_mesh(const sim::terrain_def& def,
                                std::span<const sim::terrain_stamp> stamps,
                                std::span<const sim::surface_band> bands,
                                double alt, cooked_mesh& cm,
                                double& hmin_out, double& hmax_out) {
    const dvec3 cam = (kR + alt) * kAxis;
    const auto nodes = select_chunks(alt);
    for (const auto& n : nodes) {
        const auto c = sim::gen_chunk(def, stamps, n.key,
                                      sim::chunk_edges{.neighbor_delta = n.delta});
        const int band = sim::surface_band_at(bands, c.verts[12].height_m);
        const float* col = kBandCol[band < 0 ? 0 : (band > 1 ? 1 : band)];
        const std::uint32_t base = static_cast<std::uint32_t>(cm.vertices.size());
        for (const auto& v : c.verts) {
            const dvec3 p = (kR + v.height_m) * v.dir - cam;
            mesh_vertex mv{};
            mv.pos[0] = static_cast<float>(p.x);
            mv.pos[1] = static_cast<float>(p.y);
            mv.pos[2] = static_cast<float>(p.z);
            mv.nrm[0] = col[0]; mv.nrm[1] = col[1]; mv.nrm[2] = col[2];
            // uv[0] = normalized height for the frag-side brightness ramp:
            // flat albedo at nadir hides ALL relief (a flat green screen is
            // a valid image of ANY surface), so the human-eyes frames shade
            // height explicitly. uv is dead weight in this pass.
            mv.uv[0] = static_cast<float>(v.height_m);  // raw; normalized below
            mv.uv[1] = 0.0f;
            mv.tan[3] = 1.0f;
            cm.vertices.push_back(mv);
        }
        for (int cy = 0; cy < sim::kChunk_cells; ++cy)
            for (int cx = 0; cx < sim::kChunk_cells; ++cx) {
                const int a = cy * sim::kChunk_verts + cx, b = a + 1;
                const int c2 = a + sim::kChunk_verts, d = c2 + 1;
                cm.indices.insert(cm.indices.end(),
                                  {base + a, base + c2, base + b, base + b, base + c2, base + d});
            }
    }
    // Per-frame min/max normalization: this fixture's absolute heights cluster
    // inside a narrow band (crater floor ~ -150 m), so a FIXED window would
    // quantize the whole frame to one byte. Relative shading keeps the relief
    // legible in every frame; the summary documents the per-frame mapping.
    float hmin = 1e30f, hmax = -1e30f;  // host-measured spread, logged
    for (const auto& v : cm.vertices) {
        hmin = std::min(hmin, v.uv[0]);
        hmax = std::max(hmax, v.uv[0]);
    }
    hmin_out = hmin; hmax_out = hmax;
    const float span = std::max(hmax - hmin, 1.0f);
    for (auto& v : cm.vertices)
        v.uv[0] = (v.uv[0] - hmin) / span;
    return static_cast<std::uint32_t>(cm.indices.size()) / 3u;
}

double painted_frac(const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h) {
    // painted = far from the blue clear (0,0,1). The capture carries the
    // swapchain memory order, so pure blue is (B,G,R,A)=(255,0,0) OR (0,0,255)
    // depending on format — probe both. A chunk-selection hole or swap gap
    // shows as blue -> lowers the fraction (same contract shape as the golden
    // test's zero-blue-pixel pin).
    std::size_t painted = 0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const std::uint8_t c0 = rgba[i], c1 = rgba[i + 1], c2 = rgba[i + 2];
        const bool blue = (c0 < 24 && c1 < 24 && c2 > 231) ||
                          (c2 < 24 && c1 < 24 && c0 > 231);
        if (!blue) ++painted;
    }
    return static_cast<double>(painted) / (static_cast<double>(w) * h);
}

} // namespace

int run_descent_mode(ve::platform::window& win, ve::gfx::device& dev,
                     ve::gfx::wsi_presenter& pres,
                     const std::filesystem::path& units_dir,
                     const std::filesystem::path& out_dir,
                     std::uint32_t frames) {
    const auto bad = [](const char* what) {
        VE_LOG_ERROR("descent: %s", what);
        return 2;
    };

    // --- cooked descent shaders (#13) ---------------------------------------
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
    const auto v_path = find_unit(units_dir, "descent.vert");
    const auto f_path = find_unit(units_dir, "descent.frag");
    if (v_path.empty() || f_path.empty()) {
        VE_LOG_ERROR("descent: no cooked descent.vert/.frag under %s",
                     units_dir.string().c_str());
        return 2;
    }
    ve::core::cooked::Unit vu, fu;
    std::string err;
    if (!ve::core::cooked::read_unit_file(v_path.string(), vu, &err) ||
        !ve::core::cooked::read_unit_file(f_path.string(), fu, &err)) {
        VE_LOG_ERROR("descent: reading a cooked unit: %s", err.c_str());
        return 2;
    }
    auto vert = ve::gfx::shader_module::create(dev, vu.payload);
    auto frag = ve::gfx::shader_module::create(dev, fu.payload);
    if (!vert || !frag) {
        VE_LOG_ERROR("descent: shader module: %s", (vert ? frag : vert).error().c_str());
        return 2;
    }

    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = static_cast<std::uint32_t>(sizeof(PushTerrain)),
    };
    const VkPipelineLayoutCreateInfo lci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .setLayoutCount = 0, .pSetLayouts = nullptr,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev.handle(), &lci, nullptr, &layout) != VK_SUCCESS) {
        return bad("pipeline layout");
    }
    ve::gfx::pipeline_cache cache(dev);
    ve::gfx::pipeline_desc bd{};
    bd.layout = layout;
    bd.stages = {{VK_SHADER_STAGE_VERTEX_BIT, vert->handle(), "main"},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, frag->handle(), "main"}};
    bd.bindings = {{.binding = 0,
                    .stride = static_cast<std::uint32_t>(sizeof(mesh_vertex)),
                    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
    bd.attributes = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 12},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 24}};
    bd.cull = VK_CULL_MODE_NONE;  // sphere-outward winding not trusted at seams
    bd.depth_format = VK_FORMAT_D32_SFLOAT;  // the 7.16.1 WSI depth attachment
    bd.color_formats = {pres.color_format()};
    bd.blends = {{}};
    bd.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);
    auto pipe = cache.get(bd);
    if (!pipe) {
        VE_LOG_ERROR("descent: pipeline: %s", pipe.error().c_str());
        vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
        return 2;
    }

    // One projection for the WHOLE descent: 1.5 m surface detail to a
    // 6000 km planet in a single 0.5 m..1e12 m log-zoom window (8.10).
    const mat4f proj = math::perspective_logzoom(
        static_cast<float>(kN), static_cast<float>(kF), -1.0f, 1.0f, -1.0f, 1.0f);
    // mesh frame = world axes shifted to the camera; forward = -kAxis.
    // kAxis lies in the XY plane, so world +Z is a legal up.
    const auto view_d = math::look_at_rh(dvec3{0.0, 0.0, 0.0}, -1000.0 * kAxis,
                                         dvec3{0.0, 0.0, 1.0});
    mat4f view_f{};
    for (int i = 0; i < 16; ++i) view_f.m[i] = static_cast<float>(view_d.m[i]);
    const mat4f mvp = proj * view_f;  // camera-relative meshes: view is fixed

    const auto def = relief_def();
    const auto stamps = relief_stamps();
    const auto bands = relief_bands();

    // --- the descent: 20 km -> 1.5 km, geometric so more time is spent
    // low (where the streaming churn is worst) --------------------------------
    std::vector<double> alts;
    for (std::uint32_t f = 0; f < frames; ++f) {
        const double u = frames > 1 ? static_cast<double>(f) / (frames - 1) : 0.0;
        alts.push_back(20000.0 * std::pow(1500.0 / 20000.0, u));
    }

    struct Row { double alt, painted; std::uint32_t chunks, tris; double ms; };
    std::vector<Row> rows;
    bool closed = false, png_ok = true;
    double worst_painted = 1.0;
    for (std::uint32_t f = 0; f < frames && !closed; ++f) {
        win.poll_events();
        if (win.should_close()) { closed = true; break; }

        cooked_mesh cm;
        double hmin = 0.0, hmax = 0.0;
        const std::uint32_t tris =
            build_stream_mesh(def, stamps, bands, alts[f], cm, hmin, hmax);
        auto gm = ve::gfx::gpu_mesh::upload(dev, std::move(cm));
        if (!gm) {
            VE_LOG_ERROR("descent: mesh upload: %s", gm.error().c_str());
            vkDestroyPipeline(dev.handle(), *pipe, nullptr);
            vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
            return bad("mesh upload");
        }
        if (auto idx = pres.acquire(); !idx) {
            vkDestroyPipeline(dev.handle(), *pipe, nullptr);
            vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
            return bad("acquire");
        }
        // selection count for the csv (recompute is cheap vs the raster)
        const auto sel_n = select_chunks(alts[f]).size();

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
        catt.clearValue.color.float32[0] = 0.0f;  // clear BLUE (hole probe)
        catt.clearValue.color.float32[1] = 0.0f;
        catt.clearValue.color.float32[2] = 1.0f;
        catt.clearValue.color.float32[3] = 1.0f;
        VkRenderingAttachmentInfo datt{};
        datt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        datt.imageView = pres.depth_view();
        datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        datt.clearValue.depthStencil.depth = 0.0f;  // reversed-Z far
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
        PushTerrain push{};
        std::copy(&mvp.m[0], &mvp.m[0] + 16, push.mvp);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        const VkBuffer vb = gm->vertex_buffer();
        const VkDeviceSize o = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &o);
        vkCmdBindIndexBuffer(cmd, gm->index_buffer(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, gm->index_count(), 1, 0, 0, 0);
        vkCmdEndRendering(cmd);
        const auto t0 = std::chrono::steady_clock::now();
        if (!pres.end_pass(cmd) || !pres.present(cmd)) {
            vkDestroyPipeline(dev.handle(), *pipe, nullptr);
            vkDestroyPipelineLayout(dev.handle(), layout, nullptr);
            return bad("end_pass / present");
        }
        const auto t1 = std::chrono::steady_clock::now();

        const auto cap = pres.capture_rgba8();  // between frames, never mid-pass
        if (cap.empty()) { closed = true; break; }
        const double painted = painted_frac(cap, pres.width(), pres.height());
        worst_painted = std::min(worst_painted, painted);
        rows.push_back({alts[f], painted, static_cast<std::uint32_t>(sel_n), tris,
                        std::chrono::duration<double, std::milli>(t1 - t0).count()});
        VE_LOG_INFO("descent: frame %u alt %.0f m  chunks %zu  tris %u  painted %.3f "
                      "hspan %.0f m",
                    f, alts[f], sel_n, tris, painted, hmax - hmin);

        if (!out_dir.empty()) {
            std::filesystem::create_directories(out_dir);
            char name[64];
            std::snprintf(name, sizeof name, "descent_frame%02u_alt%.0fm.png", f, alts[f]);
            const bool swap_rb = pres.color_format() == VK_FORMAT_B8G8R8A8_UNORM ||
                                 pres.color_format() == VK_FORMAT_B8G8R8A8_SRGB;
            png_ok = write_png_rgba8((out_dir / name).string().c_str(), pres.width(),
                                     pres.height(), cap.data(), swap_rb) && png_ok;
        }
    }
    vkDestroyPipeline(dev.handle(), *pipe, nullptr);
    vkDestroyPipelineLayout(dev.handle(), layout, nullptr);

    if (closed || rows.empty()) {
        VE_LOG_ERROR("descent: window closed with %zu/%u frames rendered", rows.size(), frames);
        return 4;
    }
    if (!out_dir.empty()) {
        std::filesystem::create_directories(out_dir);
        std::ofstream csv(out_dir / "descent_frames.csv");
        csv << "frame,alt_m,chunks,tris,painted_frac,present_ms\n";
        for (std::size_t i = 0; i < rows.size(); ++i)
            csv << i << ',' << rows[i].alt << ',' << rows[i].chunks << ',' << rows[i].tris
                << ',' << rows[i].painted << ',' << rows[i].ms << '\n';
        std::ofstream sum(out_dir / "descent_summary.txt");
        sum << "8.18 descent segment (HUMAN EYES; the pop/Z-fight proof is the\n"
               "terrain_descent_golden ctest test). Streaming chunks + morph\n"
               "skirts + log-zoom reversed-Z, ONE projection for the whole ride.\n"
            << "world (DEMO-ONLY dramatized data, pinned goldens untouched):\n"
               "  r=6e6 m, seed 0xB17E5EED, relief 8 km @ base_freq 300 oct5\n"
               "  (~126 km -> ~8 km features), 1 crater stamp 0.02 rad 1 km deep\n"
               "  (the golden-test twin field is a 0.05%-slope plane at these\n"
               "  scales - locally invisible; shading = band x per-frame-normalized\n"
               "  height x soft slope shade, cosmetic only)\n"
            << "camera: crater-slope axis 0.017 rad off crater center, 20 km -> " << alts.back()
            << " m, " << rows.size() << " frames, fov tangents +-1\n"
            << "chunks frame0=" << rows.front().chunks << " tris frame0=" << rows.front().tris
            << " | chunks last=" << rows.back().chunks << " tris last=" << rows.back().tris
            << " | worst painted=" << worst_painted << "\n"
            << "blue clear = hole probe (painted = non-blue); gate painted>=0.90 -> "
            << (worst_painted >= 0.90 ? "PASS" : "FAIL") << '\n';
    }
    if (worst_painted < 0.90) {
        VE_LOG_ERROR("descent: a frame painted under the 0.90 floor (%.3f) — "
                     "blue pixels = streaming hole or swap gap", worst_painted);
        return 4;
    }
    if (!out_dir.empty() && !png_ok) {
        VE_LOG_ERROR("descent: a PNG write failed (exit 4)");
        return 4;
    }
    VE_LOG_INFO("descent: %zu frames, alt %.0f -> %.0f m, worst painted %.3f",
                rows.size(), rows.front().alt, rows.back().alt, worst_painted);
    return 0;
}

} // namespace ve::demo
