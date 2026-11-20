// test: gfx_ui_immediate — 3.13 immediate-mode debug UI.
// CPU half (runs anywhere): the ui_context draw list is pinned — deterministic
// byte-for-byte across contexts given the same io, exact integer-pixel layout
// (window chrome, button, checkbox rects), full click lifecycle
// (hover -> press -> release-over click, press-off cancels), checkbox toggle,
// text-field focus/edit/commit, window close via title [x]. The font the UI
// lays out with arrives ONLY through the runtime FONT0 loader — corrupt
// input there is a clean load error, never a crash.
// GPU half (Tier 2, lavapipe): one render pass draws the frame's command
// list onto a presenter target; pins where provable (exact chrome colors,
// exact glyph pixels at atlas coverage 0/255 through the blend) plus the
// strict ui_64.png golden (decision #35).
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gfx_golden.h"

#include "../ve_test_portability.h" // getpid

#include <ve/cook/cook.h>
#include <ve/gfx/font.h>
#include <ve/gfx/descriptor.h>
#include <ve/gfx/pipeline_cache.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>
#include <ve/gfx/texture.h>
#include <ve/gfx/ui.h>
#include <ve/gfx/ui_render.h>

#include "gfx_test_env.h"

#ifndef VE_SOURCE_DIR
#define VE_SOURCE_DIR "."
#endif

namespace {
std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_ui_") + name + "_" + std::to_string(ve_test::pid()));
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

float ch8(int v) { return static_cast<float>(v) / 255.0f; }

// Same io for two contexts must produce byte-identical draw lists.
// Shared widget code for the determinism check: caller-owned state so the
// two runs start from identical values.
void run_widget_code(ve::gfx::ui_context& ui, const ve::gfx::font0& font,
                     const std::vector<ve::gfx::ui_io>& seq,
                     std::vector<ve::gfx::ui_frame>& out_frames,
                     char (&buf)[16], bool& check) {
    for (const auto& io : seq) {
        ui.begin_frame(io, font, 200, 200);
        if (ui.begin_window("Sys", 0, 0, 64, 200)) {
            ui.label("Status");
            ui.button("Go");
            ui.checkbox("On", &check);
            ui.text_field("Name", buf, sizeof buf);
        }
        ui.end_window();
        ui.end_frame();
        out_frames.push_back(ui.frame());
    }
}

bool frames_equal(const ve::gfx::ui_frame& a, const ve::gfx::ui_frame& b) {
    if (a.verts.size() != b.verts.size() || a.indices.size() != b.indices.size() ||
        a.cmds.size() != b.cmds.size())
        return false;
    for (std::size_t i = 0; i < a.verts.size(); ++i) {
        const auto& x = a.verts[i];
        const auto& y = b.verts[i];
        if (x.x != y.x || x.y != y.y || x.u != y.u || x.v != y.v || x.r != y.r ||
            x.g != y.g || x.b != y.b || x.a != y.a)
            return false;
    }
    if (std::memcmp(a.indices.data(), b.indices.data(), a.indices.size() * 4) != 0)
        return false;
    for (std::size_t i = 0; i < a.cmds.size(); ++i) {
        const auto& x = a.cmds[i];
        const auto& y = b.cmds[i];
        if (x.texture_slot != y.texture_slot || x.idx_offset != y.idx_offset ||
            x.idx_count != y.idx_count || x.clip_x != y.clip_x || x.clip_y != y.clip_y ||
            x.clip_w != y.clip_w || x.clip_h != y.clip_h)
            return false;
    }
    return true;
}
} // namespace

TEST_CASE("gfx_ui_immediate") {
    // ---- 1. cook the font (no external tool) + UI shaders (GLSL tool) --------
    // Font cooking needs nothing external; the shaders need a GLSL compiler, and
    // its absence is an environment gap rather than a gfx fault — the same
    // tiering the cook golden already uses (tests/tools/test_shader_cook.cpp,
    // #36). Cooking the font FIRST keeps the CPU half of this case running on a
    // box without a compiler instead of silently losing that coverage.
    const auto font_src =
        std::filesystem::path(VE_SOURCE_DIR) / "assets/fonts/DejaVuSansMono.ttf";
    REQUIRE(std::filesystem::exists(font_src));
    const std::string src = tmp_root("src");
    const std::string out = tmp_root("out");
    std::string err;
    std::filesystem::copy_file(font_src, std::filesystem::path(src) / "DejaVuSansMono.ttf");
    auto font_cook = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(font_cook.ok, font_cook.error);

    // The shader cook is the only step that shells out, so it is probed rather
    // than assumed (probe semantics in ve_test::tool_on_path; VE_GLSLANG may
    // name a PATH entry or an absolute path). Cooked before broken.ck is written
    // below, so the source walk only ever sees real sources.
    const std::string glslang = [] {
        const char* v = std::getenv("VE_GLSLANG");
        return (v != nullptr && *v != '\0') ? std::string(v) : std::string("glslangValidator");
    }();
    const bool have_glslang = ve_test::tool_on_path(glslang);
    if (have_glslang) {
        write_file(std::filesystem::path(src) / "ui.vert", ve::gfx::kUi_vert_glsl);
        write_file(std::filesystem::path(src) / "ui.frag", ve::gfx::kUi_frag_glsl);
        auto shader_cook = ve::cook::cook_incremental(src, out);
        REQUIRE_MESSAGE(shader_cook.ok, shader_cook.error);
    }

    ve::gfx::font0 font;
    REQUIRE_MESSAGE(ve::gfx::load_cooked_font_file(out + "/units/DejaVuSansMono.ttf.ck",
                                                   font, &err),
                    err);
    CHECK(font.ascent == 13);
    CHECK(font.glyph_count == 95);

    // Corrupt unit: clean load error, no crash (bounds-checked reader).
    write_file(std::filesystem::path(src) / "broken.ck", std::string(256, '\x7A'));
    ve::gfx::font0 junk;
    std::string err2;
    CHECK_FALSE(ve::gfx::load_cooked_font_file(src + "/broken.ck", junk, &err2));

    // ---- 2. determinism: same io -> identical draw lists on fresh contexts ---
    std::vector<ve::gfx::ui_io> seq;
    seq.push_back({25.f, 70.f, false, {}, false, false, false});
    seq.push_back({25.f, 70.f, true, {'H', 'i'}, false, false, false});
    seq.push_back({25.f, 70.f, false, {}, true, true, false});
    std::vector<ve::gfx::ui_frame> fa, fb;
    {
        ve::gfx::ui_context u1, u2;
        char buf1[16] = "", buf2[16] = "";
        bool check1 = false, check2 = false;
        run_widget_code(u1, font, seq, fa, buf1, check1);
        run_widget_code(u2, font, seq, fb, buf2, check2);
    }
    REQUIRE(fa.size() == seq.size());
    CHECK(frames_equal(fa[0], fb[0]));
    CHECK(frames_equal(fa[1], fb[1]));
    CHECK(frames_equal(fa[2], fb[2]));
    CHECK_FALSE(fa[2].verts.empty()); // widgets actually emitted

    // ---- 3. exact geometry pins (integer-pixel layout, merged solid run) -----
    {
        ve::gfx::ui_context ui;
        ve::gfx::ui_io io{.mouse_x = 0.f, .mouse_y = 0.f, .chars = {}}; // outside window (2,2)
        ui.begin_frame(io, font, kW, kH);
        REQUIRE(ui.begin_window("Sys", 2, 2, 60, 58));
        ui.button("Go");
        bool check = true;
        ui.checkbox("On", &check);
        ui.end_window();
        ui.end_frame();
        const auto& f = ui.frame();
        // Chrome run: title (2,2,60,16) + body (2,18,60,40) + close (46,2,16,16)
        // all solid slot 0 with the same clip -> one merged command.
        REQUIRE(!f.cmds.empty());
        CHECK(f.cmds[0].texture_slot == 0);
        CHECK(f.cmds[0].idx_count >= 18);
        CHECK(f.cmds[0].clip_x == 2);
        CHECK(f.cmds[0].clip_h == 58);
        REQUIRE(f.verts.size() >= 12);
        CHECK(f.verts[0].x == 2.f);  CHECK(f.verts[0].y == 2.f);   // title TL
        CHECK(f.verts[1].x == 62.f); CHECK(f.verts[1].y == 2.f);   // title TR
        CHECK(f.verts[3].x == 2.f);  CHECK(f.verts[3].y == 18.f);  // title BL
        CHECK(f.verts[4].y == 18.f);                               // body top edge
        CHECK(f.verts[7].y == 60.f);                               // body bottom
        CHECK(f.verts[0].r == doctest::Approx(ch8(50)).epsilon(1e-6));
        // Button row: cur = (10,26); width = advances("Go") + 2*pad.
        // Engine layout rule (gfx/src/ui.cpp:148-153): the f32 advances are summed as
        // float and truncated ONCE for the rect width. The previous `int tw += advance`
        // truncated per glyph (MSVC /WX rejects that implicit narrowing; GCC's wall,
        // which has no -Wconversion, let it through) and only coincidentally matched:
        // the cooked advance is 8.27515 px, so 2 glyphs give sum 16.5503 -> int 16,
        // while per-glyph truncation gives 8 + 8 = 16. Assert the engine's value.
        float tw = 0.f;
        for (const char* p = "Go"; *p; ++p) tw += font.glyph(static_cast<unsigned char>(*p))->advance;
        CHECK(static_cast<int>(tw) == 16); // DejaVu Mono 16px: 8.27515 advance/cp -> int(sum)
        // (solid run may also include the button rect; locate it by its x span)
        // A glyph-text command exists (title text) on slot 1:
        bool saw_text_cmd = false;
        for (const auto& cmd : f.cmds)
            if (cmd.texture_slot == 1) saw_text_cmd = true;
        CHECK(saw_text_cmd);
    }

    // ---- 4. interaction lifecycle -------------------------------------------
    {
        ve::gfx::ui_context ui;
        // Button rect for a (0,0,64,200) window: cur=(10, 16+8)= (10,24), h=18.
        const float bx = 20.f, by = 24.f + 9.f; // inside button
        bool clicked = false;
        for (int frame = 0; frame < 5 && !clicked; ++frame) {
            ve::gfx::ui_io io{.mouse_x = bx, .mouse_y = by, .mouse_down = frame == 1, .chars = {}};
            ui.begin_frame(io, font, 200, 200);
            ui.begin_window("Sys", 0, 0, 64, 200);
            if (ui.button("Go")) clicked = true; // press on frame1, release frame2
            ui.end_window();
            ui.end_frame();
        }
        CHECK(clicked); // hover -> press -> release-over = exactly one click

        // Press starting OFF the button never clicks it even if released over it.
        ve::gfx::ui_context ui2;
        bool off_click = false, off_click2 = false;
        { // frame A: press outside, still down
            ve::gfx::ui_io io{.mouse_x = 2.f, .mouse_y = 100.f, .mouse_down = true, .chars = {}};
            ui2.begin_frame(io, font, 200, 200);
            ui2.begin_window("Sys", 0, 0, 64, 200);
            if (ui2.button("Go")) off_click = true;
            ui2.end_window(); ui2.end_frame();
        }
        { // frame B: release INSIDE the button
            ve::gfx::ui_io io{.mouse_x = 20.f, .mouse_y = 33.f, .mouse_down = false, .chars = {}};
            ui2.begin_frame(io, font, 200, 200);
            ui2.begin_window("Sys", 0, 0, 64, 200);
            if (ui2.button("Go")) off_click2 = true;
            ui2.end_window(); ui2.end_frame();
        }
        CHECK_FALSE(off_click);
        CHECK_FALSE(off_click2); // press-capture means release must match the press target

        // Checkbox: one toggle per full click; model value follows.
        ve::gfx::ui_context ui3;
        bool on = false, toggled = false;
        for (int frame = 0; frame < 3; ++frame) {
            ve::gfx::ui_io io{.mouse_x = 12.f, .mouse_y = 24.f + 9.f, .mouse_down = frame == 0, .chars = {}};
            ui3.begin_frame(io, font, 200, 200);
            ui3.begin_window("Sys", 0, 0, 64, 200);
            if (ui3.checkbox("On", &on)) toggled = true;
            ui3.end_window(); ui3.end_frame();
        }
        CHECK(toggled);
        CHECK(on);

        // Text field: click focuses; chars edit; backspace; Enter commits.
        ve::gfx::ui_context ui4;
        char buf[16] = "";
        // field box for that layout: label "Name" at y24, box top 24+16+2=42.
        { ve::gfx::ui_io io{.mouse_x = 20.f, .mouse_y = 45.f, .mouse_down = true, .chars = {}};
          ui4.begin_frame(io, font, 200, 200); ui4.begin_window("Sys", 0, 0, 64, 200);
          ui4.text_field("Name", buf, 16); ui4.end_window(); ui4.end_frame(); }
        { ve::gfx::ui_io io{.mouse_x = 20.f, .mouse_y = 45.f, .mouse_down = false, .chars = {}};
          ui4.begin_frame(io, font, 200, 200); ui4.begin_window("Sys", 0, 0, 64, 200);
          ui4.text_field("Name", buf, 16); ui4.end_window(); ui4.end_frame(); }
        { ve::gfx::ui_io io{.mouse_x = 0.f, .mouse_y = 0.f, .mouse_down = false, .chars = {'h', 'i'}};
          ui4.begin_frame(io, font, 200, 200); ui4.begin_window("Sys", 0, 0, 64, 200);
          ui4.text_field("Name", buf, 16); ui4.end_window(); ui4.end_frame(); }
        bool committed = false;
        { ve::gfx::ui_io io{.mouse_x = 0.f, .mouse_y = 0.f, .mouse_down = false,
                            .chars = {'!'}, .backspace = true, .enter = true};
          ui4.begin_frame(io, font, 200, 200); ui4.begin_window("Sys", 0, 0, 64, 200);
          committed = ui4.text_field("Name", buf, 16);
          ui4.end_window(); ui4.end_frame(); }
        CHECK(committed);
        CHECK(std::strcmp(buf, "hi") == 0);

        // Window close via title-bar [x].
        ve::gfx::ui_context ui5;
        bool open = true;
        for (int frame = 0; frame < 2; ++frame) {
            ve::gfx::ui_io io{.mouse_x = 64.f - 8.f, .mouse_y = 8.f, .mouse_down = frame == 0, .chars = {}};
            ui5.begin_frame(io, font, 200, 200);
            if (ui5.begin_window("Sys", 0, 0, 64, 200, &open)) ui5.label("body");
            ui5.end_window();
            ui5.end_frame();
        }
        CHECK_FALSE(open);
    }

    // ---- 5. GPU half (Tier 2, lavapipe only) ---------------------------------
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 UI draw skipped");
        return;
    }
    if (!have_glslang) {
        MESSAGE("no GLSL compiler reachable ('" << glslang
                << "') — Tier-2 UI draw skipped; install glslang-tools or point VE_GLSLANG at one");
        return;
    }

    ve::gfx::CpuTexture atlas{font.atlas_w, font.atlas_h,
                              ve::core::cooked::kTexFmtRgba8, font.atlas};
    ve::gfx::CpuTexture white{1, 1, ve::core::cooked::kTexFmtRgba8,
                              {255, 255, 255, 255}};

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());
    auto tex_font = ve::gfx::gpu_texture::create(*dev, atlas);
    REQUIRE_MESSAGE(tex_font.has_value(), tex_font.error());
    auto tex_white = ve::gfx::gpu_texture::create(*dev, white);
    REQUIRE_MESSAGE(tex_white.has_value(), tex_white.error());

    // EK.1: the render pass itself is engine code (gfx::ui_renderer); this
    // test now proves it from outside, exactly as before the promotion.
    ve::cook::Unit u_v, u_f;
    REQUIRE(ve::cook::read_unit_file(out + "/units/ui.vert.ck", u_v, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/ui.frag.ck", u_f, &err));
    auto rend = ve::gfx::ui_renderer::create(
        *dev, *tex_white, *tex_font, u_v.payload, u_f.payload, VK_FORMAT_B8G8R8A8_UNORM);
    REQUIRE_MESSAGE(rend.has_value(), rend.error());

    // Build the golden frame's draw list (same code the pins above verified).
    ve::gfx::ui_context ui;
    ve::gfx::ui_io io{.mouse_x = 0.f, .mouse_y = 0.f, .chars = {}};
    ui.begin_frame(io, font, kW, kH);
    REQUIRE(ui.begin_window("Sys", 2, 2, 60, 58));
    ui.button("Go");
    bool check = true;
    ui.checkbox("On", &check);
    ui.end_window();
    ui.end_frame();
    const auto& frame = ui.frame();
    REQUIRE(!frame.cmds.empty());

    auto pres = ve::gfx::offscreen_presenter::create(*dev, kW, kH);
    REQUIRE_MESSAGE(pres.has_value(), pres.error());

    VkCommandBuffer cmd = pres->begin_command();
    REQUIRE(cmd != VK_NULL_HANDLE);
    REQUIRE(pres->begin_pass(cmd));

    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = pres->color_view();
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {ch8(10), ch8(10), ch8(16), 1.0f};

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {static_cast<std::uint32_t>(kW), static_cast<std::uint32_t>(kH)}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    REQUIRE(rend->draw(cmd, frame, kW, kH));
    vkCmdEndRendering(cmd);
    REQUIRE(pres->end_pass(cmd));
    REQUIRE(pres->submit(cmd));

    const auto px = pres->capture_rgba8(); // B,G,R,A memory order
    REQUIRE(px.size() == static_cast<std::size_t>(kW) * kH * 4);
    auto bgra = [&](int x, int y) -> std::uint32_t {
        const auto i = (static_cast<std::size_t>(y) * kW + x) * 4;
        return px[i] | (px[i + 1] << 8) | (px[i + 2] << 16) | (px[i + 3] << 24);
    };
    auto exact = [&](int x, int y, int r, int g, int b) {
        const std::uint32_t want = static_cast<std::uint32_t>(b) |
                                   (static_cast<std::uint32_t>(g) << 8) |
                                   (static_cast<std::uint32_t>(r) << 16) | (255u << 24);
        CHECK_MESSAGE(bgra(x, y) == want, "px " << x << "," << y << " got "
                                                << std::hex << bgra(x, y) << " want " << want);
    };

    // Clear outside the window; chrome exact; glyph pixels exact where the
    // atlas coverage is 0 or 255 (integer-aligned quads sample exactly one
    // texel, so the blend resolves to an exact endpoint).
    exact(0, 0, 10, 10, 16);
    exact(44, 8, 50, 55, 70);   // title bar (bare: after the 3 title glyphs, before the close btn)
    exact(32, 46, 38, 42, 55);  // body (gap row between button bottom 44 and checkbox top 50)
    exact(16, 57, 110, 200, 130); // checkbox inner square (checked)
    // Layout mirrors the engine exactly (gfx/src/ui.cpp:126-135): pen is a float
    // summed from f32 advances, and the glyph box origin is int(pen) — truncation
    // is the documented rule, so it gets an explicit cast rather than a silent
    // narrowing (MSVC /WX rejects the implicit one; advances here are whole
    // pixels, so int(pen) equals the previous int pen arithmetic bit for bit).
    float pen = 2.f + ve::gfx::ui_context::kPad;
    const int base = 2 + (16 - (font.ascent - font.descent)) / 2 + font.ascent;
    for (const char* s = "Sys"; *s; ++s) {
        const auto* g = font.glyph(static_cast<unsigned char>(*s));
        REQUIRE(g);
        if (g->w == 0) continue;
        const int l = static_cast<int>(pen);
        for (int yy = 0; yy < g->h; ++yy) {
            for (int xx = 0; xx < g->w; ++xx) {
                const auto cov = font.atlas[(static_cast<std::size_t>(g->y + yy) * font.atlas_w +
                                             g->x + xx) * 4 + 3];
                const int sx = l + g->bearing_x + xx, sy = base - g->bearing_y + yy;
                if (sx < 0 || sx >= kW || sy < 0 || sy >= kH) continue;
                if (cov == 255) exact(sx, sy, 232, 234, 240);
                else if (cov == 0) {
                    // covered by title bar only while inside the title strip
                    if (sy >= 2 && sy < 18) exact(sx, sy, 50, 55, 70);
                }
            }
        }
        pen += g->advance; // advance the pen for the next glyph
    }

    // golden (strict byte-exact on lavapipe, decision #35)
    std::vector<std::uint8_t> rgba(px.size());
    for (std::size_t i = 0; i < px.size(); i += 4) {
        rgba[i + 0] = px[i + 2];
        rgba[i + 1] = px[i + 1];
        rgba[i + 2] = px[i + 0];
        rgba[i + 3] = px[i + 3];
    }
    const auto gv = gfx_golden::check_golden("ui_64.png", rgba.data(), kW, kH, {});
    CHECK_MESSAGE(gv.ok, gv.message);

    // Engine-surface contracts that used to be inline REQUIREs in this test;
    // post-promotion they pin ui_renderer itself.
    {
        // (1) corrupt SHDR fails cleanly, never reaching the driver.
        auto bad = u_v.payload;
        bad[4] = 0x00; // break SPIR-V magic
        auto rbad = ve::gfx::ui_renderer::create(
            *dev, *tex_white, *tex_font, bad, u_f.payload, VK_FORMAT_B8G8R8A8_UNORM);
        REQUIRE(!rbad.has_value());
        REQUIRE_MESSAGE(!rbad.error().empty(), "create failure must carry an error string");

        // (2) degenerate inputs: false BEFORE touching cmd (no cmd needed).
        ve::gfx::ui_frame empty;
        CHECK(!rend->draw(VK_NULL_HANDLE, empty, kW, kH));
        CHECK(!rend->draw(VK_NULL_HANDLE, frame, 0, kH));
        CHECK(!rend->draw(VK_NULL_HANDLE, frame, kW, -1));

        // (3) host VB/IB grow-on-demand: same renderer, frame with >64 KiB of
        // vertex data appended (cmds unchanged -> drawn geometry identical).
        // Proves the old host buffers die only while the device is idle, and
        // the re-render stays byte-identical through the recreate.
        ve::gfx::ui_frame big = frame;
        while (big.verts.size() * sizeof(ve::gfx::ui_vertex) <= 65536u) {
            // (vertex_base, not base: the enclosing TEST_CASE already has an
            // int `base` for the glyph baseline — MSVC C4456 under /W4 /WX.
            // The index add is done in the index vector's own type so MSVC
            // does not flag the size_t -> uint32_t narrowing C4267.)
            const auto vertex_base = big.verts.size();
            big.verts.insert(big.verts.end(), frame.verts.begin(), frame.verts.end());
            for (const auto ix : frame.indices)
                big.indices.push_back(ix + static_cast<decltype(ix)>(vertex_base));
        }
        REQUIRE(big.verts.size() > frame.verts.size());
        auto pres2 = ve::gfx::offscreen_presenter::create(*dev, kW, kH);
        REQUIRE_MESSAGE(pres2.has_value(), pres2.error());
        VkCommandBuffer cmd2 = pres2->begin_command();
        REQUIRE(cmd2 != VK_NULL_HANDLE);
        REQUIRE(pres2->begin_pass(cmd2));
        VkRenderingAttachmentInfo att2{};
        att2.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att2.imageView = pres2->color_view();
        att2.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att2.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att2.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att2.clearValue.color = {ch8(10), ch8(10), ch8(16), 1.0f};
        VkRenderingInfo ri2{};
        ri2.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri2.renderArea = {{0, 0}, {static_cast<std::uint32_t>(kW), static_cast<std::uint32_t>(kH)}};
        ri2.layerCount = 1;
        ri2.colorAttachmentCount = 1;
        ri2.pColorAttachments = &att2;
        vkCmdBeginRendering(cmd2, &ri2);
        REQUIRE(rend->draw(cmd2, big, kW, kH));
        vkCmdEndRendering(cmd2);
        REQUIRE(pres2->end_pass(cmd2));
        REQUIRE(pres2->submit(cmd2));
        const auto px2 = pres2->capture_rgba8();
        REQUIRE(px2.size() == px.size());
        CHECK(std::memcmp(px2.data(), px.data(), px.size()) == 0); // grown-buffer render identical (raw BGRA)
        auto bgra2 = [&](int x, int y) -> std::uint32_t {
            const auto i = (static_cast<std::size_t>(y) * kW + x) * 4;
            return px2[i] | (px2[i + 1] << 8) | (px2[i + 2] << 16) | (px2[i + 3] << 24);
        };
        auto exact2 = [&](int x, int y, int r, int g, int b) {
            const std::uint32_t want = static_cast<std::uint32_t>(b) |
                                       (static_cast<std::uint32_t>(g) << 8) |
                                       (static_cast<std::uint32_t>(r) << 16) | (255u << 24);
            CHECK_MESSAGE(bgra2(x, y) == want, "px2 " << x << "," << y);
        };
        exact2(0, 0, 10, 10, 16);
        exact2(44, 8, 50, 55, 70);
        exact2(16, 57, 110, 200, 130);
    }

    // rend/vb/ib teardown is the renderer's destructor (engine-owned).
}

TEST_CASE("ui_line_primitive") {
    // 8.20.0: the schematic map's stroke primitive. CPU half only —
    // draw-list pins, no GPU pass (the big gfx_ui_immediate case already
    // proves slot-0 quads rasterize; a line is the same quad shape, rotated).
    const auto font_src =
        std::filesystem::path(VE_SOURCE_DIR) / "assets/fonts/DejaVuSansMono.ttf";
    REQUIRE(std::filesystem::exists(font_src));
    const std::string src = tmp_root("line_src");
    const std::string out = tmp_root("line_out");
    std::filesystem::copy_file(font_src, std::filesystem::path(src) / "DejaVuSansMono.ttf");
    std::string err;
    REQUIRE_MESSAGE(ve::cook::cook_incremental(src, out).ok, err);
    ve::gfx::font0 font;
    REQUIRE_MESSAGE(ve::gfx::load_cooked_font_file(out + "/units/DejaVuSansMono.ttf.ck",
                                                   font, &err),
                    err);

    ve::gfx::ui_io io{0.f, 0.f, false, {}, false, false, false};
    ve::gfx::ui_context ui;
    ui.begin_frame(io, font, 200, 200);
    REQUIRE(ui.begin_window("Map", 10, 10, 180, 180));
    const std::size_t v_in = ui.frame().verts.size();  // window chrome emitted
    const std::size_t c_in = ui.frame().cmds.size();
    ui.line(20, 20, 120, 70, 255, 128, 0, 3);
    ui.line(30, 30, 30, 30, 1, 2, 3);   // zero-length: nothing emitted
    ui.line(0, 0, 5, 0, 9, 9, 9);       // inside window: emitted
    ui.end_window();
    ui.line(50, 50, 60, 60, 0, 0, 0);   // outside any window: nothing
    ui.end_frame();
    const auto& f = ui.frame();

    // Two lines -> 8 verts / 12 indices / 2 cmds appended; colors pinned on
    // the first quad's verts (flat fill: r/g/b/a all four identical).
    CHECK(f.verts.size() == v_in + 8);
    CHECK(f.indices.size() == (v_in / 4) * 6 + 12);
    CHECK(f.cmds.size() == c_in + 2);
    // dx=100 dy=50 len=hypot: normal (−dy,dx)/len * half(thickness=3)=1.5.
    const double len = std::hypot(100.0, 50.0);
    const double nx = -50.0 / len * 1.5, ny = 100.0 / len * 1.5;
    const auto& q = f.verts[f.verts.size() - 8];
    CHECK(q.x == doctest::Approx(20.0 + nx).epsilon(1e-6));
    CHECK(q.y == doctest::Approx(20.0 + ny).epsilon(1e-6));
    CHECK(q.r == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(q.g == doctest::Approx(128.0 / 255.0).epsilon(1e-6));
    CHECK(q.b == doctest::Approx(0.0));
    CHECK(q.a == doctest::Approx(1.0).epsilon(1e-6));
    // Second line's cmd: own draw call, window client clip.
    const auto& cmd = f.cmds[f.cmds.size() - 1];
    CHECK(cmd.texture_slot == 0);
    CHECK(cmd.idx_count == 6);
    CHECK(cmd.clip_x == 10);
    CHECK(cmd.clip_y == 10);
}
