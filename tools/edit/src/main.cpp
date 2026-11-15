// ve_edit — editor-kit host (decision #54, docs/tooling-editorkit checklist).
//
// A LIVE editor window over an EMPTY world: no art beyond the one cooked
// glyph atlas, no 3D viewport, no live sim (#54). This cut carries two of
// the five panels (EK.4: def forms + mode-B schematic); EK.5 adds browser,
// inspector-tree, and loadout.
//
// Self-attesting exit-code contract (the 2.9 pattern — this tool is NEVER a
// ci_local case; tools/dev/xvfb_edit_run.sh drives scripted mode on the dev
// box):
//   0  N frames presented; the scripted form edit round-tripped the REAL
//      cook gate; frame-0 and final captures differ (the edit is visible);
//      captures carry UI ink (the pass actually painted); validation quiet
//      (the driver runs with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;
//      its empty console is the evidence, same claim as 2.9's).
//   2  init failure (no window / device / cook / font)
//   3  scripted edit refused by the gate, or a capture carries no UI ink
//   4  final capture byte-identical to frame-0's (edit did not reach screen)
//
// Scripted mode drives the input_adapter's public event surface directly —
// the same cursor/mouse/character events a real seat's GLFW callbacks
// deliver (EK.2's mapping, exercised from the other side). The GLFW shim is
// installed only when interactive.

#include <cstdlib>
#include <unistd.h> // getpid (temp work dir)
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

// ve/gfx/device.h FIRST: it pulls volk under VK_NO_PROTOTYPES in the right
// order; a raw <vulkan/vulkan.h> here would trip volk's guard (same rule as
// EK.1's header).
#include <ve/gfx/device.h>

#include <ve/cook/author_gizmos.h>
#include <ve/cook/cook.h>
#include <ve/core/log.h>
#include <ve/demo/png_out.h>
#include <ve/game/def_forms_panel.h>
#include <ve/game/def_schema.h>
#include <ve/game/inspector_panel.h>
#include <ve/gfx/font.h>
#include <ve/gfx/instance.h>
#include <ve/gfx/texture.h>
#include <ve/gfx/ui.h>
#include <ve/gfx/ui_render.h>
#include <ve/gfx/wsi_presenter.h>
#include <ve/platform/input_adapter.h>
#include <ve/platform/window.h>

#ifndef VE_SOURCE_DIR
#define VE_SOURCE_DIR "."
#endif

namespace {

using namespace ve;

void fail(const std::string& msg) { VE_LOG_ERROR("ve_edit: %s", msg.c_str()); }

std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_text(const std::filesystem::path& p, const std::string& s) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

// The EK.3 empty world, verbatim — the exact fixture world_empty proves
// passes every production gate. ve_edit boots on it when --src is not given.
constexpr const char* kEmptySys =
    "world_class = \"system\"\n"
    "name = \"Empty\"\n"
    "seed = 1\n"
    "members = []\n";

struct surface_owner {
    gfx::instance* inst = nullptr;
    VkSurfaceKHR* surf = nullptr;
    ~surface_owner() {
        if (inst && surf && *surf != VK_NULL_HANDLE) vkDestroySurfaceKHR(inst->handle(), *surf, nullptr);
    }
};

// Non-clear pixel count against the known clear color (BGRA capture order or
// not — the check only needs inequality on ANY channel by > 16).
std::size_t ink_pixels(const std::vector<std::uint8_t>& px, std::uint32_t w, std::uint32_t h) {
    const std::uint8_t cb = 26, cg = 28, cr = 33; // clear_rgba below, byte order-agnostic triple
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        const bool a0 = px[i] > cb + 16 || px[i] + 16 < cb;
        const bool a1 = px[i + 1] > cg + 16 || px[i + 1] + 16 < cg;
        const bool a2 = px[i + 2] > cr + 16 || px[i + 2] + 16 < cr;
        if (a0 || a1 || a2) ++n;
    }
    (void)w; (void)h;
    return n;
}

// The scripted edit timeline, driven through the input_adapter surface.
// `stage` counts scripted frames; on stage 1 we locate the forms "name"
// field from the previous frame's rects and start clicking (the recipe is
// test_def_forms_panel's: click down, click up, erase N chars, retype +
// enter). Everything else is idle frames.
enum class script_stage { idle, locate, down, up, erase, type, settle };

struct scripted_edit {
    script_stage stage = script_stage::idle;
    int name_len = 0;
    int erase_i = 0;
    float cx = 0.f, cy = 0.f;
    std::string new_name = "EmptyX";
};

} // namespace

int main(int argc, char** argv) {
    // --- CLI -----------------------------------------------------------------
    int planned = 0; // 0 = interactive until the window closes
    std::uint32_t width = 1400, height = 900;
    std::string work, out_dir, src_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) { fail(std::string("missing value for ") + flag); std::exit(2); }
            return argv[++i];
        };
        if (a == "--frames") planned = std::atoi(next("--frames"));
        else if (a == "--width") width = static_cast<std::uint32_t>(std::atoi(next("--width")));
        else if (a == "--height") height = static_cast<std::uint32_t>(std::atoi(next("--height")));
        else if (a == "--work") work = next("--work");
        else if (a == "--src") src_dir = next("--src");
        else if (a == "--out") out_dir = next("--out");
        else { fail("unknown argument: " + a); return 2; }
    }
    if (work.empty()) {
        work = (std::filesystem::temp_directory_path() /
                ("ve_edit_" + std::to_string(::getpid()))).string();
    }
    const std::filesystem::path work_p(work);
    const std::filesystem::path src = src_dir.empty() ? work_p / "src" : std::filesystem::path(src_dir);
    const std::filesystem::path cooked = work_p / "cooked";
    const std::filesystem::path tool = work_p / "tool";
    const std::filesystem::path toolcooked = work_p / "toolcooked";
    const bool scripted = planned > 0;
    if (scripted && out_dir.empty()) { fail("--frames requires --out"); return 2; }

    // --- 1. trees --------------------------------------------------------------
    // Tool tree: the two UI shaders (source text owned by gfx, ui_render.h)
    // + the one font. User tree: the empty world (#54: no art).
    write_text(tool / "ui.vert", gfx::kUi_vert_glsl);
    write_text(tool / "ui.frag", gfx::kUi_frag_glsl);
    std::error_code ec;
    std::filesystem::copy_file(std::filesystem::path(VE_SOURCE_DIR) / "assets/fonts/DejaVuSansMono.ttf",
                               tool / "DejaVuSansMono.ttf",
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (!src_dir.empty()) {
        if (!std::filesystem::exists(src / "world/system.toml")) {
            fail("--src tree has no world/system.toml");
            return 2;
        }
    } else {
        write_text(src / "world/system.toml", kEmptySys);
    }

    std::string err;
    auto tool_res = cook::cook_incremental(tool.string(), toolcooked.string());
    if (!tool_res.ok) {
        fail("tool-tree cook: " + tool_res.error +
             "   (GLSL compiler: glslangValidator on PATH, or point VE_GLSLANG at one)");
        return 2;
    }
    auto user_res = cook::cook_incremental(src.string(), cooked.string());
    if (!user_res.ok) { fail("world cook: " + user_res.error); return 2; }

    gfx::font0 font;
    if (!gfx::load_cooked_font_file((toolcooked / "units/DejaVuSansMono.ttf.ck").string(), font, &err)) {
        fail("font unit: " + err);
        return 2;
    }
    cook::Unit u_v, u_f;
    if (!cook::read_unit_file((toolcooked / "units/ui.vert.ck").string(), u_v, &err) ||
        !cook::read_unit_file((toolcooked / "units/ui.frag.ck").string(), u_f, &err)) {
        fail("ui shader units: " + err);
        return 2;
    }
    if (u_v.type != core::cooked::kUnitShdr || u_f.type != core::cooked::kUnitShdr) {
        fail("a ui shader unit is not SHDR");
        return 2;
    }

    // --- 2. authoring state ----------------------------------------------------
    // The forms doc parses the SAME text the gate cooks; the gate re-cooks the
    // user tree and reloads author_world on accept (the #16 loop, live).
    cook::author_world aw;
    std::map<std::string, std::string> sources{{"world/system.toml", read_text(src / "world/system.toml")}};
    if (!aw.load(sources, src.string(), cooked.string(), &err)) {
        fail("author_world load: " + err);
        return 2;
    }
    game::def_forms_doc doc;
    if (!doc.load(game::kWorld_system_def_schema, read_text(src / "world/system.toml"), &err)) {
        fail("forms parse: " + err);
        return 2;
    }
    game::def_forms_panel forms(std::move(doc));
    int gate_calls = 0, gate_accepts = 0;
    forms.gate = [&](const std::string& candidate) -> std::string {
        ++gate_calls;
        const std::string old_text = read_text(src / "world/system.toml");
        write_text(src / "world/system.toml", candidate);
        auto res = cook::cook_incremental(src.string(), cooked.string());
        if (!res.ok) {
            write_text(src / "world/system.toml", old_text); // revert bit-exact
            (void)cook::cook_incremental(src.string(), cooked.string());
            return res.error;
        }
        ++gate_accepts;
        std::map<std::string, std::string> fresh{
            {"world/system.toml", read_text(src / "world/system.toml")}};
        if (!aw.load(fresh, src.string(), cooked.string(), &err)) {
            return "re-cooked, but authoring reload failed: " + err;
        }
        return "";
    };
    game::selection_service sel;
    game::inspector_panel insp(sel);
    insp.begin_schematic(&aw); // mode-B map over the live authoring world

    // --- 3. platform seam --------------------------------------------------------
    auto w = platform::window::create(static_cast<int>(width), static_cast<int>(height),
                                      "void-engine EK — editor kit (empty world)");
    if (!w) { fail(w.error()); return 2; }
    if (const auto [fw0, fh0] = w->frame_size(); fw0 > 1u && fh0 > 1u) { width = fw0; height = fh0; }
    const auto exts = w->required_instance_extensions();
    if (exts.empty()) { fail("GLFW reported no instance extensions"); return 2; }
    auto inst = gfx::instance::create("ve_edit", exts);
    if (!inst) { fail(inst.error()); return 2; }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (auto made = w->create_surface(inst->handle()); made) {
        surface = *made;
    } else {
        fail(made.error());
        return 2;
    }
    const surface_owner surface_guard{&*inst, &surface};
    auto dev = gfx::device::create(*inst, surface);
    if (!dev) { fail(dev.error()); return 2; }
    auto pres = gfx::wsi_presenter::create(*dev, surface, width, height);
    if (!pres) { fail(pres.error()); return 2; }

    // --- 4. UI pass --------------------------------------------------------------
    gfx::CpuTexture white;
    white.width = white.height = 1;
    white.format = core::cooked::kTexFmtRgba8;
    white.pixels = {255, 255, 255, 255};
    auto tex_white = gfx::gpu_texture::create(*dev, white);
    if (!tex_white) { fail("white texture: " + tex_white.error()); return 2; }
    gfx::CpuTexture atlas;
    atlas.width = font.atlas_w;
    atlas.height = font.atlas_h;
    atlas.format = core::cooked::kTexFmtRgba8;
    atlas.pixels = font.atlas;
    auto tex_font = gfx::gpu_texture::create(*dev, atlas);
    if (!tex_font) { fail("font atlas: " + tex_font.error()); return 2; }
    auto rend = gfx::ui_renderer::create(*dev, *tex_white, *tex_font, u_v.payload, u_f.payload,
                                         pres->color_format());
    if (!rend) { fail("ui_renderer: " + rend.error()); return 2; }

    // --- 5. input -----------------------------------------------------------------
    platform::input_adapter ad;
    ad.set_frame_scale(width, height, width, height);
    if (!scripted) platform::install_glfw_input(w->glfw_handle(), ad);

    // --- 6. frame loop --------------------------------------------------------------
    gfx::ui_context ui;
    const float clear_rgba[4] = {33.f / 255.f, 28.f / 255.f, 26.f / 255.f, 1.f};
    std::vector<std::uint8_t> cap_first, cap_last;
    scripted_edit script;
    const bool bgra = pres->color_format() == VK_FORMAT_B8G8R8A8_UNORM;
    int presented = 0;
    bool ok = true;

    for (int frame = 0; planned == 0 ? !w->should_close() : frame < planned; ++frame) {
        w->poll_events();
        if (planned == 0 && w->should_close()) break;
        if (pres->needs_recreate()) {
            const auto [rw, rh] = w->frame_size();
            if (!pres->recreate(rw, rh)) { fail("presenter recreate failed"); ok = false; break; }
            width = rw; height = rh;
            ad.set_frame_scale(width, height, width, height);
        }

        // Scripted edit timeline (only after frame 0's rects exist).
        if (scripted) {
            switch (script.stage) {
            case script_stage::idle:
                if (frame == 1) script.stage = script_stage::locate;
                break;
            case script_stage::locate: {
                const auto r = forms.field_rect("name");
                if (!r) { fail("forms has no 'name' field rect after frame 0"); ok = false; break; }
                script.cx = static_cast<float>(r->x + r->w / 2);
                script.cy = static_cast<float>(r->y + r->h / 2);
                script.name_len = static_cast<int>(forms.doc().value("name").size());
                script.stage = script_stage::down;
                break;
            }
            case script_stage::down:
                ad.cursor(script.cx, script.cy);
                ad.mouse(platform::input_adapter::button::left, true);
                script.stage = script_stage::up;
                break;
            case script_stage::up:
                ad.cursor(script.cx, script.cy);
                ad.mouse(platform::input_adapter::button::left, false);
                script.stage = script_stage::erase;
                break;
            case script_stage::erase:
                if (script.erase_i++ < script.name_len) {
                    ad.press(platform::input_adapter::key::backspace);
                } else {
                    for (char c : script.new_name) ad.character(static_cast<std::uint32_t>(c));
                    ad.press(platform::input_adapter::key::enter);
                    script.stage = script_stage::type;
                }
                break;
            case script_stage::type:
            case script_stage::settle:
                break;
            }
        }

        const gfx::ui_io io = ad.next_frame();
        ui.begin_frame(io, font, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
        // Panels draw their own frames (8.2 contract) — no ui.begin_window here.
        forms.render(ui, "system.toml", 16, 16, 460, static_cast<std::int32_t>(height) - 32);
        // Mode-B map (EK.4): host-driven render_schematic + per-frame click
        // select (the panel's rising-edge contract). The ECS tree side comes
        // with the live world in EK.5 — an empty authoring world has none.
        insp.click_schematic(io);
        insp.render_schematic(ui, 492, 16, static_cast<std::int32_t>(width) - 508,
                              static_cast<std::int32_t>(height) - 32);
        ui.end_frame();

        auto idx = pres->acquire();
        if (!idx) {
            if (!pres->needs_recreate()) { fail("acquire: " + idx.error()); ok = false; break; }
            const auto [rw, rh] = w->frame_size();
            if (!pres->recreate(rw, rh)) { fail("recreate after stale acquire failed"); ok = false; break; }
            continue;
        }
        VkCommandBuffer cmd = pres->begin_command();
        if (cmd == VK_NULL_HANDLE) { fail("no command buffer"); ok = false; break; }
        if (!pres->begin_pass(cmd)) { fail("begin_pass refused the frame"); ok = false; break; }

        VkRenderingAttachmentInfo att{};
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = pres->color_view();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color.float32[0] = clear_rgba[0];
        att.clearValue.color.float32[1] = clear_rgba[1];
        att.clearValue.color.float32[2] = clear_rgba[2];
        att.clearValue.color.float32[3] = clear_rgba[3];
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {width, height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);
        if (!rend->draw(cmd, ui.frame(), static_cast<std::int32_t>(width),
                        static_cast<std::int32_t>(height))) {
            fail("ui_renderer refused the frame"); ok = false; break;
        }
        vkCmdEndRendering(cmd);
        if (!pres->end_pass(cmd)) { fail("end_pass refused the frame"); ok = false; break; }
        if (!pres->present(cmd)) { fail("present failed"); ok = false; break; }
        ++presented;

        if (presented == 1) cap_first = pres->capture_rgba8();
        if (scripted && frame + 1 < planned) cap_last = pres->capture_rgba8();
    }

    if (scripted) cap_last = pres->capture_rgba8(); // final presented frame

    if (!scripted) {
        VE_LOG_INFO("ve_edit: interactive session closed after %d frames", presented);
        return ok ? 0 : 2;
    }

    // --- 7. self-attestation ------------------------------------------------------
    if (!ok) return 2;
    const auto problems = inst->validation_problems();
    if (!problems.empty()) {
        for (const auto& p : problems) fail("validation: " + p);
        return 3; // 2.7 policy, app-side: the layer is required in the chain
    }
    if (gate_calls == 0) { fail("the scripted edit never consulted the gate"); return 3; }
    if (gate_accepts == 0) { fail("the gate refused the scripted edit: forms/cook disagree"); return 3; }
    if (!forms.last_gate_error().empty()) { fail("gate error left showing: " + forms.last_gate_error()); return 3; }
    const std::string on_disk = read_text(src / "world/system.toml");
    if (on_disk.find("EmptyX") == std::string::npos) { fail("edited name never reached the file"); return 3; }
    if (cap_first.empty() || cap_last.size() != cap_first.size()) { fail("capture plumbing broke"); return 2; }
    if (ink_pixels(cap_last, width, height) < 3000) { fail("UI never painted (ink too low)"); return 3; }
    if (cap_first == cap_last) { fail("final capture identical to frame 0 — edit not on screen"); return 4; }

    if (!out_dir.empty()) {
        std::filesystem::create_directories(out_dir);
        const std::string a = (std::filesystem::path(out_dir) / "ve_edit_f0.png").string();
        const std::string b = (std::filesystem::path(out_dir) / "ve_edit_last.png").string();
        if (!demo::write_png_rgba8(a.c_str(), width, height, cap_first.data(), bgra) ||
            !demo::write_png_rgba8(b.c_str(), width, height, cap_last.data(), bgra)) {
            fail("PNG evidence write failed");
            return 2;
        }
    }
    VE_LOG_INFO("ve_edit: %d frames, gate %d/%d accepted, ink %zu px — GREEN",
                presented, gate_accepts, gate_calls, ink_pixels(cap_last, width, height));
    return 0;
}
