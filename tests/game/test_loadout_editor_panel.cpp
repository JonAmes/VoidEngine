// Phase 8 task 8.5 — loadout_editor_panel: hull select + whole-ship swap,
// categorized socket tree, mount/unmount gated by the SIM's own
// Loadout::assemble (refused ON ENTRY with a reason — #48 "garbage hard to
// enter, easy to fix"), mass/power readout from the stats spine, and
// TOML-only write-back (#16) proven end-to-end: edit -> emit_source ->
// write -> REAL watch-mode cook (cook_incremental) -> reload -> mount
// equality. Selection split per 8.4: hull def selection is the panel's own
// working state (entity selection stays in the 8.3 selection_service).
#include <unistd.h>  // getpid: unique staging roots

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/cook/cook.h>
#include <ve/core/sim/loadout_load.h>
#include <ve/game/def_schema.h>
#include <ve/game/loadout_editor_panel.h>
#include <ve/gfx/font.h>
#include <ve/gfx/ui.h>

#include "../ve_test_portability.h"

#ifndef VE_SOURCE_DIR
#define VE_SOURCE_DIR "."
#endif

namespace {

using namespace ve;
namespace sim = core::sim;

std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_ldt_") + name + "_" + std::to_string(ve_test::pid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}

void prep_font(gfx::font0& font) {
    const auto font_src = std::filesystem::path(VE_SOURCE_DIR) / "assets/fonts/DejaVuSansMono.ttf";
    REQUIRE(std::filesystem::exists(font_src));
    const std::string src = tmp_root("src");
    const std::string out = tmp_root("out");
    std::filesystem::copy_file(font_src, std::filesystem::path(src) / "DejaVuSansMono.ttf");
    auto font_cook = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(font_cook.ok, font_cook.error);
    std::string err;
    REQUIRE_MESSAGE(gfx::load_cooked_font_file(out + "/units/DejaVuSansMono.ttf.ck", font, &err), err);
}

bool frames_equal(const gfx::ui_frame& a, const gfx::ui_frame& b) {
    if (a.verts.size() != b.verts.size() || a.indices.size() != b.indices.size() ||
        a.cmds.size() != b.cmds.size())
        return false;
    if (std::memcmp(a.verts.data(), b.verts.data(), a.verts.size() * sizeof(gfx::ui_vertex)) != 0)
        return false;
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

// Text presence pin: glyph VERTICES (8.3/8.4 lesson — consecutive label runs
// merge into one cmd; verts never merge).
std::size_t glyph_verts(const gfx::ui_frame& f) { return f.verts.size(); }

gfx::ui_io click_down(float x, float y) {
    gfx::ui_io io{};
    io.mouse_x = x;
    io.mouse_y = y;
    io.mouse_down = true;
    return io;
}
gfx::ui_io click_up(float x, float y) {
    gfx::ui_io io{};
    io.mouse_x = x;
    io.mouse_y = y;
    return io;
}

std::string read_text(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}

std::size_t count_of(const std::string& hay, const std::string& needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
    return n;
}

// ---- host-side snapshot assembly (the watcher host's job, file-free panel) --
game::loadout_snapshot snapshot_from(const std::string& def_id, const std::string& label,
                                     const sim::loaded_hull& lh, std::string source,
                                     std::vector<game::loadout_catalog_part> catalog) {
    game::loadout_snapshot s;
    s.def_id = def_id;
    s.label = label;
    s.source = std::move(source);
    s.hull = lh.loadout.hull();
    s.sockets = lh.loadout.sockets();
    for (const auto& m : lh.loadout.mounts()) {
        const bool known = std::any_of(s.types.begin(), s.types.end(),
                                       [&](const auto& t) { return t.type_id == m.type_id; });
        if (!known) s.types.push_back({m.type_id, m.category});
        sim::mount_request mr;
        mr.instance_id = m.instance_id;
        mr.type_id = m.type_id;
        mr.socket_id = m.socket_id;
        mr.attrs = m.attrs;
        mr.tags = m.tags;
        s.mounts.push_back(std::move(mr));
    }
    s.catalog = std::move(catalog);
    return s;
}

}  // namespace

TEST_CASE("loadout_editor_panel") {
    // ---- stage + COOK the real fixture tree (8.2/8.4 doctrine) ------------
    const auto root = std::filesystem::temp_directory_path() /
                      ("ve_ldt_tree_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "defs");
    std::filesystem::create_directories(root / "samples");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb",
                          root / "samples" / "test_ship.glb");
    const std::string def_a = read_text(std::string(VE_SOURCE_DIR) + "/assets/defs/test_ship.def.toml");
    REQUIRE(def_a.find("hull_mass = 98000") != std::string::npos);
    std::ofstream(root / "defs" / "test_ship.def.toml", std::ios::binary) << def_a;
    // Hull option B (#24d whole-ship swap): same hull + kit, half the hull mass.
    std::string def_b = def_a;
    def_b.replace(def_b.find("hull_mass = 98000"), 17, "hull_mass = 49000");
    std::ofstream(root / "defs" / "test_ship_b.def.toml", std::ios::binary) << def_b;
    const auto out = root / "out";
    {
        const auto r = ve::cook::cook_incremental(root.string(), out.string());
        REQUIRE_MESSAGE(r.ok, r.error);
    }

    const std::string id_a = "defs/test_ship.def.toml";
    const std::string id_b = "defs/test_ship_b.def.toml";
    std::string err;
    core::cooked::Unit def0a, def0b, socku;
    REQUIRE_MESSAGE(core::cooked::read_unit_file((out / "units" / (id_a + ".ck")).string(), def0a, &err), err);
    REQUIRE_MESSAGE(core::cooked::read_unit_file((out / "units" / (id_b + ".ck")).string(), def0b, &err), err);
    REQUIRE_MESSAGE(core::cooked::read_unit_file((out / "units" / "samples/test_ship.glb.sock.ck").string(), socku, &err), err);
    auto lha = sim::load_hull_loadout(def0a, socku, 0xAULL);
    const std::string lha_err = lha ? std::string{} : lha.error();
    REQUIRE_MESSAGE(lha.has_value(), lha_err);
    auto lhb = sim::load_hull_loadout(def0b, socku, 0xAULL);
    const std::string lhb_err = lhb ? std::string{} : lhb.error();
    REQUIRE_MESSAGE(lhb.has_value(), lhb_err);
    CHECK(lha->loadout.mounts().size() == 11);

    // Host-built catalog: sensor.array DIFFERS from the authored sensor_scan
    // (2000 W vs 1500 W, "150 km" suffix) so edit effects stay distinguishable.
    std::vector<game::loadout_catalog_part> catalog = {
        {"thruster.main", sim::socket_category::main_,
         {{"mass", 900, "900"}, {"power.draw", 25000, "25000"},
          {"thrust.force", 320000, "320000"}, {"thrust.isp", 300, "300"}},
         {"propulsion"}},
        {"thruster.rcs", sim::socket_category::rcs_,
         {{"mass", 40, "40"}, {"power.draw", 800, "800"},
          {"thrust.force", 4000, "4000"}, {"thrust.isp", 220, "220"}},
         {}},
        {"sensor.array", sim::socket_category::utility_,
         {{"mass", 60, "60"}, {"power.draw", 2000, "2000"},
          {"sensor.range", 150000, "\"150 km\""}, {"sensor.res_arcmin", 2.5, "2.5"}},
         {}},
    };

    game::loadout_editor_panel panel;
    panel.set_hulls({snapshot_from(id_a, "light freighter", *lha, def_a, catalog),
                     snapshot_from(id_b, "halved freighter", *lhb, def_b, catalog)});
    REQUIRE(panel.current_def_id() != nullptr);
    CHECK(*panel.current_def_id() == id_a);  // sorted first hull option

    // ---- stats-spine readout, pinned numerically ---------------------------
    // 98000 hull + 900+900+40+60+250+400+120 = 100670 kg
    CHECK(panel.total_mass() == 100670.0);
    CHECK(panel.power_supply() == 170000.0);   // reactor + battery
    CHECK(panel.power_draw() == 57300.0);
    CHECK(panel.mount_count() == 11);

    gfx::font0 font;
    prep_font(font);
    const std::int32_t h_all = 700;
    auto render = [&](gfx::ui_context& u, std::int32_t h) {
        const gfx::ui_io none{};
        u.begin_frame(none, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);
        u.end_frame();
    };
    auto click = [&](gfx::ui_context& u, const std::string& key, std::int32_t h) {
        const auto r = panel.rect_for(key);
        REQUIRE(r.w > 0);
        const float cx = static_cast<float>(r.x + r.w / 2);
        const float cy = static_cast<float>(r.y + r.h / 2);
        const gfx::ui_io down = click_down(cx, cy), up = click_up(cx, cy);
        u.begin_frame(down, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);
        u.end_frame();
        u.begin_frame(up, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);
        u.end_frame();
        const gfx::ui_io none{};
        u.begin_frame(none, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);  // settle: post-click rows refreshed
        u.end_frame();
    };

    // ---- determinism: two fresh contexts, same state -> identical frames ---
    {
        gfx::ui_context ua, ub;
        render(ua, h_all);
        render(ub, h_all);
        CHECK(frames_equal(ua.frame(), ub.frame()));
    }

    gfx::ui_context ui;
    render(ui, h_all);

    // ---- unmount sensor_scan ([x] on its socket row) ----------------------
    click(ui, "un:hp_wingtip_L", h_all);
    CHECK(panel.mount_count() == 10);
    CHECK(panel.total_mass() == 100610.0);  // 60 kg gone
    CHECK(panel.power_draw() == 55800.0);   // 1500 W gone

    // ---- picker open on the freed utility socket; gate a WRONG category ---
    click(ui, "pick:hp_wingtip_L", h_all);
    CHECK(panel.rect_for("cat:thruster.rcs").w > 0);  // catalog NOT pre-filtered:
                                                      // garbage must be enterable
                                                      // enough to be refused.
    // Same-state baseline, then only the refusal changes the draw (8.4 rule:
    // a presence pin needs a same-selection baseline or the delta lies).
    render(ui, h_all);
    const std::size_t base = glyph_verts(ui.frame());
    click(ui, "cat:thruster.rcs", h_all);
    REQUIRE(panel.last_reject() != nullptr);
    CHECK(*panel.last_reject() == "category mismatch (thruster.rcs @ hp_wingtip_L)");
    CHECK(panel.mount_count() == 10);  // candidate never entered state
    render(ui, h_all);
    CHECK_GT(glyph_verts(ui.frame()), base);  // REFUSED label drawn

    // ---- mount the right part (also demonstrates "easy to fix") -----------
    click(ui, "cat:sensor.array", h_all);
    CHECK(panel.last_reject() == nullptr);
    CHECK(panel.mount_count() == 11);
    CHECK(panel.total_mass() == 100670.0);
    CHECK(panel.power_draw() == 57800.0);  // new sensor draws 2000 W, not 1500

    // ---- write-back (#16): TOML only, untouched bytes preserved -----------
    const std::string emitted = panel.emit_source();
    CHECK(emitted.find("sensor_array_hp_wingtip_l") != std::string::npos);
    CHECK(emitted.find("id = \"sensor_scan\"") == std::string::npos);  // block removed
    CHECK(count_of(emitted, "[[component]]") == 11);
    CHECK(emitted.find("w_engL") != std::string::npos);                // wires intact
    CHECK(emitted.find("# Phase 5 sample hull def") != std::string::npos);  // comments intact
    CHECK(emitted.find("15 deg") != std::string::npos);                // #42 suffixes intact

    // ---- close the loop with the REAL watch-mode cook + reload ------------
    std::ofstream(root / id_a, std::ios::binary) << emitted;
    {
        const auto r = ve::cook::cook_incremental(root.string(), out.string());
        REQUIRE_MESSAGE(r.ok, r.error);
    }
    REQUIRE_MESSAGE(core::cooked::read_unit_file((out / "units" / (id_a + ".ck")).string(), def0a, &err), err);
    auto relaunched = sim::load_hull_loadout(def0a, socku, 0xAULL);
    REQUIRE_MESSAGE(relaunched, "");
    const auto& mm = relaunched->loadout.mounts();
    REQUIRE(mm.size() == 11);
    // Panel working order == reloaded order == def component order.
    const std::vector<std::string> want = {
        "engine_L", "engine_R", "rcs_wt", "turret_dorsal", "seat_pilot",
        "door_1", "panel_main", "berth_1", "reactor_1", "battery_1",
        "sensor_array_hp_wingtip_l"};
    for (std::size_t i = 0; i < 11; ++i) {
        CHECK(mm[i].instance_id == want[i]);
    }
    CHECK(mm[10].type_id == "sensor.array");
    CHECK(mm[10].socket_id == "hp_wingtip_L");
    // #42 suffix converted on the way through the cooklet: "150 km" -> 150000.
    double range = -1.0, draw = -1.0, mass = -1.0;
    for (const auto& a : mm[10].attrs) {
        if (a.key == "sensor.range") range = a.value;
        if (a.key == "power.draw") draw = a.value;
        if (a.key == "mass") mass = a.value;
    }
    CHECK(range == 150000.0);
    CHECK(draw == 2000.0);
    CHECK(mass == 60.0);

    // ---- hull swap (#24d): whole loadout replaced by the option's own -----
    click(ui, "use:" + id_b, h_all);
    CHECK(*panel.current_def_id() == id_b);
    CHECK(panel.mount_count() == 11);
    CHECK(panel.total_mass() == 51670.0);  // 49000 hull + same 2670 kg kit
    CHECK(panel.power_draw() == 57300.0);  // B still has the ORIGINAL sensor
    // A's edits are untouched under the swap (independent working copies):
    click(ui, "use:" + id_a, h_all);
    CHECK(panel.total_mass() == 100670.0);
    CHECK(panel.power_draw() == 57800.0);
}

TEST_CASE("loadout_editor_gates") {
    // Pure sim-side gates: no files, no cook — the panel refuses garbage ON
    // ENTRY via Loadout::assemble and keeps state clean ("garbage hard to
    // enter, easy to fix", #48).
    sim::hull_info h;
    h.class_tag = "class.test";
    h.grid = {2.0, 4, 2.2, 0.1};
    h.hull_mass = 1000.0;

    std::vector<sim::socket_desc> sockets = {
        {"hp_M", sim::socket_category::main_, {}, {}},
        {"hp_R", sim::socket_category::rcs_, {}, {}},
        {"hp_G", sim::socket_category::gun_, {}, {}},
        {"hp_U", sim::socket_category::utility_, {}, {}},
    };
    // ghost.part is deliberately NOT in the type table -> unknown-type gate.
    std::vector<sim::component_type> types = {
        {"p_main", sim::socket_category::main_},
        {"p_rcs", sim::socket_category::rcs_},
        {"p_gun", sim::socket_category::gun_},
        {"p_util", sim::socket_category::utility_},
    };
    std::vector<game::loadout_catalog_part> catalog = {
        {"p_main", sim::socket_category::main_, {{"mass", 1, "1"}}, {}},
        {"p_rcs", sim::socket_category::rcs_, {{"mass", 1, "1"}}, {}},
        {"p_gun", sim::socket_category::gun_, {{"mass", 1, "1"}}, {}},
        {"p_util", sim::socket_category::utility_, {{"mass", 1, "1"}}, {}},
        {"ghost.part", sim::socket_category::utility_, {{"mass", 1, "1"}}, {}},
    };

    game::loadout_snapshot s;
    s.def_id = "defs/mini.def.toml";
    s.label = "mini";
    s.source = "class_tag = \"class.test\"\nhull_mass = 1000\n";  // no components yet
    s.hull = h;
    s.sockets = sockets;
    s.types = types;
    s.catalog = catalog;

    game::loadout_editor_panel panel;
    panel.set_hulls({std::move(s)});
    CHECK(panel.mount_count() == 0);
    CHECK(panel.total_mass() == 1000.0);  // bare hull only

    gfx::font0 font;
    prep_font(font);
    const std::int32_t h_all = 700;
    auto render = [&](gfx::ui_context& u, std::int32_t h) {
        const gfx::ui_io none{};
        u.begin_frame(none, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);
        u.end_frame();
    };
    auto click = [&](gfx::ui_context& u, const std::string& key, std::int32_t h) {
        const auto r = panel.rect_for(key);
        REQUIRE(r.w > 0);
        const float cx = static_cast<float>(r.x + r.w / 2);
        const float cy = static_cast<float>(r.y + r.h / 2);
        const gfx::ui_io down = click_down(cx, cy), up = click_up(cx, cy);
        u.begin_frame(down, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);
        u.end_frame();
        u.begin_frame(up, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);
        u.end_frame();
        const gfx::ui_io none{};
        u.begin_frame(none, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);  // settle: post-click rows refreshed
        u.end_frame();
    };
    gfx::ui_context ui;
    render(ui, h_all);

    // type gate: rcs and utility parts onto the MAIN socket — refused, twice,
    // with the attempted part named in the reason, state never touched.
    click(ui, "pick:hp_M", h_all);
    click(ui, "cat:p_rcs", h_all);
    REQUIRE(panel.last_reject() != nullptr);
    CHECK(*panel.last_reject() == "category mismatch (p_rcs @ hp_M)");
    CHECK(panel.mount_count() == 0);
    click(ui, "cat:p_util", h_all);
    REQUIRE(panel.last_reject() != nullptr);
    CHECK(*panel.last_reject() == "category mismatch (p_util @ hp_M)");
    CHECK(panel.mount_count() == 0);
    // unknown type gate (catalog part with no component_type entry).
    click(ui, "cat:ghost.part", h_all);
    REQUIRE(panel.last_reject() != nullptr);
    CHECK(*panel.last_reject() == "unknown component type (ghost.part @ hp_M)");
    CHECK(panel.mount_count() == 0);

    // fill every socket; occupied sockets lose their [pick] rect and gain [x].
    click(ui, "cat:p_main", h_all);
    CHECK(panel.mount_count() == 1);
    CHECK(panel.last_reject() == nullptr);
    click(ui, "pick:hp_R", h_all); click(ui, "cat:p_rcs", h_all);
    click(ui, "pick:hp_G", h_all); click(ui, "cat:p_gun", h_all);
    click(ui, "pick:hp_U", h_all); click(ui, "cat:p_util", h_all);
    CHECK(panel.mount_count() == 4);
    CHECK(panel.total_mass() == 1004.0);
    CHECK(panel.rect_for("pick:hp_M").w == 0);  // occupied: no picker entry
    CHECK(panel.rect_for("un:hp_M").w > 0);

    // unmount + remount loop ("easy to fix"): fresh instance id, no dup.
    click(ui, "un:hp_U", h_all);
    CHECK(panel.mount_count() == 3);
    click(ui, "pick:hp_U", h_all);
    click(ui, "cat:p_util", h_all);
    CHECK(panel.mount_count() == 4);

    // write-back from an empty-component source: blocks append at EOF.
    const std::string emitted = panel.emit_source();
    CHECK(count_of(emitted, "[[component]]") == 4);
    CHECK(emitted.find("id = \"p_main_hp_m\"") != std::string::npos);
    CHECK(emitted.find("id = \"p_util_hp_u\"") != std::string::npos);
    CHECK(emitted.find("class_tag") == 0);  // root keys untouched, still first
}
