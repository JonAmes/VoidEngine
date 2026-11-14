// test: def_forms_panel, def_forms_writeback_blocked — 8.2 (#43 mode C,
// #48 editor kit, #16 write-back rule). Schema-driven forms over the REAL
// Phase 5 fixture (assets/defs/test_ship.def.toml), headless (ui_io in,
// draw list out). Pins:
//   - the doc model loads the ratified fixture: every schema leaf has a
//     path ("grid.tolerance", "component.7.facet.1.target", "wire.0.id"),
//     nested [[component.facet]] rows index per parent;
//   - edits re-emit ONLY the touched line: comments, headers, and every
//     untouched line stay byte-identical (minimal git-diffable delta);
//   - array leaves (hull_com) commit per element; attr maps re-emit as a
//     block with quotes restored;
//   - enum widgets cycle ratified spellings; two same-spelled enums in one
//     window stay independent (button id salts);
//   - pickers click WHOLE candidates only; ".target" candidates are derived
//     from the def's own wire ids (garbage-impossible refs, #28a);
//   - add_child appends a schema-shaped [[row]] block;
//   - determinism: fresh panel + fresh ui over the same io sequence ->
//     byte-identical frames;
//   - writeback_blocked: a gate rejection keeps the rejected value OUT of
//     the write buffer and OUT of the source text (field error shown),
//     while earlier gated edits stay; entry grammar (unit dim) rejects
//     before the gate is even consulted.
//   - the gate IS the real cooklet (staged tree, cook_incremental incl.
//     hull validate) — writeback_blocked refuses an unregistered attr key
//     the entry grammar cannot know about;
//   - recook_hotswap: edit -> TOML delta -> re-cook -> the cooked payload
//     on disk carries the new stat and not the old one (#16 loop closed).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/cook/cook.h>
#include <ve/game/def_forms_panel.h>
#include <ve/game/def_schema.h>
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
             (std::string("ve_forms_") + name + "_" + std::to_string(ve_test::pid()));
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

void prep_font(gfx::font0& font) {
    const auto font_src = std::filesystem::path(VE_SOURCE_DIR) / "assets/fonts/DejaVuSansMono.ttf";
    REQUIRE(std::filesystem::exists(font_src));
    const std::string src = tmp_root("src");
    const std::string out = tmp_root("out");
    std::filesystem::copy_file(font_src, std::filesystem::path(src) / "DejaVuSansMono.ttf");
    auto font_cook = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(font_cook.ok, font_cook.error);
    std::string err;
    REQUIRE_MESSAGE(gfx::load_cooked_font_file(out + "/units/DejaVuSansMono.ttf.ck", font, &err),
                    err);
}

std::string read_fixture() {
    const auto p = std::filesystem::path(VE_SOURCE_DIR) / "assets/defs/test_ship.def.toml";
    REQUIRE(std::filesystem::exists(p));
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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
gfx::ui_io type(std::vector<char> chars, bool enter = false) {
    gfx::ui_io io{};
    io.chars = std::move(chars);
    io.enter = enter;
    return io;
}
gfx::ui_io erase() {
    gfx::ui_io io{};
    io.backspace = true;
    return io;
}
std::vector<char> lit(const char* s) { return {s, s + std::strlen(s)}; }

std::size_t count_occurrences(const std::string& hay, const std::string& needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
    return n;
}

game::def_forms_doc load_doc() {
    game::def_forms_doc doc;
    std::string err;
    const bool ok = doc.load(game::kHull_def_schema, read_fixture(), &err);
    REQUIRE_MESSAGE(ok, err);
    return doc;
}

}  // namespace

// ---- real watch-cook gate (#16 loop, test-side) ---------------------------
// Stages the fixture + its ../samples/test_ship.glb ref under a temp tree;
// the gate writes the panel's candidate source there and runs the REAL
// cooklet (hulldef parse + hull validate). Empty return = accepted; the
// cooklet's first error otherwise. ve_game itself stays cook-free; the
// HOST wires this half — exactly how the watch-loop consumes take_write.
struct cook_gate {
    std::filesystem::path root;
    int recooks = 0;
    std::string last_error;
    explicit cook_gate(const char* tag)
        : root(std::filesystem::temp_directory_path() /
               ("ve_forms_" + std::string(tag) + "_" + std::to_string(ve_test::pid()))) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "samples");
        std::filesystem::create_directories(root / "defs");
        std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb",
                              root / "samples" / "test_ship.glb");
        std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/defs/test_ship.def.toml",
                              root / "defs" / "test_ship.def.toml");
    }
    std::string operator()(const std::string& src) {
        {
            std::ofstream f(root / "defs" / "test_ship.def.toml",
                            std::ios::binary | std::ios::trunc);
            f << src;
        }
        const auto r = ve::cook::cook_incremental(root.string(), (root / "out").string());
        ++recooks;
        last_error = r.ok ? std::string{} : r.error;
        return last_error;
    }
};

// the f64 that follows the FIRST occurrence of an attr key string in a
// cooked payload (attrs encode as {str key, f64}; DEF0 writes components in
// authored order, so this is component.0's stat).
// (plain memcmp scan: std::search across mixed char/unsigned-char iterator
// value types silently misbehaves on libstdc++ here.)
std::optional<double> first_attr_f64(const std::vector<unsigned char>& b, std::string_view key) {
    for (std::size_t i = 0; i + key.size() + 8 <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, key.data(), key.size()) != 0) continue;
        double v;
        std::memcpy(&v, b.data() + i + key.size(), 8);
        return v;
    }
    return std::nullopt;
}

TEST_CASE("def_forms_panel") {
    auto doc = load_doc();

    // ---- doc model over the ratified fixture --------------------------
    CHECK(doc.value("hull_mass") == "98000");
    CHECK(doc.value("grid.tolerance") == "0.1 m");
    CHECK(doc.value("component.7.facet.1.target") == "w_engL");   // panel_main repair
    CHECK(doc.value("component.7.id") == "panel_main");
    CHECK(doc.value("wire.0.id") == "w_engL");
    CHECK(doc.value("hull_com.1") == "0.25 m");
    CHECK(doc.value("section.0.name") == "bridge");
    CHECK(doc.value("component.0.attrs").find("thrust.gimbal_rad=15 deg") !=
          std::string::npos);  // quotes stripped for the field text
    CHECK(doc.field("grid.cell")->kind == game::field_kind::unit_str);
    CHECK(doc.field("component.0.category")->kind == game::field_kind::enumeration);

    game::def_forms_panel panel(std::move(doc));

    gfx::font0 font;
    prep_font(font);

    // ui_io must OUTLIVE begin_frame..end_frame (the context keeps a
    // pointer for the whole frame, ui.cpp io_) — never pass temporaries.
    auto frame_of = [&](gfx::ui_context& ui, const gfx::ui_io& io, const char* path, int span) {
        const int idx = panel.leaf_index(path);
        REQUIRE_GE(idx, 0);
        ui.begin_frame(io, font, 1280, 720);
        panel.render(ui, "def forms", 8, 8, 720, 700, idx, span);
        ui.end_frame();
    };
    auto render = [&](gfx::ui_context& ui, const char* path, int span = 10) {
        const gfx::ui_io none{};
        frame_of(ui, none, path, span);
    };
    auto click_center = [&](gfx::ui_context& ui, const char* path) {
        const auto r = panel.field_rect(path);
        REQUIRE(r.has_value());
        const float cx = static_cast<float>(r->x + r->w / 2), cy = static_cast<float>(r->y + r->h / 2);
        const gfx::ui_io down = click_down(cx, cy), up = click_up(cx, cy);
        frame_of(ui, down, path, 10);
        frame_of(ui, up, path, 10);
    };
    auto type_into = [&](gfx::ui_context& ui, const char* path, const char* text,
                         std::size_t erase_len) {
        for (std::size_t i = 0; i < erase_len; ++i) {
            const gfx::ui_io io = erase();
            frame_of(ui, io, path, 10);
        }
        const gfx::ui_io io = type(lit(text), true);
        frame_of(ui, io, path, 10);
    };

    // ---- unit field commit: line-preserved write-back ------------------
    {
        gfx::ui_context ui;
        render(ui, "grid.tolerance");
        click_center(ui, "grid.tolerance");
        type_into(ui, "grid.tolerance", "0.15 m", 6);  // draft held "0.1 m"
        render(ui, "grid.tolerance");                  // commit frame processed
    }
    CHECK(panel.dirty());
    {
        const auto& src = panel.doc().source();
        CHECK(src.find("tolerance = \"0.15 m\"") != std::string::npos);
        CHECK(src.find("subcell_ratio = 4") != std::string::npos);
        // untouched lines byte-identical incl. comments:
        CHECK(count_occurrences(src, "# Sections tile the hull without overlap (#19a).") == 1);
        CHECK(count_occurrences(src, "cell = \"2 m\"") == 1);
        CHECK(count_occurrences(src, "[[component]]") == 11);
    }

    // ---- array element commit (hull_com.0) -----------------------------
    {
        gfx::ui_context ui;
        render(ui, "hull_com.0", 8);
        click_center(ui, "hull_com.0");
        type_into(ui, "hull_com.0", "0.5 m", 4);
        render(ui, "hull_com.0", 8);
    }
    CHECK(panel.doc().value("hull_com.0") == "0.5 m");
    CHECK(panel.doc().source().find("hull_com = [ \"0.5 m\", \"0.25 m\", \"0.4 m\" ]") !=
          std::string::npos);

    // ---- enum cycle + id salts (two "main" categories, independent) ----
    {
        gfx::ui_context ui;
        const int idx = panel.leaf_index("component.0.category");
        ui.begin_frame({}, font, 1280, 720);
        panel.render(ui, "def forms", 8, 8, 720, 700, idx, 30);  // covers comp 0 and 1
        ui.end_frame();
        const auto r = panel.field_rect("component.0.category");
        REQUIRE(r.has_value());
        const float cx = static_cast<float>(r->x + r->w / 2), cy = static_cast<float>(r->y + r->h / 2);
        const gfx::ui_io down = click_down(cx, cy), up = click_up(cx, cy);
        ui.begin_frame(down, font, 1280, 720);
        panel.render(ui, "def forms", 8, 8, 720, 700, idx, 30);
        ui.end_frame();
        ui.begin_frame(up, font, 1280, 720);
        panel.render(ui, "def forms", 8, 8, 720, 700, idx, 30);
        ui.end_frame();
    }
    CHECK(panel.doc().value("component.0.category") == "rcs");      // main -> rcs
    CHECK(panel.doc().value("component.1.category") == "main");     // untouched sibling

    // ---- picker: candidates derived from the def's own wire ids --------
    {
        gfx::ui_context ui;
        const char* path = "component.7.facet.1.target";
        render(ui, path, 6);
        click_center(ui, path);  // opens the picker ([pick] is the primary rect)
        render(ui, path, 12);    // candidates now drawn
        const auto cr = panel.candidate_rect(path, 1);  // wire order: w_engL, w_engR
        REQUIRE(cr.has_value());
        const float cx = static_cast<float>(cr->x + cr->w / 2), cy = static_cast<float>(cr->y + cr->h / 2);
        const gfx::ui_io down = click_down(cx, cy), up = click_up(cx, cy);
        ui.begin_frame(down, font, 1280, 720);
        panel.render(ui, "def forms", 8, 8, 720, 700, panel.leaf_index(path), 12);
        ui.end_frame();
        ui.begin_frame(up, font, 1280, 720);
        panel.render(ui, "def forms", 8, 8, 720, 700, panel.leaf_index(path), 12);
        ui.end_frame();
    }
    CHECK(panel.doc().value("component.7.facet.1.target") == "w_engR");
    CHECK(panel.doc().source().find("target = \"w_engR\"") != std::string::npos);

    // ---- attr map: value edited through the composite field ------------
    {
        gfx::ui_context ui;
        render(ui, "component.0.attrs", 4);
        click_center(ui, "component.0.attrs");
        type_into(ui, "component.0.attrs",
                  "mass=900, power.draw=25000, thrust.force=350000, thrust.isp=300, "
                  "thrust.gimbal_rad=15 deg",
                  panel.doc().value("component.0.attrs").size());
        render(ui, "component.0.attrs", 4);
    }
    {
        const auto& src = panel.doc().source();
        CHECK(src.find("thrust.force = 350000") != std::string::npos);
        CHECK(src.find("thrust.gimbal_rad = \"15 deg\"") !=
              std::string::npos);  // NUMBER SUFFIX re-quoted (valid TOML)
        CHECK(src.find("[component.attrs]") != std::string::npos);
    }

    // ---- add_child: schema-shaped block appended ------------------------
    {
        std::string err;
        REQUIRE(panel.doc_mut().add_child("wire", &err));
        const auto& src = panel.doc().source();
        CHECK(src.find("[[wire]]") != std::string::npos);
        CHECK(count_occurrences(src, "[[wire]]") == 7);
        CHECK(src.find("id = \"new_part\"") != std::string::npos);
        CHECK(panel.leaf_index("wire.6.id") >= 0);
        CHECK(panel.doc().value("wire.6.from") == "component.new_part");
    }

    // ---- take_write: host gets the full source, dirty clears ------------
    const std::string written = panel.take_write();
    CHECK_FALSE(panel.dirty());
    CHECK(written == panel.doc().source());
    CHECK(written.find("tolerance = \"0.15 m\"") != std::string::npos);
    CHECK(written.find("hull_mass = 98000") != std::string::npos);

    // ---- determinism: fresh panel, same io sequence, byte-identical ----
    auto replay = [&]() {
        game::def_forms_panel p2(load_doc());
        gfx::ui_context ui;
        std::vector<gfx::ui_frame> frames;
        auto rf = [&](const gfx::ui_io& io, int first, int count) {
            ui.begin_frame(io, font, 1280, 720);
            p2.render(ui, "def forms", 8, 8, 720, 700, first, count);
            ui.end_frame();
            frames.push_back(ui.frame());
        };
        rf({}, 0, 20);
        rf(click_down(400.f, 40.f), 0, 20);
        rf(click_up(400.f, 40.f), 0, 20);
        rf(erase(), 0, 20);
        rf(type(lit("0.2 m"), true), 0, 20);
        rf({}, 100, 15);
        rf(click_down(30.f, 30.f), 100, 15);
        rf(click_up(30.f, 30.f), 100, 15);
        rf({}, 0, 20);
        return frames;
    };
    const auto fa = replay(), fb = replay();
    REQUIRE(fa.size() == fb.size());
    for (std::size_t i = 0; i < fa.size(); ++i)
        CHECK_MESSAGE(frames_equal(fa[i], fb[i]), "frame " << i);
}

TEST_CASE("def_forms_writeback_blocked") {
    auto doc = load_doc();
    game::def_forms_panel panel(std::move(doc));
    // Host gate = the REAL cooklet (parse + hull validate) over a staged
    // tree — the same contract the watch-loop applies to take_write (#16).
    cook_gate cg("blocked");
    panel.gate = [&](const std::string& src) { return cg(src); };

    gfx::font0 font;
    prep_font(font);
    gfx::ui_context ui;
    auto render = [&](const char* path, int span = 10) {
        const int idx = panel.leaf_index(path);
        REQUIRE_GE(idx, 0);
        const gfx::ui_io none{};
        ui.begin_frame(none, font, 1280, 720);
        panel.render(ui, "def forms", 8, 8, 720, 700, idx, span);
        ui.end_frame();
    };
    auto wb_frame = [&](const gfx::ui_io& io, const char* path, int span) {
        ui.begin_frame(io, font, 1280, 720);
        panel.render(ui, "def forms", 8, 8, 720, 700, panel.leaf_index(path), span);
        ui.end_frame();
    };

    // good edit first (gate passes) ------------------------------------
    {
        render("grid.tolerance");
        const auto r = panel.field_rect("grid.tolerance");
        REQUIRE(r.has_value());
        const float cx = static_cast<float>(r->x + r->w / 2), cy = static_cast<float>(r->y + r->h / 2);
        const gfx::ui_io down = click_down(cx, cy), up = click_up(cx, cy);
        wb_frame(down, "grid.tolerance", 10);
        wb_frame(up, "grid.tolerance", 10);
        for (int i = 0; i < 6; ++i) {
            const gfx::ui_io io = erase();
            wb_frame(io, "grid.tolerance", 10);
        }
        const gfx::ui_io fin = type(lit("0.3 m"), true);
        wb_frame(fin, "grid.tolerance", 10);
    }
    CHECK(panel.dirty());

    // gate-blocked edit (attr map gets 999999) --------------------------
    {
        render("component.0.attrs", 4);
        const auto r = panel.field_rect("component.0.attrs");
        REQUIRE(r.has_value());
        const float cx = static_cast<float>(r->x + r->w / 2), cy = static_cast<float>(r->y + r->h / 2);
        const gfx::ui_io down = click_down(cx, cy), up = click_up(cx, cy);
        wb_frame(down, "component.0.attrs", 4);
        wb_frame(up, "component.0.attrs", 4);
        for (std::size_t i = 0; i < panel.doc().value("component.0.attrs").size(); ++i) {
            const gfx::ui_io io = erase();
            wb_frame(io, "component.0.attrs", 4);
        }
        const gfx::ui_io fin = type(
            lit("mass=900, power.draw=25000, thrust.force=320000, thrust.isp=300, "
                "thrust.gimbal_rad=15 deg, foo.bar=1"), true);  // unregistered key
        wb_frame(fin, "component.0.attrs", 4);
        render("component.0.attrs", 4);  // BLOCKED label renders under field
    }
    CHECK_MESSAGE(panel.last_gate_error().find("foo.bar") != std::string::npos,
                  panel.last_gate_error());
    CHECK(panel.last_gate_error().find("registry") != std::string::npos);
    CHECK(cg.last_error == panel.last_gate_error());  // reason came from the cooklet
    CHECK(panel.doc().value("component.0.attrs").find("foo.bar") == std::string::npos);
    const auto av = panel.doc().value("component.0.attrs");
    CHECK(av.find("thrust.force=320000") != std::string::npos);  // reverted (leaf form)

    // write buffer carries the GOOD edit only — the blocked one never
    // enters the source at all (#16: gate BEFORE write; git is undo).
    const std::string written = panel.take_write();
    CHECK(written.find("foo.bar") == std::string::npos);
    CHECK(written.find("tolerance = \"0.3 m\"") != std::string::npos);
    CHECK_FALSE(panel.dirty());

    // entry grammar rejects BEFORE the gate: dim-mismatched unit never
    // reaches the gate fn (count gate calls).
    int gate_calls = 0;
    panel.gate = [&](const std::string&) {
        ++gate_calls;
        return std::string{};
    };
    {
        render("grid.cell", 6);
        const auto r = panel.field_rect("grid.cell");
        REQUIRE(r.has_value());
        const float cx = static_cast<float>(r->x + r->w / 2), cy = static_cast<float>(r->y + r->h / 2);
        const gfx::ui_io down = click_down(cx, cy), up = click_up(cx, cy);
        wb_frame(down, "grid.cell", 6);
        wb_frame(up, "grid.cell", 6);
        for (int i = 0; i < 4; ++i) {
            const gfx::ui_io io = erase();
            wb_frame(io, "grid.cell", 6);
        }
        const gfx::ui_io fin = type(lit("2 s"), true);  // time in a length field
        wb_frame(fin, "grid.cell", 6);
    }
    CHECK(panel.doc().value("grid.cell") == "2 m");
    const int calls_after_bad_entry = gate_calls;
    for (int i = 0; i < 1; ++i) render("grid.cell", 6);
    CHECK(gate_calls == calls_after_bad_entry);  // entry rejection consulted no gate
}

TEST_CASE("def_forms_recook_hotswap") {
    // The full #16 loop in one case: form edit -> gated candidate source
    // written to the staged tree -> real cooklet re-cooks (watch-mode's
    // cook_incremental) -> the cooked payload the engine memory-maps has
    // the new value. Panel still never touches disk itself.
    std::ifstream in(std::string(VE_SOURCE_DIR) + "/assets/defs/test_ship.def.toml");
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    game::def_forms_doc doc;
    std::string err;
    REQUIRE(doc.load(game::kHull_def_schema, src, &err));
    game::def_forms_panel panel(std::move(doc));
    cook_gate cg("hotswap");
    panel.gate = [&](const std::string& s2) { return cg(s2); };

    gfx::font0 font;
    prep_font(font);
    gfx::ui_context ui;
    auto wb_frame = [&](const gfx::ui_io& io, const char* path, int span) {
        ui.begin_frame(io, font, 1280, 720);
        panel.render(ui, "def forms", 8, 8, 720, 700, panel.leaf_index(path), span);
        ui.end_frame();
    };
    {
        const gfx::ui_io none{};
        wb_frame(none, "component.0.attrs", 4);
        const auto r = panel.field_rect("component.0.attrs");
        REQUIRE(r.has_value());
        const float cx = static_cast<float>(r->x + r->w / 2), cy = static_cast<float>(r->y + r->h / 2);
        const gfx::ui_io down = click_down(cx, cy), up = click_up(cx, cy);
        wb_frame(down, "component.0.attrs", 4);
        wb_frame(up, "component.0.attrs", 4);
        for (std::size_t i = 0; i < panel.doc().value("component.0.attrs").size(); ++i) {
            const gfx::ui_io io = erase();
            wb_frame(io, "component.0.attrs", 4);
        }
        const gfx::ui_io fin = type(
            lit("mass=900, power.draw=25000, thrust.force=350000, thrust.isp=300, "
                "thrust.gimbal_rad=15 deg"), true);
        wb_frame(fin, "component.0.attrs", 4);
    }
    CHECK(panel.dirty());
    CHECK_GE(cg.recooks, 1);            // every accepted commit round-tripped
    CHECK(panel.last_gate_error().empty());

    // The cooked unit on disk (what the engine hot-swaps in) now carries
    // 350 kN and no longer 320 kN — the loop is closed without the panel
    // ever writing a byte or touching a payload (#16).
    ve::cook::Unit du;
    REQUIRE(ve::cook::read_unit_file((cg.root / "out/units/defs/test_ship.def.toml.ck").string(),
                                     du, &err));
    CHECK(du.id == "defs/test_ship.def.toml");
    // component.0 (engine_L — first in authored/payload order) now stats
    // 350 kN; engine_R's separate attrs still say 320 kN (untouched row),
    // which is exactly right: the swap is per-component, not global.
    const auto v0 = first_attr_f64(du.payload, "thrust.force");
    REQUIRE(v0.has_value());
    CHECK(*v0 == 350000.0);

    // TOML delta is still minimal: one line changed in the whole file.
    const std::string delta = panel.take_write();
    CHECK(delta.find("thrust.force = 350000") != std::string::npos);
    int changed = 0;
    {
        std::ifstream again(std::string(VE_SOURCE_DIR) + "/assets/defs/test_ship.def.toml");
        std::string orig((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
        std::size_t a = 0, b = 0;
        while (a < orig.size() && b < delta.size()) {
            if (orig[a] != delta[b]) { ++changed; while (a < orig.size() && orig[a++] != '\n'); while (b < delta.size() && delta[b++] != '\n'); }
            else { ++a; ++b; }
        }
        CHECK(changed == 1);
    }
}
