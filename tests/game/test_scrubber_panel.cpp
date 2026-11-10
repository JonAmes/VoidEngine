// test: scrubber_panel — 6.16 (#16). The replay scrubber's debug panel on
// the Phase 3 pure-CPU immediate UI core (3.13): clicks arrive as neutral
// ui_io, output is a ui_frame draw list — headless (Tier 1), no display,
// no GPU. Pins (all rects derived from the ui_context layout rules, window
// (0,0,220,360), DejaVu Mono 16px advance 8.27515/cp):
//   - transport buttons click through: "+1"/"+100" step_frame, "-1"/"-100"
//     jump_to; the scrubber lands on the live run's BIT-EXACT hash;
//   - tick field: focus, typed digits + Enter jump; stale buffer cleared by
//     backspace then committed via the "Go" button;
//   - "Diff t-100" checkbox grows/shrinks the draw list and fails closed to
//     a "diff n/a" row when the span underflows below tick -1;
//   - determinism: same io sequence into fresh context + fresh scrubber ->
//     byte-identical frames.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/cook/cook.h>
#include <ve/core/sim/replay.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/scrubber.h>
#include <ve/game/scrubber_panel.h>
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
             (std::string("ve_scrub_") + name + "_" + std::to_string(ve_test::pid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
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

// --- live-run replay record (same scenario as 6.15) ------------------------
constexpr std::int64_t kTicks = 600;
constexpr std::int64_t kCpEvery = 100;
constexpr double kForce = 1200.0;
constexpr double kMass = 50000.0;

struct live_run {
    core::sim::ReplayLog log;
    std::vector<std::uint64_t> hashes;
    std::vector<core::sim::sim_checkpoint> cps;
};

core::sim::rigid_params make_params() {
    core::sim::rigid_params p{};
    p.mass = kMass;
    p.inertia_tensor = core::math::dmat3::diag({1.0e5, 8.0e5, 7.0e5});
    return p;
}

live_run run_live() {
    live_run r;
    r.cps.push_back({-1, core::sim::rigid_state{}});
    const core::sim::rigid_params p = make_params();
    core::sim::rail_world w;
    core::sim::rigid_state s{};
    for (std::int64_t t = 0; t < kTicks; ++t) {
        core::sim::tick_command cmd{};
        cmd.thrust_body = {0.0, 0.0, -kForce};
        if (t >= 400 && t < 500)
            cmd.torque_body = {0.0, 100.0, 0.0};
        r.log.append(cmd);
        core::sim::replay_step(w, r.log, static_cast<std::size_t>(t),
                               static_cast<core::sim::tick_t>(t), p, s);
        r.hashes.push_back(core::sim::state_hash(static_cast<core::sim::tick_t>(t), s));
        if ((t + 1) % kCpEvery == 0)
            r.cps.push_back({static_cast<core::sim::tick_t>(t), s});
    }
    return r;
}

// Click at a point: down frame then up frame (ui click = press+release over).
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

} // namespace

TEST_CASE("scrubber_panel") {
    // Font: cook DejaVu Mono 16px (CPU only, no external tool). No shaders:
    // the panel is verified purely through the draw list — nothing presents.
    const auto font_src =
        std::filesystem::path(VE_SOURCE_DIR) / "assets/fonts/DejaVuSansMono.ttf";
    REQUIRE(std::filesystem::exists(font_src));
    const std::string src = tmp_root("src");
    const std::string out = tmp_root("out");
    std::filesystem::copy_file(font_src, std::filesystem::path(src) / "DejaVuSansMono.ttf");
    auto font_cook = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(font_cook.ok, font_cook.error);
    gfx::font0 font;
    std::string err;
    REQUIRE_MESSAGE(gfx::load_cooked_font_file(out + "/units/DejaVuSansMono.ttf.ck", font, &err),
                    err);

    const live_run live = run_live();
    const core::sim::rail_world w;

    gfx::ui_context ui;
    game::scrubber_panel panel;
    core::sim::ReplayScrubber sc(w, make_params());
    REQUIRE(sc.load(live.log, live.cps));
    REQUIRE(sc.jump_to(-1));

    auto frame = [&](const gfx::ui_io& io) {
        game::scrubber_panel_actions acts;
        ui.begin_frame(io, font, 800, 600);
        panel.draw(ui, sc, 0, 0, acts);
        ui.end_frame();
        return acts;
    };

    // --- idle frame: window + all widgets emitted --------------------------
    frame({});
    CHECK_FALSE(ui.frame().cmds.empty());

    // --- "+1": step_frame(1) from the pre-step cp ---------------------------
    // rect (8,72,32,18) -> center (24,81).
    frame(click_down(24.f, 81.f));
    auto a = frame(click_up(24.f, 81.f));
    CHECK(a.stepped == 1);
    CHECK(sc.cursor() == 0);
    CHECK(sc.hash().value() == live.hashes[0]);

    // --- "+100": rect (8,96,49,18) -> center (32,105) -----------------------
    frame(click_down(32.f, 105.f));
    a = frame(click_up(32.f, 105.f));
    CHECK(a.stepped == 100);
    CHECK(sc.cursor() == 100);
    CHECK(sc.hash().value() == live.hashes[100]);

    // --- "-100": jump_to(0); rect (8,24,49,18) -> center (32,33) ------------
    frame(click_down(32.f, 33.f));
    a = frame(click_up(32.f, 33.f));
    CHECK(a.jumped);
    CHECK(a.jumped_to.value() == 0);
    CHECK(sc.cursor() == 0);
    CHECK(sc.hash().value() == live.hashes[0]);

    // --- "-1" to the tick -1 checkpoint (cursor lands pre-step) -------------
    // rect (8,48,32,18) -> center (24,57).
    frame(click_down(24.f, 57.f));
    a = frame(click_up(24.f, 57.f));
    CHECK(a.jumped);
    CHECK(sc.cursor() == -1);

    // --- tick field: focus, type "150", Enter commits -> jump 150 -----------
    // field box rect (8,138,204,18); label consumed y 120..138.
    frame(click_down(100.f, 147.f));
    frame(click_up(100.f, 147.f));
    {
        gfx::ui_io io{};
        io.mouse_x = 100.f;
        io.mouse_y = 147.f;
        io.chars = {'1', '5', '0'};
        io.enter = true;
        a = frame(io);
    }
    CHECK(a.jumped);
    CHECK(a.jumped_to.value() == 150);
    CHECK(sc.cursor() == 150);
    CHECK(sc.hash().value() == live.hashes[150]);

    // --- stale buffer: refocus, backspace x3, type "25", "Go" commits -------
    frame(click_down(100.f, 147.f));
    frame(click_up(100.f, 147.f));
    for (int i = 0; i < 3; ++i) {
        gfx::ui_io io{};
        io.backspace = true;
        frame(io);
    }
    {
        gfx::ui_io io{};
        io.chars = {'2', '5'};
        frame(io);
    }
    // "Go" rect (8,162,32,18) -> center (24,171).
    frame(click_down(24.f, 171.f));
    a = frame(click_up(24.f, 171.f));
    CHECK(a.jumped);
    CHECK(a.jumped_to.value() == 25);  // "150" fully erased; "25" committed
    CHECK(sc.cursor() == 25);
    CHECK(sc.hash().value() == live.hashes[25]);

    // --- diff checkbox grows/shrinks the view at the SAME cursor -----------
    // checkbox row (8,186,200,18); click inside the box at (14,195).
    const std::size_t off25 = ui.frame().verts.size();  // cursor 25, diff off
    frame(click_down(14.f, 195.f));
    a = frame(click_up(14.f, 195.f));
    CHECK(a.diff_toggled);
    frame({});
    // cursor 25: diff_between(-75, 25) is out of range -> "diff n/a" row:
    // the view grows but fails closed (no bogus deltas).
    CHECK(ui.frame().verts.size() > off25);

    frame(click_down(14.f, 195.f));
    a = frame(click_up(14.f, 195.f));
    CHECK(a.diff_toggled);
    frame({});
    CHECK(ui.frame().verts.size() == off25);  // exact return to off view

    // Jump to 350 via the field (buffer holds "25": clear, then type) and
    // re-enable the diff: a REAL diff view (cursor >= span) grows the list.
    frame(click_down(100.f, 147.f));
    frame(click_up(100.f, 147.f));
    for (int i = 0; i < 2; ++i) {
        gfx::ui_io io{};
        io.backspace = true;
        frame(io);
    }
    {
        gfx::ui_io io{};
        io.chars = {'3', '5', '0'};
        io.enter = true;
        a = frame(io);
    }
    CHECK(sc.cursor() == 350);
    CHECK(sc.hash().value() == live.hashes[350]);
    frame({});
    const std::size_t off350 = ui.frame().verts.size();
    frame(click_down(14.f, 195.f));
    a = frame(click_up(14.f, 195.f));
    CHECK(a.diff_toggled);
    frame({});
    CHECK(ui.frame().verts.size() > off350);  // dp/dv/dw rows rendered

    // --- "-1" scrub-back lands bit-exact mid-leg ----------------------------
    frame(click_down(24.f, 57.f));
    a = frame(click_up(24.f, 57.f));
    CHECK(sc.cursor() == 349);
    CHECK(sc.hash().value() == live.hashes[349]);

    // --- determinism: fresh context + fresh scrubber, same io -> same bytes --
    std::vector<gfx::ui_io> seq = {click_down(24.f, 81.f), click_up(24.f, 81.f),
                                   click_down(32.f, 105.f), click_up(32.f, 105.f),
                                   click_down(14.f, 195.f), click_up(14.f, 195.f),
                                   click_down(100.f, 147.f), click_up(100.f, 147.f),
                                   [] {
                                       gfx::ui_io io{};
                                       io.chars = {'4', '9', '9'};
                                       io.enter = true;
                                       return io;
                                   }()};
    std::vector<gfx::ui_frame> fa, fb;
    game::scrubber_panel p1, p2;
    gfx::ui_context ui1, ui2;
    core::sim::ReplayScrubber s1(w, make_params()), s2(w, make_params());
    REQUIRE(s1.load(live.log, live.cps));
    REQUIRE(s2.load(live.log, live.cps));
    REQUIRE(s1.jump_to(-1));
    REQUIRE(s2.jump_to(-1));
    for (const auto& io : seq) {
        game::scrubber_panel_actions a1;
        ui1.begin_frame(io, font, 800, 600);
        p1.draw(ui1, s1, 0, 0, a1);
        ui1.end_frame();
        fa.push_back(ui1.frame());
    }
    for (const auto& io : seq) {
        game::scrubber_panel_actions a2;
        ui2.begin_frame(io, font, 800, 600);
        p2.draw(ui2, s2, 0, 0, a2);
        ui2.end_frame();
        fb.push_back(ui2.frame());
    }
    REQUIRE(fa.size() == seq.size());
    for (std::size_t i = 0; i < fa.size(); ++i)
        CHECK(frames_equal(fa[i], fb[i]));
    CHECK(s1.cursor() == s2.cursor());
    CHECK(s1.hash().value() == live.hashes[499]);
}
