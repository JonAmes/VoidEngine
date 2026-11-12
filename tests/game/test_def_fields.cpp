// test: def_form_fields, def_form_refpicker — 8.1 (#43 mode-C forms, #48
// editor kit). Typed def-form widgets on the 3.13 immediate UI (panel
// pattern per 3.16): ui_io in, draw list out, headless. Pins:
//   - unit_str fields enforce #42 ON ENTRY through the SHARED grammar
//     (game/src/units.cpp, now the cooklet's parser too): ratified suffix,
//     no bare numbers, strict number, trim-on-commit — and the schema's
//     DIM gate rejects wrong-family suffixes the cooklet itself would
//     coerce ("5 s" in a length field is a form-level rejection);
//   - rejected drafts keep the committed value untouched, show the
//     cooklet's own message, and stay editable;
//   - number/integer/text/tag grammars reject "inf", "4.5", blank, and
//     "Bad Key!" without touching committed text;
//   - enum fields cycle the ratified set (main->rcs->gun->utility->main);
//   - ref pickers commit ONLY whole candidate spellings — typing cannot
//     alter them at all (garbage-impossible cross-refs, #26a/#28a);
//   - determinism: fresh context + fresh field states, same io sequence
//     -> byte-identical frames.
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/cook/cook.h>
#include <ve/game/def_fields.h>
#include <ve/game/def_schema.h>
#include <ve/game/units.h>
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
             (std::string("ve_deff_") + name + "_" + std::to_string(ve_test::pid()));
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

// Field-state set + edits for one frame (layout order fixed; base y per
// 3.13 rules in a (0,0,320,600) window, DejaVu Mono 16px, text_h=16):
//   unit   label 24, box y 42..60   (center x=160, y=51)
//   mass   base y 66,      box 84..102   (160, 93)
//   ratio  base y 108,     box 126..144  (160, 135)
//   type   base y 150,     box 168..186  (160, 177)
//   tag    base y 192,     box 210..228  (160, 219)
//   enum   label 234, button 256..274    (32, 265)
//   bool   row 280..298                  (14, 289)
//   pick   labels 304/326, [pick] 348..366 (30, 357); open: cands 372/396/420
struct form {
    game::edit_field unit, mass, ratio, type, tag;
    game::enum_field cat;
    bool manual = false;
    game::picker_field from;
    std::vector<std::string> cands = {"socket.hp_front", "component.engine0",
                                      "component.reactor"};
    form() {
        game::prime_field(unit, "12 m");
        game::prime_field(mass, "50000");
        game::prime_field(ratio, "4");
        game::prime_field(type, "engine");
        game::prime_field(tag, "class.freighter");
        game::prime_enum_field(cat, "category", "main");
    }
};
struct edits {
    game::field_edit unit = game::field_edit::none, mass = game::field_edit::none,
                     ratio = game::field_edit::none, type = game::field_edit::none,
                     tag = game::field_edit::none, cat = game::field_edit::none,
                     manual = game::field_edit::none, from = game::field_edit::none;
};

void draw_form(gfx::ui_context& ui, form& f, edits& e) {
    ui.begin_window("def", 0, 0, 320, 600);
    e.unit = game::draw_unit_field(ui, "hull_com[0]", f.unit, game::unit_dim::length);
    e.mass = game::draw_number_field(ui, "hull_mass", f.mass);
    e.ratio = game::draw_integer_field(ui, "grid.subcell_ratio", f.ratio);
    e.type = game::draw_text_field(ui, "comp.type", f.type);
    e.tag = game::draw_tag_field(ui, "class_tag", f.tag);
    e.cat = game::draw_enum_field(ui, "component.category", f.cat);
    e.manual = game::draw_bool_field(ui, "facet.manual", f.manual);
    e.from = game::draw_picker_field(ui, "wire.from", f.from, f.cands);
    ui.end_window();
}

}  // namespace

TEST_CASE("def_form_fields") {
    gfx::font0 font;
    prep_font(font);
    gfx::ui_context ui;
    form f;

    auto frame = [&](const gfx::ui_io& io) {
        edits e;
        ui.begin_frame(io, font, 800, 600);
        draw_form(ui, f, e);
        ui.end_frame();
        return e;
    };

    edits idle = frame({});
    CHECK_FALSE(ui.frame().cmds.empty());
    CHECK(idle.unit == game::field_edit::none);

    // --- unit field: #42 on entry via the SHARED grammar --------------
    frame(click_down(160.f, 51.f));
    frame(click_up(160.f, 51.f));
    for (int i = 0; i < 4; ++i) frame(erase());  // "12 m" -> ""
    auto e = frame(type(lit("5 ft"), true));
    CHECK(e.unit == game::field_edit::rejected);
    CHECK(f.unit.committed == "12 m");  // rejected draft never touches commit
    CHECK(f.unit.error);
    CHECK(f.unit.message.find("unknown suffix") != std::string::npos);
    frame({});  // error label persists (shown under the field)
    for (int i = 0; i < 4; ++i) frame(erase());  // rejected draft stays editable
    e = frame(type(lit("0.4 m"), true));
    CHECK(e.unit == game::field_edit::committed);
    CHECK(f.unit.committed == "0.4 m");
    CHECK_FALSE(f.unit.error);
    CHECK(*game::parse_unit_entry(f.unit.committed, game::unit_dim::length) == doctest::Approx(0.4));

    frame(click_down(160.f, 51.f));
    frame(click_up(160.f, 51.f));
    for (int i = 0; i < 6; ++i) frame(erase());
    e = frame(type(lit("500"), true));  // bare number: hard entry rejection (#42)
    CHECK(e.unit == game::field_edit::rejected);
    CHECK(f.unit.message.find("suffix mandatory per #42") != std::string::npos);
    CHECK(f.unit.committed == "0.4 m");
    for (int i = 0; i < 3; ++i) frame(erase());
    e = frame(type(lit("5 s"), true));  // DIM gate: forms stricter than cook (#42 on entry)
    CHECK(e.unit == game::field_edit::rejected);
    CHECK(f.unit.message.find("is not a length unit") != std::string::npos);
    CHECK(f.unit.committed == "0.4 m");
    for (int i = 0; i < 3; ++i) frame(erase());
    e = frame(type(lit("2 km"), true));
    CHECK(e.unit == game::field_edit::committed);
    CHECK(f.unit.committed == "2 km");
    CHECK(*game::parse_unit_entry(f.unit.committed, game::unit_dim::length) == doctest::Approx(2000.0));
    frame(click_down(160.f, 51.f));  // a COMMIT drops focus (3.13 semantics); re-click
    frame(click_up(160.f, 51.f));
    for (int i = 0; i < 4; ++i) frame(erase());
    e = frame(type(lit(" 1.5 AU "), true));  // outer whitespace trims on commit
    CHECK(e.unit == game::field_edit::committed);
    CHECK(f.unit.committed == "1.5 AU");
    CHECK(*game::si_from_string(f.unit.committed) == doctest::Approx(1.5 * game::kMeters_per_AU));

    // --- number / integer fields --------------------------------------
    frame(click_down(160.f, 93.f));
    frame(click_up(160.f, 93.f));
    for (int i = 0; i < 5; ++i) frame(erase());
    e = frame(type(lit("inf"), true));  // from_chars/isfinite strictness matches cook (#42)
    CHECK(e.mass == game::field_edit::rejected);
    CHECK(f.mass.committed == "50000");
    for (int i = 0; i < 3; ++i) frame(erase());
    e = frame(type(lit("49000"), true));
    CHECK(e.mass == game::field_edit::committed);
    CHECK(f.mass.committed == "49000");

    frame(click_down(160.f, 135.f));
    frame(click_up(160.f, 135.f));
    frame(erase());
    e = frame(type(lit("4.5"), true));
    CHECK(e.ratio == game::field_edit::rejected);
    CHECK(f.ratio.committed == "4");
    for (int i = 0; i < 3; ++i) frame(erase());
    e = frame(type(lit("2"), true));
    CHECK(e.ratio == game::field_edit::committed);
    CHECK(f.ratio.committed == "2");

    // --- text / tag fields ---------------------------------------------
    frame(click_down(160.f, 177.f));
    frame(click_up(160.f, 177.f));
    for (int i = 0; i < 6; ++i) frame(erase());
    e = frame(type(lit(" "), true));
    CHECK(e.type == game::field_edit::rejected);
    CHECK(f.type.committed == "engine");
    frame(erase());  // the typed space
    e = frame(type(lit("reactor"), true));
    CHECK(e.type == game::field_edit::committed);
    CHECK(f.type.committed == "reactor");

    frame(click_down(160.f, 219.f));
    frame(click_up(160.f, 219.f));
    for (int i = 0; i < 15; ++i) frame(erase());
    e = frame(type(lit("Bad Key!"), true));  // dotted-name grammar (5.4 registry)
    CHECK(e.tag == game::field_edit::rejected);
    CHECK(f.tag.committed == "class.freighter");
    for (int i = 0; i < 8; ++i) frame(erase());
    e = frame(type(lit("class.hauler"), true));
    CHECK(e.tag == game::field_edit::committed);
    CHECK(f.tag.committed == "class.hauler");

    // --- enum field: click cycles ratified options only -----------------
    e = frame(click_down(32.f, 265.f));
    e = frame(click_up(32.f, 265.f));
    CHECK(e.cat == game::field_edit::committed);
    CHECK(f.cat.value == "rcs");
    frame(click_down(32.f, 265.f));
    e = frame(click_up(32.f, 265.f));
    CHECK(f.cat.value == "gun");
    frame(click_down(32.f, 265.f));
    e = frame(click_up(32.f, 265.f));
    CHECK(f.cat.value == "utility");
    frame(click_down(32.f, 265.f));
    e = frame(click_up(32.f, 265.f));  // wraps
    CHECK(f.cat.value == "main");

    // --- bool field ------------------------------------------------------
    frame(click_down(14.f, 289.f));
    e = frame(click_up(14.f, 289.f));
    CHECK(e.manual == game::field_edit::committed);
    CHECK(f.manual);
    frame(click_down(14.f, 289.f));
    e = frame(click_up(14.f, 289.f));
    CHECK_FALSE(f.manual);

    // --- ref picker: candidate-only commits ------------------------------
    frame(click_down(30.f, 357.f));
    e = frame(click_up(30.f, 357.f));  // open
    CHECK(f.from.open);
    e = frame(type(lit("socket.zzz"), true));  // nothing to type into: no text field
    CHECK(f.from.committed.empty());
    frame(click_down(30.f, 405.f));
    e = frame(click_up(30.f, 405.f));  // "component.engine0"
    CHECK(e.from == game::field_edit::committed);
    CHECK(f.from.committed == "component.engine0");
    CHECK_FALSE(f.from.open);

    // --- shared grammar equivalence: game entry == cook wrapper -----------
    CHECK(*game::si_from_string("5 km") == doctest::Approx(5000.0));
    auto bare = game::si_from_string("500");
    CHECK_FALSE(bare.has_value());
    CHECK(bare.error().find("suffix mandatory per #42") != std::string::npos);
    auto three = game::si_from_string("1 2 m");
    CHECK(three.error().find("too many tokens") != std::string::npos);
    auto badnum = game::si_from_string("abc m");
    CHECK(badnum.error().find("bad number") != std::string::npos);

    // --- determinism: fresh context + fresh states, same io -> same bytes --
    std::vector<gfx::ui_io> seq = {
        click_down(160.f, 51.f), click_up(160.f, 51.f),
        erase(), erase(), erase(), erase(),
        type(lit("2 km"), true),
        click_down(32.f, 265.f), click_up(32.f, 265.f),
        click_down(30.f, 357.f), click_up(30.f, 357.f),
        click_down(30.f, 405.f), click_up(30.f, 405.f),
        gfx::ui_io{},  // idle frame
    };
    std::vector<gfx::ui_frame> fa, fb;
    for (int run = 0; run < 2; ++run) {
        gfx::ui_context u;
        form g;
        for (const auto& io : seq) {
            edits ed;
            u.begin_frame(io, font, 800, 600);
            draw_form(u, g, ed);
            u.end_frame();
            (run == 0 ? fa : fb).push_back(u.frame());
        }
        if (run == 0) {
            CHECK(g.unit.committed == "2 km");
            CHECK(g.cat.value == "rcs");
            CHECK(g.from.committed == "component.engine0");
        } else {
            CHECK(g.unit.committed == "2 km");
            CHECK(g.cat.value == "rcs");
            CHECK(g.from.committed == "component.engine0");
        }
    }
    REQUIRE(fa.size() == fb.size());
    for (std::size_t i = 0; i < fa.size(); ++i) CHECK(frames_equal(fa[i], fb[i]));
}

TEST_CASE("def_form_refpicker") {
    // Garbage-impossible refs on their own: candidates arrive pre-spelled
    // ("socket.<id>"/"component.<id>" per #26a; raw ids for name_refs) and
    // selection commits the EXACT candidate — keystrokes cannot reach it.
    gfx::font0 font;
    prep_font(font);
    gfx::ui_context ui;
    game::picker_field pick;
    std::vector<std::string> cands = {"socket.hp_front", "component.engine0"};

    auto draw = [&](const gfx::ui_io& io) {
        ui.begin_frame(io, font, 800, 600);
        ui.begin_window("refs", 0, 0, 320, 300);
        const auto edit = game::draw_picker_field(ui, "wire.to", pick, cands);
        ui.end_window();
        ui.end_frame();
        return edit;
    };

    // closed layout: label 24 (->46), current 46 (->68), [pick] 68..86.
    draw({});
    CHECK(pick.committed.empty());
    draw(click_down(30.f, 77.f));
    draw(click_up(30.f, 77.f));  // open: candidates at 94..112, 118..136
    CHECK(pick.open);
    for (const auto& io : {type(lit("garbage."), true), type(lit("socket."), false)})
        draw(io);  // keystrokes land nowhere
    CHECK(pick.committed.empty());
    auto e = [&] {
        draw(click_down(30.f, 103.f));
        return draw(click_up(30.f, 103.f));  // "socket.hp_front"
    }();
    CHECK(e == game::field_edit::committed);
    CHECK(pick.committed == "socket.hp_front");  // exact candidate spelling incl. prefix
    CHECK_FALSE(pick.open);

    // reselect same candidate: idempotent, still exact.
    draw(click_down(30.f, 77.f));
    draw(click_up(30.f, 77.f));
    draw(click_down(30.f, 103.f));
    e = draw(click_up(30.f, 103.f));
    CHECK(e == game::field_edit::committed);
    CHECK(pick.committed == "socket.hp_front");
}
