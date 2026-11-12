// Phase 8 task 8.4 — manifest_browser_panel: a manifest.bin view (relative
// paths == content IDs, dep graph, cooked sizes, re-cook status) with the
// selection handing its def to the 8.2 forms panel (#48 kit spine).
// Pins: the REAL fixture tree cooked by the real cooklet produces exactly
// 3 manifest rows with pinned IDs and dep edges; the panel draws them
// deterministically (byte-identical frames across fresh contexts); row
// clicks select + hand the .toml source to def_forms_panel (which then
// finds its leaves); dep clicks navigate the graph; stale + NOT COOKED
// badges change the draw; scroll clamps and scrolled-away rows lose hit
// rects; a selection that leaves the snapshot drops with its handoff.
#include <unistd.h>  // getpid: unique staging roots

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/cook/cook.h>
#include <ve/game/def_forms_panel.h>
#include <ve/game/def_schema.h>
#include <ve/game/manifest_browser_panel.h>
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
             (std::string("ve_mani_") + name + "_" + std::to_string(ve_test::pid()));
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

std::string read_text(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}

}  // namespace

TEST_CASE("manifest_browser_panel") {
    // ---- stage + COOK the real fixture tree (same doctrine as 8.2) ------
    const auto root = std::filesystem::temp_directory_path() /
                      ("ve_mani_tree_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "defs");
    std::filesystem::create_directories(root / "samples");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb",
                          root / "samples" / "test_ship.glb");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/defs/test_ship.def.toml",
                          root / "defs" / "test_ship.def.toml");
    const auto out = root / "out";
    {
        const auto r = ve::cook::cook_incremental(root.string(), out.string());
        REQUIRE_MESSAGE(r.ok, r.error);
    }

    // ---- manifest fixture pinned NUMERICALLY at the host side ----------
    core::cooked::Manifest manifest;
    std::string err;
    REQUIRE_MESSAGE(core::cooked::read_manifest((out / "manifest.bin").string(), manifest, &err), err);
    REQUIRE(manifest.size() == 3);
    const std::string def_id = "defs/test_ship.def.toml";
    const std::string glb_id = "samples/test_ship.glb";
    const std::string sock_id = "samples/test_ship.glb.sock";
    auto it = manifest.begin();
    CHECK(it->first == def_id);   // std::map order: content-ID ascending
    CHECK((++it)->first == glb_id);
    CHECK((++it)->first == sock_id);
    CHECK(manifest[def_id].deps == std::vector<std::string>{glb_id});   // refs edge
    CHECK(manifest[glb_id].deps.empty());                              // leaf
    CHECK(manifest[sock_id].deps == std::vector<std::string>{glb_id}); // companion

    // Host-side snapshot assembly (exactly what the watcher host does):
    // manifest rows + stat of the cooked unit + live-source sha256.
    auto build_rows = [&](bool mark_stale) {
        std::vector<game::manifest_row> rows;
        for (const auto& [id, e] : manifest) {
            game::manifest_row r;
            r.id = id;
            r.deps = e.deps;
            const auto ck = out / "units" / (id + ".ck");
            if (std::filesystem::exists(ck))
                r.cooked_size = std::filesystem::file_size(ck);
            if (mark_stale) {
                const auto srcp = root / id;
                if (std::filesystem::exists(srcp)) {
                    core::cooked::Bytes bytes;
                    REQUIRE(core::cooked::read_file_bytes(srcp.string(), bytes, &err));
                    r.stale = core::cooked::sha256(bytes) != e.source_sha;
                }
            }
            rows.push_back(std::move(r));
        }
        return rows;
    };

    game::manifest_browser_panel panel;
    panel.def_source = [&](const std::string& id) -> std::optional<std::string> {
        if (id.size() > 5 && id.compare(id.size() - 5, 5, ".toml") == 0)
            return read_text(root / id);
        return std::nullopt;
    };
    panel.set_rows(build_rows(false));
    CHECK(panel.row_count() == 3);

    gfx::font0 font;
    prep_font(font);
    gfx::ui_context ui;
    const std::int32_t h_all = 700;
    auto render = [&](gfx::ui_context& u, std::int32_t h) {
        const gfx::ui_io none{};
        u.begin_frame(none, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);
        u.end_frame();
    };
    auto click = [&](gfx::ui_context& u, const std::string& key, std::int32_t h) {
        const auto r = panel.rect_for(key);
        REQUIRE(r.has_value());
        const float cx = static_cast<float>(r->x + r->w / 2);
        const float cy = static_cast<float>(r->y + r->h / 2);
        const gfx::ui_io down = click_down(cx, cy), up = click_up(cx, cy);
        u.begin_frame(down, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);
        u.end_frame();
        u.begin_frame(up, font, 1280, 720);
        panel.render(u, 8, 8, 720, h);
        u.end_frame();
    };

    // ---- determinism: two fresh contexts, same data -> identical frames --
    {
        gfx::ui_context ua, ub;
        render(ua, h_all);
        render(ub, h_all);
        CHECK(frames_equal(ua.frame(), ub.frame()));
    }

    // ---- select the def -> handoff feeds the 8.2 forms panel ------------
    click(ui, "mrow:" + def_id, h_all);
    REQUIRE(panel.selected_id() != nullptr);
    CHECK(*panel.selected_id() == def_id);
    REQUIRE(panel.handoff() != nullptr);
    CHECK(panel.handoff()->id == def_id);
    CHECK(panel.handoff()->source.find("refs = ") != std::string::npos);
    {
        game::def_forms_doc doc;
        REQUIRE_MESSAGE(doc.load(game::kHull_def_schema, panel.handoff()->source, &err), err);
        game::def_forms_panel forms(std::move(doc));
        CHECK(forms.leaf_index("grid.tolerance") >= 0);
        gfx::ui_context uf;
        uf.begin_frame(gfx::ui_io{}, font, 1280, 720);
        forms.render(uf, "def forms", 8, 8, 720, 700, forms.leaf_index("grid.tolerance"), 10);
        uf.end_frame();
        CHECK(forms.field_rect("grid.tolerance").has_value());
    }

    // ---- dep click navigates the graph; non-def clears the handoff ------
    click(ui, "dep:" + glb_id, h_all);
    REQUIRE(panel.selected_id() != nullptr);
    CHECK(*panel.selected_id() == glb_id);
    CHECK(panel.handoff() == nullptr);   // provider refuses the mesh
    CHECK(panel.rect_for("dep:" + glb_id).has_value());

    // ---- NOT COOKED: delete a cooked unit, host re-stats -> badge ------
    {
        render(ui, h_all);
        const std::size_t before = glyph_verts(ui.frame());
        std::filesystem::remove(out / "units" / (glb_id + ".ck"));
        panel.set_rows(build_rows(false));   // glb still selected; size 0 now
        render(ui, h_all);
        CHECK_LT(glyph_verts(ui.frame()), before);  // "cooked: N bytes" -> "NOT COOKED"
        const auto rc = ve::cook::cook_incremental(root.string(), out.string());
        REQUIRE_MESSAGE(rc.ok, rc.error);      // unit back on disk
    }

    // ---- STALE: edit the source WITHOUT re-cooking -> badge on row+detail
    {
        // Baseline WITH the def already selected: the only delta after the
        // stale snapshot is the badges themselves (selection unchanged), so
        // this pin cannot be satisfied by a selection-change glyph skew.
        click(ui, "mrow:" + def_id, h_all);
        render(ui, h_all);
        const std::size_t before = glyph_verts(ui.frame());
        std::ofstream f(root / def_id, std::ios::binary | std::ios::app);
        f << "# edited but not re-cooked\n";
        f.close();
        auto rows = build_rows(true);
        bool any_stale = false;
        for (const auto& r : rows) any_stale = any_stale || r.stale;
        CHECK(any_stale);
        panel.set_rows(std::move(rows));
        render(ui, h_all);
        CHECK_GT(glyph_verts(ui.frame()), before);  // STALE badge drawn
    }

    // ---- scroll + whole-row culling + graph-jump auto-scroll ------------
    // Synthetic snapshot (panel is data-driven; host may inject any rows):
    std::vector<game::manifest_row> synth;
    for (char c = 'a'; c <= 'h'; ++c) {
        game::manifest_row r;
        r.id = std::string(1, c);
        r.deps = {"z"};
        r.cooked_size = 1;
        synth.push_back(std::move(r));
    }
    synth.back().deps = {"a"};   // h -> a jump link for the auto-scroll pin
    panel.set_rows(std::move(synth));
    panel.def_source = {};
    const std::int32_t h_small = 152;  // (152-16-8-48)/24 = 3 visible rows
    render(ui, h_small);
    CHECK(panel.rect_for("mrow:a").has_value());
    CHECK(panel.rect_for("mrow:c").has_value());
    CHECK_FALSE(panel.rect_for("mrow:d").has_value());  // culled: NO hit rect
    click(ui, "scroll_dn", h_small);
    render(ui, h_small);
    CHECK(panel.scroll() == 1);
    CHECK_FALSE(panel.rect_for("mrow:a").has_value());  // scrolled away
    CHECK(panel.rect_for("mrow:d").has_value());
    for (int i = 0; i < 20; ++i) click(ui, "scroll_dn", h_small);
    CHECK(panel.scroll() == 5);   // clamped: 8 rows, 3 visible
    render(ui, h_small);
    CHECK(panel.rect_for("mrow:h").has_value());
    click(ui, "mrow:h", h_small);
    CHECK(panel.rect_for("dep:a").has_value());
    click(ui, "dep:a", h_small);          // graph jump scrolls UP into view
    CHECK(panel.scroll() == 0);
    CHECK(*panel.selected_id() == "a");
    render(ui, h_small);
    CHECK_FALSE(panel.rect_for("mrow:h").has_value());

    // ---- selection leaving the snapshot drops selection + handoff -------
    panel.def_source = [](const std::string&) -> std::optional<std::string> {
        return "x = 1\n";   // any id answers, proves the drop is about the row
    };
    click(ui, "mrow:a", h_small);
    CHECK(panel.handoff() != nullptr);
    std::vector<game::manifest_row> shrunk;
    game::manifest_row only;
    only.id = "b";
    shrunk.push_back(std::move(only));
    panel.set_rows(std::move(shrunk));
    CHECK(panel.selected_id() == nullptr);
    CHECK(panel.handoff() == nullptr);
}
