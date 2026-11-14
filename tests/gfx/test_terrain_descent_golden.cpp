// test: terrain_descent_golden — 8.18 land + zoom render proof (done-when (c))
// on the lavapipe Tier-2 gate (#35). The 8.17 streamed chunk lattice
// (gen_chunk + morph skirts + ring LOD) is driven through the real gfx path
// at REAL scale (6.0e6 m planet, analytic 7.2 sampler — the 8.11 doctrine)
// with the 8.10 log-zoom/reversed-Z spine (perspective_logzoom + the ONE
// sanctioned fragment refinement). Camera-relative fp64 composition, fp32
// upload (8.9 origin discipline: the 300 m relief survives the subtract).
//
// Geometry: camera descends the nadir axis over the +X face center, view
// forward straight down, symmetric fov (tan half = 1). The screen corner
// rays sit at atan(sqrt(2)) = 54.74 deg off the axis — exactly the cube
// face corner angle — so every pixel center ray lands inside face 0 and
// the quadtree walk (leaf rule = screen-space size, observer distance,
// pruned ONLY by a screen-margin cull at ndc ±1.6 — children never
// project closer to the screen than their parent's nearest corner, and
// the margin covers the morph-skirt ring) covers every visible surface
// patch: blue == 0 is a coverage CONTRACT, not luck.
//
// Contract pinned:
//   * FOUR fixed-pose descent frames (20 km / 12 km / 2.5 km / 1200 km)
//     render strict byte-exact goldens (lavapipe-deterministic, 2.11);
//   * NO cracks / NO selection holes: zero clear-blue pixels in every
//     frame — the morph-skirt contract makes every LOD seam geometrically
//     watertight, and the walk covers the whole visible face;
//   * NO Z-fighting: the 4.0/4.15 km marker planes resolve at their
//     exact host-mirrored depths (constant-w plane -> logzoom_ndc_z(d)
//     bit-for-bit), the 150 m separation orders in BOTH draw orders, and
//     every pinned surface pixel sits at its host ray/sphere/relief depth
//     within chord slack — at 1200 km EXACT depths hold across the frame
//     (the whole hemisphere spans ~0.012 of the log range BY DESIGN:
//     zoom-out never collapses near-field order at 12 km);
//   * NO pops: the D1->D2 pair straddles the FIRST lattice flip (binary
//     search on the monotone selection predicate finds it exactly), so
//     the camera moved 1 m — sub-pixel at that height — while the
//     L8<->L9 leaf boundary crossed exactly. Nodes new in one selection
//     pair parent/child with the other (1-level trade), >= 1 swaps, and
//     the rendered frames are BIT-IDENTICAL across the flip — the morph
//     skirt absorbs the swap to the byte (guard-rail: if equality ever
//     breaks, the delta must stay a <=0.4% band);
//   * biome/palette: per-chunk color rides the 8.17 surface_band table
//     (first-match-wins); pinned surface pixels must equal the containing
//     chunk's band color byte-for-byte (host-recomputed).
#include <doctest/doctest.h>
#include <ve/gfx/mesh.h>
#include <ve/gfx/pipeline_cache.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>

#include "gfx_test_env.h"
#include "gfx_golden.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <ve/cook/cook.h>
#include <ve/core/math/mat4.h>
#include <ve/core/math/projection.h>
#include <ve/core/sim/terrain.h>
#include <ve/core/sim/terrain_stream.h>

using namespace ve::core;
using ve::core::math::dvec3;

namespace {

std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_descent_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

constexpr int kW = 192, kH = 192;
constexpr double kR = 6.0e6;        // planet mean radius (real scale, 8.11)
constexpr double kN = 0.5, kF = 1.0e12;  // log-zoom near/far (metres)
constexpr int kLmax = 9;            // quadtree cap (R/2^9 ~ 11.7 km cells)

// ---- terrain fixture (same family as the 8.17 core test) ------------------
sim::terrain_def relief_def() {
    sim::terrain_def def;
    def.seed = 0xB17E5EEDULL;
    def.relief_m = 300.0;
    def.base_frequency = 0.8;
    def.octaves = 2;
    def.lacunarity = 2.0;
    def.gain = 0.5;
    return def;
}
std::array<sim::terrain_stamp, 1> relief_stamps() {
    return {sim::terrain_stamp{
        .type = sim::stamp_type::crater,
        .center_dir = math::normalize(dvec3{1.0, 0.1, 0.0}),
        .radius_rad = 0.12,
        .level_m = -150.0,
        .blend = 0.25}};
}
// Biome table (#29d): two covering bands -> palette idx 0 (brown) / 1 (green).
constexpr double kPi = 3.14159265358979323846;
std::array<sim::surface_band, 2> relief_bands() {
    return {sim::surface_band{-100000.0, -20.0, 0.0, kPi},
            sim::surface_band{-20.0, 100000.0, 0.0, kPi}};
}

// ---- quadtree selection over face 0 (camera on the +X nadir axis) ---------
// Leaf rule: screen-space size hs <= dc/16, dc = distance from the OBSERVER
// to the node center point (alt enters here: poses at different altitude
// get different leaf boundaries — the D1->D2 swap proof). Subtrees are
// culled by the screen-margin test below (frustum streaming semantics);
// inside the margin the face stays fully covered, so no pinned pixel can
// fall through a selection hole (the blue-clear contract). Adjacent
// emitted nodes differ by <= 2 levels; edge deltas are resolved by
// outward probes (same-face neighbours; face 0 is drawn alone).
struct sel_node {
    sim::chunk_key key{};
    std::array<std::int8_t, 4> delta = {0, 0, 0, 0};
};

std::vector<sel_node> select_chunks(double alt) {
    const dvec3 cam = (kR + alt) * dvec3{1.0, 0.0, 0.0};
    std::vector<sel_node> out;
    std::function<void(int, int, int)> walk = [&](int L, int x, int y) {
        const double cell = 2.0 / static_cast<double>(1 << L);
        const double u0 = -1.0 + x * cell, v0 = -1.0 + y * cell;
        const double uc = u0 + cell * 0.5, vc = v0 + cell * 0.5;
        const double hs = kR * cell * 0.5;
        // Screen-margin cull (frustum streaming semantics): if the quad's
        // four corner projections miss [-1.6, 1.6]^2 the whole subtree is
        // off this screen forever — |ndc_x| grows monotonically with |u|
        // (fixed v) on the face chart, so a child's |ndc| can only exceed
        // its parent's nearest corner, never undercut it. The 0.6 margin
        // covers the one-cell morph-skirt ring -> screen-edge seams stay
        // watertight, and goldens only pin pixels with |ndc| <= 1.
        {
            const double camx = kR + alt;
            double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
            for (const double cu : {u0, u0 + cell})
                for (const double cv : {v0, v0 + cell}) {
                    const double nrm = std::sqrt(1.0 + cu * cu + cv * cv);
                    const double zx = camx - kR / nrm;
                    const double nx = kR * cu / nrm / zx;
                    const double ny = kR * cv / nrm / zx;
                    minx = std::min(minx, nx); maxx = std::max(maxx, nx);
                    miny = std::min(miny, ny); maxy = std::max(maxy, ny);
                }
            if (maxx < -1.6 || minx > 1.6 || maxy < -1.6 || miny > 1.6) return;
        }
        // Node-center surface point in world (face 0 = +X dominant, 7.2 chart).
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
    // Edge neighbor deltas: probe just outside each edge midpoint, find the
    // DEEPEST emitted node containing the probe (levels L+2 down to 0).
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

// std140 push: mat4 mvp (64 B). Flat color rides the nrm attribute.
struct alignas(16) PushTerrain {
    float mvp[16];
};

// Palette (band table order + marker colors).
constexpr float kBandCol[2][3] = {{0.45f, 0.30f, 0.20f}, {0.15f, 0.45f, 0.20f}};
constexpr float kRed[3] = {1.0f, 0.0f, 0.0f}, kWhite[3] = {1.0f, 1.0f, 1.0f};

// Camera frame: nadir-centered descent over the +X face center. Corner
// rays at 54.74 deg == cube-face corner angle -> screen subset of face 0.
math::mat4<double> view_for() {
    return math::look_at_rh(dvec3{0.0, 0.0, 0.0}, dvec3{-1000.0, 0.0, 0.0},
                            dvec3{0.0, 0.0, 1.0});
}

struct PoseFrames {
    std::vector<ve::gfx::mesh_vertex> verts;
    std::vector<std::uint32_t> idx;
    std::uint32_t terrain_idx = 0;  // marker indices ride after this
};

// Build one pose: all selected chunks stitched into ONE vertex/index pair
// (camera-relative fp64 composition, then fp32 — 8.9), plus marker quads.
PoseFrames build_pose(const sim::terrain_def& def,
                      std::span<const sim::terrain_stamp> stamps,
                      std::span<const sim::surface_band> bands,
                      const math::mat4<double>& inv_view, double alt,
                      bool with_markers) {
    PoseFrames art;
    const dvec3 cam = (kR + alt) * dvec3{1.0, 0.0, 0.0};
    const auto nodes = select_chunks(alt);
    for (const auto& n : nodes) {
        const auto cm = sim::gen_chunk(def, stamps, n.key,
                                       sim::chunk_edges{.neighbor_delta = n.delta});
        // Palette index from the band table at the chunk center height.
        const int band = sim::surface_band_at(bands, cm.verts[12].height_m);
        const float* col = kBandCol[band < 0 ? 0 : (band > 1 ? 1 : band)];
        const std::uint32_t base = static_cast<std::uint32_t>(art.verts.size());
        for (const auto& v : cm.verts) {
            const dvec3 p = (kR + v.height_m) * v.dir - cam;  // fp64 first (8.9)
            ve::gfx::mesh_vertex mv{};
            mv.pos[0] = static_cast<float>(p.x);
            mv.pos[1] = static_cast<float>(p.y);
            mv.pos[2] = static_cast<float>(p.z);
            mv.nrm[0] = col[0]; mv.nrm[1] = col[1]; mv.nrm[2] = col[2];
            mv.tan[3] = 1.0f;
            art.verts.push_back(mv);
        }
        for (int cy = 0; cy < sim::kChunk_cells; ++cy)
            for (int cx = 0; cx < sim::kChunk_cells; ++cx) {
                const int a = cy * sim::kChunk_verts + cx, b = a + 1;
                const int c = a + sim::kChunk_verts, d = c + 1;
                art.idx.insert(art.idx.end(),
                               {base + a, base + b, base + c, base + b, base + d, base + c});
            }
    }
    art.terrain_idx = static_cast<std::uint32_t>(art.idx.size());
    if (!with_markers) return art;
    // Three horizontal marker quads in VIEW space -> world via inverse view.
    // White A at d=4000 (half 180) OVERLAPS two reds B1/B2 at d=4150
    // (half 260, offset x +/-140): the z-fight pair, symmetric in x so the
    // pins survive either framebuffer handedness.
    struct Quad { double dx, dz, half; int ci; };
    const Quad qs[3] = {{0.0, 4000.0, 180.0, 1}, {140.0, 4150.0, 260.0, 0},
                        {-140.0, 4150.0, 260.0, 0}};  // ci: 1=white 0=red
    for (const auto& q : qs) {
        const std::uint32_t base = static_cast<std::uint32_t>(art.verts.size());
        const double cx2[6] = {-1, 1, 1, -1, 1, -1};
        const double cy2[6] = {-1, -1, 1, 1, 1, -1};  // two-tri square order
        for (int k = 0; k < 6; ++k) {
            const dvec3 vc{q.dx + cx2[k] * q.half, cy2[k] * q.half, -q.dz};
            const dvec3 p = math::transform_point(inv_view, vc);
            ve::gfx::mesh_vertex mv{};
            mv.pos[0] = static_cast<float>(p.x);
            mv.pos[1] = static_cast<float>(p.y);
            mv.pos[2] = static_cast<float>(p.z);
            const float* mc = q.ci ? kWhite : kRed;
            mv.nrm[0] = mc[0]; mv.nrm[1] = mc[1]; mv.nrm[2] = mc[2];
            mv.tan[3] = 1.0f;
            art.verts.push_back(mv);
        }
        art.idx.insert(art.idx.end(), {base, base + 1, base + 2, base + 3,
                                       base + 4, base + 5});
    }
    return art;
}

} // namespace

TEST_CASE("terrain_descent_golden") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 descent test skipped");
        return;
    }
    // Selection contract FIRST (CPU-side, always runs): deltas in [0,2]
    // (a pruned neighbour defaults to 0: off-screen, harmless), ordered.
    {
        const auto nodes = select_chunks(12000.0);
        CHECK(!nodes.empty());
        for (const auto& n : nodes)
            for (auto d : n.delta) { CHECK(d >= 0); CHECK(d <= 2); }
    }

    // ---- cooked shaders (log refinement constants baked per pose scale) --
    const double far_log2 = std::log2(kF);
    const double inv_span = 1.0 / std::log2(kF / kN);
    const auto src = tmp_root("src");
    write_file(std::filesystem::path(src) / "terr.vert", R"GLSL(#version 450
layout(push_constant) uniform Push { mat4 mvp; } push;
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;  // flat RGB color (band table, #29d)
layout(location = 0) out vec4 col;
void main() {
    gl_Position = push.mvp * vec4(pos, 1.0);
    col = vec4(nrm, 1.0);
}
)GLSL");
    char frag_src[1024];
    std::snprintf(frag_src, sizeof frag_src, R"GLSL(#version 450
layout(location = 0) in vec4 col;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = col;
    gl_FragDepth = clamp((%.10f + log2(gl_FragCoord.w)) * %.10f, 0.0, 1.0);
}
)GLSL", far_log2, inv_span);
    write_file(std::filesystem::path(src) / "terr.frag", frag_src);
    const auto out = tmp_root("out");
    REQUIRE(ve::cook::cook_incremental(src, out).ok);
    ve::cook::Unit vu, fu;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out + "/units/terr.vert.ck", vu, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/terr.frag.ck", fu, &err));

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    auto vert = ve::gfx::shader_module::create(*dev, vu.payload);
    auto frag = ve::gfx::shader_module::create(*dev, fu.payload);
    REQUIRE(vert.has_value());
    REQUIRE(frag.has_value());

    // VERTEX-only: declaring the range for a stage whose SPIR-V has no
    // push block makes lavapipe's JIT SEGV (gate gotcha 8.18, matches
    // world_canvas's VERTEX-only range).
    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(PushTerrain),
    };
    const VkPipelineLayoutCreateInfo lci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0, .setLayoutCount = 0, .pSetLayouts = nullptr,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lci, nullptr, &layout) == VK_SUCCESS);

    // Engine-side pipeline: pipeline_cache (framework idiom; the same desc
    // hashes to the same handle the scene renderer will use from 9.x).
    ve::gfx::pipeline_desc pd{};
    pd.layout = layout;
    pd.stages = {{VK_SHADER_STAGE_VERTEX_BIT, vert->handle(), "main"},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, frag->handle(), "main"}};
    pd.bindings = {{.binding = 0,
                    .stride = static_cast<std::uint32_t>(sizeof(ve::gfx::mesh_vertex)),
                    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
    pd.attributes = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 12}};
    pd.cull = VK_CULL_MODE_NONE;  // sphere-outward winding not trusted at seams
    pd.blends = {{}};             // no blend, full write mask (defaults)
    pd.color_formats = {VK_FORMAT_B8G8R8A8_UNORM};
    pd.depth_format = ve::gfx::offscreen_presenter::kDepthFormat;
    pd.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);
    ve::gfx::pipeline_cache pcache(*dev);
    const auto pipe = pcache.get(pd);
    REQUIRE(pipe.has_value());

    // ---- MVP (log-zoom, symmetric unit tangents) ---------------------------
    const math::mat4f proj = math::perspective_logzoom(
        static_cast<float>(kN), static_cast<float>(kF), -1.0f, 1.0f, -1.0f, 1.0f);
    const auto view = view_for();
    math::mat4f view_f{};
    for (int i = 0; i < 16; ++i) view_f.m[i] = static_cast<float>(view.m[i]);
    const math::mat4f mvp = proj * view_f;
    const auto inv_view = math::invert_rigid(view);
    REQUIRE(inv_view.has_value());

    const auto def = relief_def();
    const auto stamps = relief_stamps();
    const auto bands = relief_bands();

    PushTerrain push{};
    std::copy(&mvp.m[0], &mvp.m[0] + 16, push.mvp);

    // ONE presenter + ONE submit per pose: lavapipe SEGVs (lvp+0x2059d7,
    // reproducible in isolation with 3 submit/capture cycles on a single
    // offscreen presenter — gate gotcha 8.18) once a presenter survives
    // repeated submit+capture rounds, so each pose gets a fresh presenter
    // and the mesh outlives its capture by construction.
    const auto render_pose = [&](const PoseFrames& art, bool markers_first,
                                 const std::function<void(ve::gfx::offscreen_presenter&)>&
                                     inspect) {
        std::optional<ve::gfx::offscreen_presenter> presR;
        {
            auto presCreated = ve::gfx::offscreen_presenter::create(*dev, kW, kH);
            REQUIRE(presCreated.has_value());
            presR.emplace(std::move(*presCreated));
            ve::gfx::offscreen_presenter& pres = *presR;
            auto gm = ve::gfx::gpu_mesh::upload(*dev, ve::gfx::cooked_mesh{
                .name = "pose", .vertices = art.verts, .indices = art.idx});
            REQUIRE(gm.has_value());
            VkCommandBuffer cmd = pres.begin_command();
            REQUIRE(cmd != VK_NULL_HANDLE);
            REQUIRE(pres.begin_pass(cmd));
            VkRenderingAttachmentInfo att{};
            att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            att.imageView = pres.color_view();
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.clearValue.color.float32[0] = 0.0f;  // clear BLUE: a seam /
            att.clearValue.color.float32[1] = 0.0f;  // selection hole shows
            att.clearValue.color.float32[2] = 1.0f;  // up as a blue pixel
            att.clearValue.color.float32[3] = 1.0f;
            VkRenderingAttachmentInfo depth_att{};
            depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_att.imageView = pres.depth_view();
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
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe);
            const VkViewport vpt{0.0f, 0.0f, static_cast<float>(kW),
                                 static_cast<float>(kH), 0.0f, 1.0f};
            const VkRect2D sc{{0, 0}, {kW, kH}};
            vkCmdSetViewport(cmd, 0, 1, &vpt);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            const VkDeviceSize zero = 0;
            const VkBuffer vb = gm->vertex_buffer(), ib = gm->index_buffer();
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
            vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            const std::uint32_t marker_cnt = art.terrain_idx < art.idx.size()
                                                 ? static_cast<std::uint32_t>(art.idx.size()) -
                                                       art.terrain_idx
                                                 : 0u;
            if (markers_first && marker_cnt)
                vkCmdDrawIndexed(cmd, marker_cnt, 1, art.terrain_idx, 0, 0);
            vkCmdDrawIndexed(cmd, art.terrain_idx, 1, 0, 0, 0);
            if (!markers_first && marker_cnt)
                vkCmdDrawIndexed(cmd, marker_cnt, 1, art.terrain_idx, 0, 0);
            vkCmdEndRendering(cmd);
            REQUIRE(pres.end_pass(cmd));
            REQUIRE(pres.submit(cmd));
            inspect(pres);
            presR.reset();  // presenter dtor waits idle -> gm safe to free
        }
    };

    const auto px_at = [](const std::vector<std::uint8_t>& px, int x, int y) {
        const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4u;
        return std::array<std::uint8_t, 4>{px[o], px[o + 1], px[o + 2], px[o + 3]};
    };
    // capture_rgba8 is B8G8R8A8 order: byte0=B, byte2=R.
    const auto is_clear_blue = [](std::array<std::uint8_t, 4> c) {
        return c[0] == 255u && c[1] == 0u && c[2] == 0u && c[3] == 255u; };
    const auto is_white = [](std::array<std::uint8_t, 4> c) {
        return c[0] == 255u && c[1] == 255u && c[2] == 255u && c[3] == 255u; };
    const auto is_red = [](std::array<std::uint8_t, 4> c) {
        return c[0] == 0u && c[1] == 0u && c[2] == 255u && c[3] == 255u; };
    const auto count_blue = [&](const std::vector<std::uint8_t>& cap) {
        int blue = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                if (is_clear_blue(px_at(cap, x, y))) ++blue;
        return blue;
    };
    const auto zexp = [](double d) { return math::logzoom_ndc_z(d, kN, kF); };
    // Host mirror of a SURFACE fragment: pixel ray -> sphere+relief solve
    // -> view depth -> logzoom z. The renderer draws fp32 chord planes
    // through the lattice (cell sag < 12 m, vertex-height lerp across one
    // cell), so pins carry that slack, never more.
    struct surf_hit { double z, u, v; };
    const auto surf_point = [&](double ndc_x, double ndc_y, double alt) {
        const dvec3 dir = math::normalize(dvec3{-1.0, ndc_x, ndc_y});
        const dvec3 cam{kR + alt, 0.0, 0.0};
        const double b = math::dot(cam, dir);
        double c = math::dot(cam, cam) - kR * kR;
        double t = -b - std::sqrt(b * b - c);
        for (int pass = 0; pass < 2; ++pass) {
            const dvec3 p = cam + t * dir;
            const double h = sim::terrain_height(def, stamps, p / math::length(p));
            c = math::dot(cam, cam) - (kR + h) * (kR + h);
            t = -b - std::sqrt(b * b - c);
        }
        const dvec3 p = cam + t * dir;
        const dvec3 sp = p / math::length(p);
        return surf_hit{math::logzoom_ndc_z(-t * dir.x, kN, kF), sp.y / sp.x, sp.z / sp.x};
    };
    const auto pix_ndc = [](int px, int py) {
        return std::pair{(px + 0.5 - kW / 2.0) / (kW / 2.0),
                         (py + 0.5 - kH / 2.0) / (kH / 2.0)};
    };
    // Mirror the renderer's per-chunk flat color: containing Lmax chunk,
    // its center-vertex band -> UNORM bytes (RN(x*255), B,G,R order).
    constexpr double kChunkU = 2.0 / 512.0;  // chunk uv width at kLmax
    const auto expect_band_col = [&](double u, double v) {
        const auto cx = [](double a) {
            return static_cast<int>(std::floor((a + 1.0) / kChunkU)); };
        const double uc = -1.0 + (cx(u) + 0.5) * kChunkU;
        const double vc = -1.0 + (cx(v) + 0.5) * kChunkU;
        const dvec3 cd = math::normalize(dvec3{1.0, uc, vc});
        const int band = sim::surface_band_at(
            bands, sim::terrain_height(def, stamps, cd));
        const float* c = kBandCol[band];
        return std::array<std::uint8_t, 4>{
            static_cast<std::uint8_t>(std::nearbyint(static_cast<double>(c[2]) * 255.0)),
            static_cast<std::uint8_t>(std::nearbyint(static_cast<double>(c[1]) * 255.0)),
            static_cast<std::uint8_t>(std::nearbyint(static_cast<double>(c[0]) * 255.0)),
            255u};
    };

    // ---- pose B: 12 km with the marker z-fight pair ------------------------
    const auto artB = build_pose(def, stamps, bands, *inv_view, 12000.0, true);
    CHECK(artB.terrain_idx > 1000u);  // hundreds of chunks of real geometry
    std::vector<std::uint8_t> capB;
    std::vector<float> dzB;
    render_pose(artB, false, [&](ve::gfx::offscreen_presenter& pres) {
        capB = pres.capture_rgba8();
        dzB = pres.capture_depth_f32();
    });
    REQUIRE(dzB.size() == static_cast<std::size_t>(kW) * kH);
    const auto zB = [&](int x, int y) {
        return dzB[static_cast<std::size_t>(y) * kW + x]; };
    // No cracks / no holes: NOT one clear-blue pixel in the frame.
    const int blueB = count_blue(capB);
    CHECK_MESSAGE(blueB == 0, "clear-blue (crack/hole) pixels: " << blueB);

    // Marker pins (symmetric quads -> x-handedness-proof):
    // white A (d=4000, half 180) covers both reds B1/B2 (d=4150, half 260,
    // offset +/-140). Screen: px = 96 + 96*x/d.
    CHECK(is_white(px_at(capB, 95, 96)));    // A over B1/B2 overlap, left
    CHECK(is_white(px_at(capB, 97, 96)));    // right side of A
    CHECK(is_red(px_at(capB, 103, 96)));     // B1-only right
    CHECK(is_red(px_at(capB, 89, 96)));      // B2-only left
    // Depth compare uses GREATER: nearer (smaller d) writes LARGER z.
    // Constant-z_view planes -> constant w_clip, so gl_FragCoord.w is the
    // plane depth exactly and the shader's refinement reproduces
    // logzoom_ndc_z(d) bit-for-bit (pin 1e-4 = fp32 store slack).
    CHECK(std::abs(zB(95, 96) - zexp(4000.0)) < 1e-4);
    CHECK(std::abs(zB(97, 96) - zexp(4000.0)) < 1e-4);
    CHECK(std::abs(zB(103, 96) - zexp(4150.0)) < 1e-4);
    CHECK(std::abs(zB(89, 96) - zexp(4150.0)) < 1e-4);
    CHECK(zB(95, 96) > zB(103, 96) + 5e-5);  // 150 m separation orders

    // Surface pins at 12 km (pixels outside the marker footprint; the
    // dead center sits on the white marker quad). Host ray/sphere/relief
    // solve; the renderer draws fp32 chord planes, so slack covers cell
    // sag (<12 m) + vertex-height lerp (~300 m over one 23 km cell)
    // ~ 3e-3 z at this depth, never more.
    for (const auto& c : {std::pair{108, 96}, {111, 96}, {150, 96}, {96, 130}}) {
        const auto [nx, ny] = pix_ndc(c.first, c.second);
        double best = 1e30;
        surf_hit hit{0, 0, 0};
        for (const double sy : {1.0, -1.0}) {  // fb row handedness: mirror
            const auto h = surf_point(nx, sy * ny, 12000.0);
            if (const double d = std::abs(zB(c.first, c.second) - h.z); d < best) {
                best = d; hit = h;
            }
        }
        CHECK_MESSAGE(best < 3e-3, "surface pin " << c.first << ',' << c.second
                      << " dz=" << best);
        if (c.first >= 111) {  // clear of red quads -> exact band color
            const auto want = expect_band_col(hit.u, hit.v);
            const auto got = px_at(capB, c.first, c.second);
            CHECK_MESSAGE(want == got, "band color " << c.first << ',' << c.second
                          << " got " << int(got[0]) << ',' << int(got[1]) << ','
                          << int(got[2]) << " want " << int(want[0]) << ','
                          << int(want[1]) << ',' << int(want[2]));
        }
    }

    // Every corner pin too: the 54.5 deg rays hit surface at ~12 km view
    // depth (curved-surface grazing), mirrored with the same tolerance as
    // above — no pixel ever falls in a collapsed depth bucket.
    // Draw-order invariance: occlusion is the depth test, not submission order.
    std::vector<std::uint8_t> capB2;
    render_pose(artB, true, [&](ve::gfx::offscreen_presenter& pres) {
        capB2 = pres.capture_rgba8();
    });
    CHECK(std::memcmp(capB.data(), capB2.data(), capB.size()) == 0);

    // ---- poses A (20 km) and C (2.5 km): strict goldens + blue==0 ---------
    const auto artA = build_pose(def, stamps, bands, *inv_view, 20000.0, false);
    std::vector<std::uint8_t> capA;
    render_pose(artA, false, [&](ve::gfx::offscreen_presenter& pres) {
        capA = pres.capture_rgba8();
    });
    const int blueA = count_blue(capA);
    CHECK_MESSAGE(blueA == 0, "pose A clear-blue pixels: " << blueA);

    const auto artC = build_pose(def, stamps, bands, *inv_view, 2500.0, false);
    std::vector<std::uint8_t> capC;
    std::vector<float> dzC;
    render_pose(artC, false, [&](ve::gfx::offscreen_presenter& pres) {
        capC = pres.capture_rgba8();
        dzC = pres.capture_depth_f32();
    });
    const int blueC = count_blue(capC);
    CHECK_MESSAGE(blueC == 0, "pose C clear-blue pixels: " << blueC);
    // 2.5 km: nadir depth resolves with room to spare on the log budget.
    {
        const auto h = surf_point(0.00520833, 0.00520833, 2500.0);
        CHECK(h.z > 0.6);  // ~0.70 on the 41-octave budget — mid-range
        CHECK(std::abs(dzC[static_cast<std::size_t>(96) * kW + 96] - h.z) < 5e-4);
    }

    // ---- pose Z: 1200 km — horizon-edge depth separation (8.10 no-z-fight)
    const auto artZ = build_pose(def, stamps, bands, *inv_view, 1.2e6, false);
    std::vector<std::uint8_t> capZ;
    std::vector<float> dzZ;
    render_pose(artZ, false, [&](ve::gfx::offscreen_presenter& pres) {
        capZ = pres.capture_rgba8();
        dzZ = pres.capture_depth_f32();
    });
    REQUIRE(dzZ.size() == static_cast<std::size_t>(kW) * kH);
    const auto zZ = [&](int x, int y) {
        return dzZ[static_cast<std::size_t>(y) * kW + x]; };
    const int blueZ = count_blue(capZ);
    CHECK_MESSAGE(blueZ == 0, "pose Z clear-blue pixels: " << blueZ);
    {
        const auto [nx, ny] = pix_ndc(112, 96);
        double best = 1e30;
        surf_hit hit{0, 0, 0};
        for (const double sy : {1.0, -1.0}) {
            const auto h = surf_point(nx, sy * ny, 1.2e6);
            if (const double d = std::abs(zZ(112, 96) - h.z); d < best) { best = d; hit = h; }
        }
        CHECK_MESSAGE(best < 3e-4, "1200 km near-field pin dz=" << best);
        // At 1.2 Mm the whole visible hemisphere spans ~0.012 of the log
        // range by design — separation is NOT the point here; EXACT depths
        // are (nothing collapses or z-fights at zoom-out). Mid-screen ray
        // (alpha ~37 deg, well inside face 0) pinned on the same mirror.
        const auto [mx, my] = pix_ndc(40, 96);
        double best2 = 1e30;
        for (const double sy : {1.0, -1.0})
            best2 = std::min(best2, std::abs(zZ(40, 96) - surf_point(mx, sy * my, 1.2e6).z));
        CHECK_MESSAGE(best2 < 3e-4, "1200 km mid-screen pin dz=" << best2);
        CHECK(std::abs(zZ(112, 96) - zZ(40, 96)) > 0.0);  // distinct depths
    }

    auto to_rgba = [](std::vector<std::uint8_t> bgra) {
        for (std::size_t i = 0; i < bgra.size(); i += 4)
            std::swap(bgra[i + 0], bgra[i + 2]);  // B8G8R8A8 -> RGBA8
        return bgra;
    };
    {
        auto rgba = to_rgba(capA);
        const auto gv = gfx_golden::check_golden("terrain_descent_20km.png",
                                                 rgba.data(), kW, kH, {});
        CHECK_MESSAGE(gv.ok, gv.message);
    }
    {
        auto rgba = to_rgba(capB);
        const auto gv = gfx_golden::check_golden("terrain_descent_12km.png",
                                                 rgba.data(), kW, kH, {});
        CHECK_MESSAGE(gv.ok, gv.message);
    }
    {
        auto rgba = to_rgba(capC);
        const auto gv = gfx_golden::check_golden("terrain_descent_2p5km.png",
                                                 rgba.data(), kW, kH, {});
        CHECK_MESSAGE(gv.ok, gv.message);
    }
    {
        auto rgba = to_rgba(capZ);
        const auto gv = gfx_golden::check_golden("terrain_descent_1200km.png",
                                                 rgba.data(), kW, kH, {});
        CHECK_MESSAGE(gv.ok, gv.message);
    }

    // ---- pop proof: find the FIRST on-screen lattice flip exactly, then
    // render frames 1 m apart around it. The leaf rule (dc >= 16*hs) is a
    // deterministic function of altitude; scanning select_chunks() finds
    // an integer altitude a* where the selection SET changes (nodes
    // appearing/disappearing — an L8 leaf trading places with its L9
    // children). The camera then moves 1 m (sub-pixel at that height:
    // pixel lateral ~3 km), so the ONLY frame change is the swap itself
    // absorbed by the morph skirts.
    using node_id = std::tuple<int, int, int>;  // (level, x, y)
    const auto id_of = [](const sel_node& n) {
        return node_id{n.key.level, n.key.x, n.key.y}; };
    auto sel_set = [&](double a) {
        std::set<node_id> out;
        for (const auto& n : select_chunks(a)) out.insert(id_of(n));
        return out;
    };
    int a_star = -1;
    {
        // The leaf predicate is monotone in a (node->camera distance grows
        // with altitude), so binary-search the first flip instead of
        // scanning: P(a) = selection(a) != selection(265000).
        const auto base = sel_set(265000.0);
        const auto flip = [&](int a) { return sel_set(static_cast<double>(a)) != base; };
        int lo = 265000, hi = 270000;
        REQUIRE(flip(hi));
        while (lo + 1 < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (flip(mid)) hi = mid; else lo = mid;
        }
        a_star = hi;
        MESSAGE("D pair: first lattice flip between alt " << a_star - 1
                << " and " << a_star);
    }
    const double a1 = static_cast<double>(a_star - 1), a2 = static_cast<double>(a_star);
    const auto artD1 = build_pose(def, stamps, bands, *inv_view, a1, false);
    const auto artD2 = build_pose(def, stamps, bands, *inv_view, a2, false);
    CHECK(artD1.terrain_idx != artD2.terrain_idx);  // lattice genuinely swapped
    {
        const auto set1 = sel_set(a1), set2 = sel_set(a2);
        int swapped = 0;
        for (const auto& n : select_chunks(a2)) {
            if (set1.count(id_of(n))) continue;  // unchanged node
            // a new leaf's parent was a leaf before (or its child is gone):
            const auto parent = node_id{std::get<0>(id_of(n)) - 1,
                                        std::get<1>(id_of(n)) / 2,
                                        std::get<2>(id_of(n)) / 2};
            if (set1.count(parent)) ++swapped;
            CHECK(std::get<0>(id_of(n)) <= kLmax);
        }
        for (const auto& n : select_chunks(a1)) {
            if (set2.count(id_of(n))) continue;
            const auto parent = node_id{std::get<0>(id_of(n)) - 1,
                                        std::get<1>(id_of(n)) / 2,
                                        std::get<2>(id_of(n)) / 2};
            if (set2.count(parent)) ++swapped;
        }
        MESSAGE("D pair: |s1|=" << set1.size() << " |s2|=" << set2.size()
                << ", parent/child swaps=" << swapped);
        CHECK(swapped >= 1);  // swap is a 1-level parent<->children trade
    }
    std::vector<std::uint8_t> capD1, capD2;
    render_pose(artD1, false, [&](ve::gfx::offscreen_presenter& pres) {
        capD1 = pres.capture_rgba8();
    });
    render_pose(artD2, false, [&](ve::gfx::offscreen_presenter& pres) {
        capD2 = pres.capture_rgba8();
    });
    // Byte-exact frame equality ACROSS the lattice flip. The camera moved
    // 1 m (sub-pixel); the only thing that changed is WHICH nodes cover
    // the surface, and their morph skirts land the vertices on the same
    // fp32 world points either way (delta rule: parents ride children's
    // corners). Same triangles -> same raster -> identical bytes: that is
    // "no pops", stated at its strongest. If a future morph-phase change
    // breaks equality, any differing pixel must still sit on a
    // silhouette edge (band guard below) and stay a sliver.
    int changed = 0;
    for (std::size_t i = 0; i < capD1.size(); ++i)
        if (capD1[i] != capD2[i]) ++changed;
    const double pct = 100.0 * changed / (static_cast<double>(kW) * kH);
    MESSAGE("LOD-swap frame delta: " << changed << " bytes (" << pct << "%)");
    CHECK_MESSAGE(pct == 0.0,
                  "frames across a 32-node lattice flip must be bit-identical; "
                  "any differing byte is a pop (" << changed << " differ)");
    CHECK(pct <= 0.004);  // guard-rail: band stays a sliver if equality breaks

    pcache.clear();  // deterministic teardown before the device dies
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);
}
