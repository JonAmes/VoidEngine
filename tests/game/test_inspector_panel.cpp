// Phase 8 task 8.3 — inspector_panel: the entity tree (ECS entities +
// their components, names opaque per 6.1) and the stats/modifier-chain
// view over the running sim, driven through the SHARED selection_service
// (#48 kit spine: 8.4/8.5 panels will select the same way). Pins: the
// render list is deterministic, select/expand/scroll actions work,
// scroll clamps, scrolled-away rows lose their hit rects, a dead
// selection degrades to "(no live selection)", and stats + chains draw
// strictly more than the bare label does.
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/cook/cook.h>
#include <ve/core/ecs.h>
#include <ve/core/sim/modifiers.h>
#include <ve/core/sim/stats.h>
#include <ve/game/inspector_panel.h>
#include <ve/gfx/font.h>
#include <ve/gfx/ui.h>

#include "../ve_test_portability.h"

#ifndef VE_SOURCE_DIR
#define VE_SOURCE_DIR "."
#endif

namespace {

using namespace ve;

std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_insp_") + name + "_" + std::to_string(ve_test::pid()));
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

// Text presence pin: glyph vertices (atlas slot 1). Per-CMD counting is
// fragile — consecutive label runs merge into one cmd; verts never merge.
std::size_t glyph_verts(const gfx::ui_frame& f) {
    return f.verts.size();
}

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

struct test_hull { int compartments = 0; };
struct test_thruster { float kn = 0.f; };

}  // namespace

TEST_CASE("inspector_panel") {
    // --- scene: ecs entities + one entity's stats + modifier chain -------
    core::ecs_world world;
    const auto hull = world.register_component_type("ve.test/hull");
    const auto thruster = world.register_component_type("ve.test/thruster");
    const auto e0 = world.create();
    const auto e1 = world.create();
    const auto e2 = world.create();
    world.emplace(e0, hull, test_hull{3});
    world.emplace(e1, hull, test_hull{});
    world.emplace(e1, thruster, test_thruster{90.f});

    core::StatsSet ship_stats;
    ship_stats.set_base("hull.hp", 100.0f);
    ship_stats.set_base("shield.hp", 50.0f);
    core::ModifierStack ship_mods;
    REQUIRE(ship_mods.add_source(42, {"ve.test/seat"}));
    core::modifier_desc md;
    md.target_attr = "hull.hp";
    md.stack_id = "ve.test/dmg";
    md.op = core::mod_op::percent;
    md.value = -25.0f;
    md.policy = core::stack_policy::replace;
    REQUIRE(ship_mods.add_modifier(42, md) == core::mod_add_result::added);

    game::inspector_data data;
    data.world = &world;
    data.stats_for = [&](core::entity_id e) -> const core::StatsSet* {
        return e == e0 ? &ship_stats : nullptr;
    };
    data.mods_for = [&](core::entity_id e) -> const core::ModifierStack* {
        return e == e0 ? &ship_mods : nullptr;
    };

    game::selection_service sel;
    game::inspector_panel panel(sel);

    gfx::font0 font;
    prep_font(font);

    const std::int32_t h_all = 600;
    auto draw = [&](gfx::ui_context& ui, const gfx::ui_io& io, std::int32_t h) {
        // ui_io must OUTLIVE begin_frame..end_frame (ui.cpp keeps a pointer).
        ui.begin_frame(io, font, 1280, 720);
        panel.render(ui, data, 8, 8, 900, h);
        ui.end_frame();
    };
    auto render = [&](gfx::ui_context& ui, std::int32_t h) {
        const gfx::ui_io none{};
        draw(ui, none, h);
    };
    auto click = [&](gfx::ui_context& ui, const std::string& key, std::int32_t h) {
        const auto r = panel.rect_for(key);
        REQUIRE_MESSAGE(r.has_value(), "no rect for " << key);
        const float cx = static_cast<float>(r->x + r->w / 2);
        const float cy = static_cast<float>(r->y + r->h / 2);
        const gfx::ui_io down = click_down(cx, cy);
        const gfx::ui_io up = click_up(cx, cy);
        draw(ui, down, h);
        draw(ui, up, h);
    };

    gfx::ui_context ui;

    // --- tree rows exist for every entity --------------------------------
    render(ui, h_all);
    CHECK(panel.rect_for("tree:0").has_value());
    CHECK(panel.rect_for("tree:1").has_value());
    CHECK(panel.rect_for("tree:2").has_value());
    CHECK(panel.rect_for("tw:1").has_value());
    CHECK_FALSE(sel.selected().has_value());

    // --- deterministic render list: same io, same bytes twice ------------
    gfx::ui_context ui2;
    render(ui, h_all);
    render(ui2, h_all);
    CHECK(frames_equal(ui.frame(), ui2.frame()));

    // --- selecting drives the SHARED service ------------------------------
    click(ui, "tree:1", h_all);
    REQUIRE(sel.selected().has_value());
    CHECK(*sel.selected() == e1);

    // --- twisty expands; components render (more glyph rows) -------------
    const std::size_t before = glyph_verts(ui.frame());
    click(ui, "tw:1", h_all);
    CHECK(sel.is_expanded(e1));
    render(ui, h_all);
    CHECK_GT(glyph_verts(ui.frame()), before);  // both opaque type names drawn
    click(ui, "tw:1", h_all);
    CHECK_FALSE(sel.is_expanded(e1));
    render(ui, h_all);
    CHECK(glyph_verts(ui.frame()) == before);

    // --- stats + modifier chain draw on e0 --------------------------------
    click(ui, "tree:0", h_all);
    render(ui, h_all);
    const std::size_t with_stats = glyph_verts(ui.frame());
    game::inspector_data bare = data;
    bare.stats_for = nullptr;
    bare.mods_for = nullptr;
    auto draw_bare = [&](gfx::ui_context& u) {
        const gfx::ui_io none{};
        u.begin_frame(none, font, 1280, 720);
        panel.render(u, bare, 8, 8, 900, h_all);
        u.end_frame();
    };
    gfx::ui_context ui3;
    draw_bare(ui3);
    // e0 also has stats label lines + one source + one mod line: strict.
    CHECK_GT(with_stats, glyph_verts(ui3.frame()));

    // --- scroll: [v] moves, clamps, scrolled-away rows lose hit rects ----
    game::inspector_panel p2(sel);  // fresh view state, same shared selection
    auto render2 = [&](std::int32_t h) {
        gfx::ui_context u;
        const gfx::ui_io none{};
        u.begin_frame(none, font, 1280, 720);
        p2.render(u, data, 8, 8, 900, h);
        u.end_frame();
        return u;
    };
    gfx::ui_context u2 = render2(h_all);
    CHECK(p2.tree_scroll() == 0);
    auto click2 = [&](gfx::ui_context& u, const std::string& key, std::int32_t h) {
        const auto r = p2.rect_for(key);
        REQUIRE_MESSAGE(r.has_value(), "no rect for " << key);
        const float cx = static_cast<float>(r->x + r->w / 2);
        const float cy = static_cast<float>(r->y + r->h / 2);
        const gfx::ui_io down = click_down(cx, cy);
        const gfx::ui_io up = click_up(cx, cy);
        u.begin_frame(down, font, 1280, 720);
        p2.render(u, data, 8, 8, 900, h);
        u.end_frame();
        u.begin_frame(up, font, 1280, 720);
        p2.render(u, data, 8, 8, 900, h);
        u.end_frame();
    };
    click2(u2, "scroll_dn", h_all);
    CHECK(p2.tree_scroll() == 1);
    click2(u2, "scroll_dn", h_all);
    CHECK(p2.tree_scroll() == 2);
    click2(u2, "scroll_dn", h_all);  // 3 entities: max scroll = 2 (clamp)
    CHECK(p2.tree_scroll() == 2);
    CHECK_FALSE(p2.rect_for("tree:0").has_value());  // scrolled away
    CHECK_FALSE(p2.rect_for("tree:1").has_value());
    CHECK(p2.rect_for("tree:2").has_value());
    for (int i = 0; i < 5; ++i) click2(u2, "scroll_up", h_all);
    CHECK(p2.tree_scroll() == 0);  // clamped at top
    CHECK(p2.rect_for("tree:0").has_value());

    // --- dead selection degrades instead of crashing ----------------------
    sel.clear_selection();
    click(u2, "tree:2", h_all);
    REQUIRE(*sel.selected() == e2);
    REQUIRE(world.destroy(e2));
    render2(h_all);  // must not crash; takes the "(no live selection)" path

    // --- shared service: another panel sees the same state ----------------
    game::inspector_panel p3(sel);  // 8.4/8.5-style reuse of the same state
    gfx::ui_context u3;
    const gfx::ui_io none_final{};
    u3.begin_frame(none_final, font, 1280, 720);
    p3.render(u3, data, 950, 8, 300, h_all);
    u3.end_frame();
    CHECK(p3.rect_for("tree:0").has_value());
}
