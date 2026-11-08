// test: gfx_text_draw — 3.12 runtime half. The vendored TTF is cooked to a
// FONT0 atlas (the cook path itself is pinned by cook_font_atlas), the
// runtime parses ONLY that payload (zero font parsing at runtime, decision
// #15), uploads the atlas through the 3.7 gpu_texture path, and draws "Hi"
// as push-constant glyph quads with the static NEAREST sampler.
// Pins (golden policy: exactness where provable, golden covers the rest):
//   - every framebuffer pixel the CPU atlas model can predict EXACTLY
//     (texel coverage 255 -> white; coverage 0 -> exact clear bytes) matches
//     the capture byte-exact — that is most of every glyph;
//   - pixels outside every glyph box are the exact clear color;
//   - lit-pixel count is in range (text actually covers, no silent dropout);
//   - 64x64 golden text_64.png byte-exact on lavapipe (#35).
// Quads are integer-pixel aligned, so rasterization adds no partial edge
// coverage: every fragment samples exactly one atlas texel (texel-center UVs).
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gfx_golden.h"

#include "../font_layout.h"
#include "../ve_test_portability.h" // getpid

#include <ve/cook/cook.h>
#include <ve/gfx/descriptor.h>
#include <ve/gfx/pipeline_cache.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>
#include <ve/gfx/texture.h>

#include "gfx_test_env.h"

#ifndef VE_SOURCE_DIR
#define VE_SOURCE_DIR "."
#endif

namespace {
std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_text_") + name + "_" + std::to_string(ve_test::pid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}

constexpr int kW = 64, kH = 64;

// Text layout (CPU-side authority; the shader just maps these integers).
constexpr int kPen0 = 4;         // pen x of first glyph
constexpr int kBaseline = 4 + 13; // top + ascent (FONT0 ascent pinned by cook test)
constexpr int kMaxGlyphs = 3;

// std430 mirror of the push block (GLSL aligns Glyph to 16B after count).
struct GlyphPush {
    std::int32_t rect[4]; // screen l, t, w, h (top-left origin, px)
    float uv[4];          // u0 v0 u1 v1, texel-center inset
};
struct PushBlock {
    std::uint32_t count = 0;
    std::uint32_t pad[3] = {0, 0, 0};
    GlyphPush glyphs[kMaxGlyphs];
};
static_assert(sizeof(PushBlock) == 112);

// Clear color: dyadic channels so the UNORM bytes are exact and predictable
// (RNE(0.25*255)=64, RNE(0.375*255)=96); the fragment shader mixes to/from
// these exact values, so coverage-0 and coverage-255 texels reproduce
// byte-exact on ANY conformant rasterizer, not just lavapipe.
constexpr std::array<std::uint8_t, 4> kClearBgra{96, 64, 64, 255}; // B,G,R,A
constexpr std::array<std::uint8_t, 4> kWhite{255, 255, 255, 255};

std::array<std::uint8_t, 4> bgra_probe(const std::vector<std::uint8_t>& p, int x, int y) {
    const auto i = (static_cast<std::size_t>(y) * kW + x) * 4;
    return {p[i], p[i + 1], p[i + 2], p[i + 3]};
}

const char* kTextVert = R"GLSL(#version 450
struct Glyph { ivec4 rect; vec4 uv; };
layout(push_constant) uniform Push { uint count; Glyph g[3]; } push;
layout(location = 0) out vec2 uv;
void main() {
    const ivec2 corners[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(1, 1), ivec2(0, 1));
    const int tri[6] = int[6](0, 1, 2, 0, 2, 3);
    int gi = gl_VertexIndex / 6;
    ivec2 c = corners[tri[gl_VertexIndex % 6]];
    Glyph gly = push.g[gi];
    vec2 p = vec2(float(gly.rect.x) + float(c.x) * float(gly.rect.z),
                  float(gly.rect.y) + float(c.y) * float(gly.rect.w));
    // Vulkan NDC y is DOWN: framebuffer row r maps to y_ndc = 2r/H - 1.
    // The usual OpenGL flip (1 - ...) would mirror the text to the bottom.
    gl_Position = vec4(p.x / 32.0 - 1.0, p.y / 32.0 - 1.0, 0.5, 1.0);
    uv = vec2(c.x == 0 ? gly.uv.x : gly.uv.z, c.y == 0 ? gly.uv.y : gly.uv.w);
}
)GLSL";

const char* kTextFrag = R"GLSL(#version 450
layout(binding = 0) uniform sampler2D atlas;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    // Atlas alpha = glyph coverage. mix() against the exact clear color:
    // coverage 0 reproduces the clear byte-exact, coverage 1 is white.
    float a = texture(atlas, uv).a;
    const vec3 kBg = vec3(0.25, 0.25, 0.375);
    color = vec4(mix(kBg, vec3(1.0), a), 1.0);
}
)GLSL";
} // namespace

TEST_CASE("gfx_text_draw") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 text test skipped");
        return;
    }
    // ---- 1. cook the vendored font + our two shaders, one pass --------------
    const auto font_src =
        std::filesystem::path(VE_SOURCE_DIR) / "assets/fonts/DejaVuSansMono.ttf";
    REQUIRE(std::filesystem::exists(font_src));
    const std::string src = tmp_root("src");
    const std::string out = tmp_root("out");
    std::string err;
    std::filesystem::copy_file(font_src, std::filesystem::path(src) / "DejaVuSansMono.ttf");
    write_file(std::filesystem::path(src) / "text.vert", kTextVert);
    write_file(std::filesystem::path(src) / "text.frag", kTextFrag);
    auto cook = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(cook.ok, cook.error);

    ve::cook::Unit u_font;
    REQUIRE_MESSAGE(ve::cook::read_unit_file(out + "/units/DejaVuSansMono.ttf.ck", u_font, &err), err);
    ve::test::Font0 f;
    REQUIRE(ve::test::parse_font0_payload(u_font.payload, f));

    // ---- 2. atlas upload through the 3.7 texture path (NEAREST sampler) ----
    ve::gfx::CpuTexture cpu;
    cpu.width = f.atlas_w;
    cpu.height = f.atlas_h;
    cpu.format = ve::core::cooked::kTexFmtRgba8;
    cpu.pixels = f.atlas;

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    auto tex = ve::gfx::gpu_texture::create(*dev, cpu);
    REQUIRE_MESSAGE(tex.has_value(), tex.error());

    ve::cook::Unit u_v, u_f;
    REQUIRE(ve::cook::read_unit_file(out + "/units/text.vert.ck", u_v, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/text.frag.ck", u_f, &err));
    auto sm_v = ve::gfx::shader_module::create(*dev, u_v.payload);
    auto sm_f = ve::gfx::shader_module::create(*dev, u_f.payload);
    REQUIRE(sm_v.has_value());
    REQUIRE(sm_f.has_value());

    // ---- 3. sampler set + pipeline layout (push block for glyph table) -----
    auto sampler = ve::gfx::create_static_sampler(*dev);
    REQUIRE_MESSAGE(sampler.has_value(), sampler.error());
    auto dsl = ve::gfx::sampler_set_layout::create(*dev, 0, VK_SHADER_STAGE_FRAGMENT_BIT);
    REQUIRE_MESSAGE(dsl.has_value(), dsl.error());
    const VkDescriptorSetLayout set_layouts[1] = {dsl->handle()};
    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushBlock),
    };
    const VkPipelineLayoutCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .setLayoutCount = 1, .pSetLayouts = set_layouts,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &pci, nullptr, &layout) == VK_SUCCESS);

    auto pool = ve::gfx::descriptor_pool::create(*dev, 4);
    REQUIRE_MESSAGE(pool.has_value(), pool.error());
    auto set = pool->allocate(dsl->handle());
    REQUIRE_MESSAGE(set.has_value(), set.error());
    pool->write_sampler(*set, 0, *sampler, tex->view());

    // ---- 4. build the "Hi" glyph table (integer-aligned quads) --------------
    // Pen model: x integer per glyph (rounded advance), y fixed baseline.
    const char* text = "Hi";
    PushBlock push{};
    std::vector<std::pair<int, int>> boxes; // screen rects for the CPU model
    int pen = kPen0;
    for (const char* ch = text; *ch; ++ch) {
        const auto cp = static_cast<std::uint32_t>(static_cast<unsigned char>(*ch));
        const auto& g = f.glyphs[cp - f.first_cp];
        REQUIRE(g.w > 0);
        GlyphPush& gp = push.glyphs[push.count++];
        const int l = pen + g.bearing_x;
        const int t = kBaseline - g.bearing_y;
        gp.rect[0] = l; gp.rect[1] = t; gp.rect[2] = g.w; gp.rect[3] = g.h;
        // Texel-center UVs: NEAREST picks atlas columns [ax, ax+w) exactly.
        gp.uv[0] = (g.x + 0.5f) / static_cast<float>(f.atlas_w);
        gp.uv[1] = (g.y + 0.5f) / static_cast<float>(f.atlas_h);
        gp.uv[2] = (g.x + g.w - 0.5f) / static_cast<float>(f.atlas_w);
        gp.uv[3] = (g.y + g.h - 0.5f) / static_cast<float>(f.atlas_h);
        boxes.emplace_back(l, t);
        pen = static_cast<int>(std::lround(pen + g.advance));
    }
    CHECK(push.count == 2);

    // ---- 5. pipeline (no vertex input; 6 verts per glyph quad) --------------
    ve::gfx::pipeline_desc desc{};
    desc.layout = layout;
    desc.stages = {
        {VK_SHADER_STAGE_VERTEX_BIT, sm_v->handle(), "main"},
        {VK_SHADER_STAGE_FRAGMENT_BIT, sm_f->handle(), "main"},
    };
    desc.cull = VK_CULL_MODE_NONE; // quad winding: two triangles, both kept
    desc.depth_test = false;
    desc.depth_write = false;
    desc.color_formats = {VK_FORMAT_B8G8R8A8_UNORM};
    desc.blends.emplace_back();
    desc.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);

    auto pres = ve::gfx::offscreen_presenter::create(*dev, kW, kH);
    REQUIRE_MESSAGE(pres.has_value(), pres.error());
    ve::gfx::pipeline_cache cache(*dev);
    auto pipe = cache.get(desc);
    REQUIRE_MESSAGE(pipe.has_value(), pipe.error());

    // ---- 6. draw -------------------------------------------------------------
    VkCommandBuffer cmd = pres->begin_command();
    REQUIRE(cmd != VK_NULL_HANDLE);
    REQUIRE(pres->begin_pass(cmd));

    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = pres->color_view();
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {0.25f, 0.25f, 0.375f, 1.0f}; // == kClearBgra bytes

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {static_cast<std::uint32_t>(kW), static_cast<std::uint32_t>(kH)}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);

    const VkViewport vpt{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {static_cast<std::uint32_t>(kW), static_cast<std::uint32_t>(kH)}};
    vkCmdSetViewport(cmd, 0, 1, &vpt);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &*set, 0, nullptr);
    vkCmdPushConstants(cmd, layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushBlock), &push);
    vkCmdDraw(cmd, 6u * push.count, 1, 0, 0);

    vkCmdEndRendering(cmd);
    REQUIRE(pres->end_pass(cmd));
    REQUIRE(pres->submit(cmd));

    // ---- 7. CPU atlas model pins (exact where provable) ---------------------
    auto mip0 = tex->capture_mip_rgba8(0); // upload must be byte-exact
    REQUIRE_MESSAGE(mip0.has_value(), mip0.error());
    CHECK_MESSAGE(*mip0 == f.atlas, "GPU mip0 != FONT0 atlas");
    const auto px = pres->capture_rgba8(); // B,G,R,A memory order
    if (std::getenv("VE_TEXT_DEBUG")) {
        for (int y = 0; y < kH; ++y) {
            std::string row;
            for (int x = 0; x < kW; ++x) {
                const auto pr = bgra_probe(px, x, y);
                row += (pr == kWhite) ? 'W' : (pr == kClearBgra ? '.' : '?');
            }
            MESSAGE("fb y" << y << " " << row);
        }
    }
    REQUIRE(px.size() == static_cast<std::size_t>(kW) * kH * 4u);
    auto bgra = [&](int x, int y) {
        const auto i = (static_cast<std::size_t>(y) * kW + x) * 4;
        return std::array{px[i], px[i + 1], px[i + 2], px[i + 3]};
    };

    int lit = 0, exact_checks = 0;
    auto in_box = [&](int x, int y) {
        for (std::size_t gi = 0; gi < boxes.size(); ++gi) {
            const auto& g = f.glyphs[static_cast<unsigned char>(text[gi]) - f.first_cp];
            const int l = boxes[gi].first, t = boxes[gi].second;
            if (x >= l && x < l + g.w && y >= t && y < t + g.h)
                return static_cast<int>(gi);
        }
        return -1;
    };
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const auto got = bgra(x, y);
            const int gi = in_box(x, y);
            if (gi < 0) {
                // Outside every quad: exact clear color (no blend state, no bleed).
                CHECK(got == kClearBgra);
                continue;
            }
            const auto& g = f.glyphs[static_cast<unsigned char>(text[gi]) - f.first_cp];
            const std::size_t ax = static_cast<std::size_t>(g.x) + (x - boxes[gi].first);
            const std::size_t ay = static_cast<std::size_t>(g.y) + (y - boxes[gi].second);
            const std::uint8_t cov = f.atlas[(ay * f.atlas_w + ax) * 4 + 3];
            if (cov == 0) {
                CHECK_MESSAGE(got == kClearBgra, "x" << x << " y" << y << " bg got "
                    << int(got[0]) << "," << int(got[1]) << "," << int(got[2]) << "," << int(got[3]));
                ++exact_checks;
            } else if (cov == 255) {
                CHECK_MESSAGE(got == kWhite, "x" << x << " y" << y << " wht got "
                    << int(got[0]) << "," << int(got[1]) << "," << int(got[2]) << "," << int(got[3])
                    << " atlas ax,ay " << ax << "," << ay);
                ++exact_checks;
                ++lit;
            }
            // Partial-coverage texels: per-channel fp mix vs RNE; only the
            // golden constrains those (policy: exactness pins stay provable).
        }
    }
    // Thresholds derive from the CPU model, not magic numbers: DejaVu Mono
    // at 16 px has ~1.7 px stems, so AA leaves few fully-covered (cov==255)
    // texels. The per-pixel CHECKs above are the real proof: every cov==255
    // texel drew exact white and every cov==0 texel drew exact background.
    int model_lit = 0, model_exact = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const int gi = in_box(x, y);
            if (gi < 0) continue;
            const auto& g = f.glyphs[static_cast<unsigned char>(text[gi]) - f.first_cp];
            const std::size_t ax = static_cast<std::size_t>(g.x) + (x - boxes[gi].first);
            const std::size_t ay = static_cast<std::size_t>(g.y) + (y - boxes[gi].second);
            const std::uint8_t cov = f.atlas[(ay * f.atlas_w + ax) * 4 + 3];
            if (cov == 255) ++model_lit;
            if (cov == 0 || cov == 255) ++model_exact;
        }
    CHECK(model_lit > 15);    // the model itself guarantees visible ink
    CHECK(lit == model_lit);
    CHECK(exact_checks == model_exact);

    // ---- 8. golden (strict byte-exact on lavapipe, decision #35) ------------
    std::vector<std::uint8_t> rgba(px.size());
    for (std::size_t i = 0; i < px.size(); i += 4u) {
        rgba[i + 0] = px[i + 2];
        rgba[i + 1] = px[i + 1];
        rgba[i + 2] = px[i + 0];
        rgba[i + 3] = px[i + 3];
    }
    const auto gv = gfx_golden::check_golden("text_64.png", rgba.data(), kW, kH, {});
    CHECK_MESSAGE(gv.ok, gv.message);

    cache.clear();
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);
    vkDestroySampler(dev->handle(), *sampler, nullptr);
    gfx_test::check_clean_validation(*inst);
}
