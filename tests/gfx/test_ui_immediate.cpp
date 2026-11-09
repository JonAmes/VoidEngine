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
#include <ve/gfx/descriptor.h>
#include <ve/gfx/font.h>
#include <ve/gfx/pipeline_cache.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>
#include <ve/gfx/texture.h>
#include <ve/gfx/ui.h>

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

// UI render shader: pixels (top-left origin) -> Vulkan NDC (y DOWN); the
// alpha that blends is the texture's (atlas coverage; solid slot alpha is 1).
const char* kUiVert = R"GLSL(#version 450
layout(location = 0) in vec2 pos;   // pixels, top-left origin
layout(location = 1) in vec2 uvin;  // texcenter uv
layout(location = 2) in vec2 colrg;
layout(location = 3) in vec2 colba;
layout(push_constant) uniform Push { float sx; float sy; };  // 2/W, 2/H
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;
void main() {
    uv = uvin;
    color = vec4(colrg, colba);
    gl_Position = vec4(pos.x * sx - 1.0, pos.y * sy - 1.0, 0.5, 1.0);
}
)GLSL";

const char* kUiFrag = R"GLSL(#version 450
layout(binding = 0) uniform sampler2D t;  // slot 0: solid white, slot 1: atlas
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(color.rgb, color.a * texture(t, uv).a);
}
)GLSL";

float ch8(int v) { return static_cast<float>(v) / 255.0f; }

// Host-visible buffer for the UI draw lists (test-local; engine-owned buffer
// types already exist for meshes/uniforms, this is just the test's plumbing).
struct host_buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    void create(const ve::gfx::device& dev, VkDeviceSize size, VkBufferUsageFlags usage) {
        const VkBufferCreateInfo bi{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr, .flags = 0, .size = size, .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr};
        REQUIRE(vkCreateBuffer(dev.handle(), &bi, nullptr, &buf) == VK_SUCCESS);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev.handle(), buf, &req);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(dev.physical(), &mp);
        std::uint32_t mt = 0xFFFFFFFFu;
        for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                mt = i;
                break;
            }
        }
        REQUIRE(mt != 0xFFFFFFFFu);
        const VkMemoryAllocateInfo ai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr, .allocationSize = req.size, .memoryTypeIndex = mt};
        REQUIRE(vkAllocateMemory(dev.handle(), &ai, nullptr, &mem) == VK_SUCCESS);
        REQUIRE(vkBindBufferMemory(dev.handle(), buf, mem, 0) == VK_SUCCESS);
        REQUIRE(vkMapMemory(dev.handle(), mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS);
    }
    void destroy(const ve::gfx::device& dev) {
        vkDestroyBuffer(dev.handle(), buf, nullptr);
        vkFreeMemory(dev.handle(), mem, nullptr);
    }
};

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
        write_file(std::filesystem::path(src) / "ui.vert", kUiVert);
        write_file(std::filesystem::path(src) / "ui.frag", kUiFrag);
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

    ve::cook::Unit u_v, u_f;
    REQUIRE(ve::cook::read_unit_file(out + "/units/ui.vert.ck", u_v, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/ui.frag.ck", u_f, &err));
    auto sm_v = ve::gfx::shader_module::create(*dev, u_v.payload);
    auto sm_f = ve::gfx::shader_module::create(*dev, u_f.payload);
    REQUIRE(sm_v.has_value());
    REQUIRE(sm_f.has_value());

    auto sampler = ve::gfx::create_static_sampler(*dev);
    REQUIRE_MESSAGE(sampler.has_value(), sampler.error());
    auto dsl = ve::gfx::sampler_set_layout::create(*dev, 0, VK_SHADER_STAGE_FRAGMENT_BIT);
    REQUIRE_MESSAGE(dsl.has_value(), dsl.error());
    const VkDescriptorSetLayout set_layouts[2] = {dsl->handle(), dsl->handle()};
    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 8};
    const VkPipelineLayoutCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .setLayoutCount = 2, .pSetLayouts = set_layouts,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &pci, nullptr, &layout) == VK_SUCCESS);

    auto pool = ve::gfx::descriptor_pool::create(*dev, 4);
    REQUIRE_MESSAGE(pool.has_value(), pool.error());
    auto set_solid = pool->allocate(dsl->handle());
    auto set_text = pool->allocate(dsl->handle());
    REQUIRE_MESSAGE(set_solid.has_value(), set_solid.error());
    REQUIRE_MESSAGE(set_text.has_value(), set_text.error());
    pool->write_sampler(*set_solid, 0, *sampler, tex_white->view());
    pool->write_sampler(*set_text, 0, *sampler, tex_font->view());

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

    host_buf vb, ib;
    vb.create(*dev, frame.verts.size() * sizeof(ve::gfx::ui_vertex),
              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ib.create(*dev, frame.indices.size() * sizeof(std::uint32_t),
              VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    std::memcpy(vb.map, frame.verts.data(), frame.verts.size() * sizeof(ve::gfx::ui_vertex));
    std::memcpy(ib.map, frame.indices.data(), frame.indices.size() * 4);

    ve::gfx::pipeline_desc desc{};
    desc.layout = layout;
    desc.stages = {
        {VK_SHADER_STAGE_VERTEX_BIT, sm_v->handle(), "main"},
        {VK_SHADER_STAGE_FRAGMENT_BIT, sm_f->handle(), "main"},
    };
    desc.bindings = {{0, static_cast<std::uint32_t>(sizeof(ve::gfx::ui_vertex)),
                      VK_VERTEX_INPUT_RATE_VERTEX}};
    desc.attributes = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},   // pos px
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},   // uv
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 16},  // color.rg
        {3, 0, VK_FORMAT_R32G32_SFLOAT, 24},  // color.ba
    };
    desc.cull = VK_CULL_MODE_NONE;
    desc.depth_test = false;
    desc.depth_write = false;
    desc.color_formats = {VK_FORMAT_B8G8R8A8_UNORM};
    ve::gfx::blend_attachment_state blend{};
    blend.blend_enable = true;
    blend.src_color = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    // RGB: src-alpha over dst. A: keep the target's alpha (never write UI
    // coverage into it) — src_alpha=0, dst_alpha=1.
    blend.src_alpha = VK_BLEND_FACTOR_ZERO;
    blend.dst_alpha = VK_BLEND_FACTOR_ONE;
    desc.blends = {blend};
    desc.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);

    auto pres = ve::gfx::offscreen_presenter::create(*dev, kW, kH);
    REQUIRE_MESSAGE(pres.has_value(), pres.error());
    ve::gfx::pipeline_cache cache(*dev);
    auto pipe = cache.get(desc);
    REQUIRE_MESSAGE(pipe.has_value(), pipe.error());

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

    const VkViewport vpt{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vpt);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe);
    const float push[2] = {2.0f / kW, 2.0f / kH}; // NDC y-DOWN: row r -> 2r/H-1
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    const VkDeviceSize voff = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buf, &voff);
    vkCmdBindIndexBuffer(cmd, ib.buf, 0, VK_INDEX_TYPE_UINT32);
    for (const auto& c : frame.cmds) {
        const VkDescriptorSet set = c.texture_slot == 0 ? *set_solid : *set_text;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        const VkRect2D sc{{c.clip_x, c.clip_y}, {static_cast<std::uint32_t>(c.clip_w),
                                                 static_cast<std::uint32_t>(c.clip_h)}};
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdDrawIndexed(cmd, c.idx_count, 1, static_cast<std::int32_t>(c.idx_offset), 0, 0);
    }

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

    vb.destroy(*dev);
    ib.destroy(*dev);
    vkDestroySampler(dev->handle(), *sampler, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout, nullptr);
}
