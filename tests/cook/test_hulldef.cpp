// test: cook_hull_def — 5.8: the sample hull def (assets/defs) cooks with
// the glb fixture into the normalized DEF0 v2 payload (grid, sections,
// components + attrs + facets, catalog, wires, refs; #42 SI coercion), the
// manifest records the def -> glb dependency edge, and payload bytes are
// golden-pinned. test: cook_hull_def_gates — schema failures are loud:
// bad class_tag, missing grid, unknown category, attr keys outside the
// ratified registry (#42 bare/unknown suffixes), bad wire endpoints, dup
// ids, bad section boxes; defs WITHOUT class_tag stay raw passthrough.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../ve_test_portability.h"  // getpid

#include <ve/cook/cook.h>
#include <ve/cook/hulldef.h>
#include <ve/core/cooked.h>

#ifndef VE_SOURCE_DIR
#error "VE_SOURCE_DIR must be defined by CMake"
#endif

namespace {

// Minimal reader for the DEF0 v2 payload (layout documented in cooked.h).
struct Reader {
    const ve::cook::Bytes& b;
    std::size_t at = 0;
    std::uint8_t u8() { return b.at(at++); }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[at + i]) << (8 * i);
        at += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    double f64() {
        double v = 0;
        std::memcpy(&v, &b[at], 8);
        at += 8;
        return v;
    }
    std::string str() {
        const auto n = u32();
        std::string s(reinterpret_cast<const char*>(&b[at]), n);
        at += n;
        return s;
    }
};

std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_hulldef_") + name + "_" + std::to_string(ve_test::pid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}

}  // namespace

TEST_CASE("cook_hull_def") {
    // Stage the real fixture trees under one src tree so the hull def's
    // ../samples refs and its component TYPE defs' refs resolve into the
    // cook set (recursive: samples/ + defs/ incl. defs/comptypes #52a).
    const std::string src = tmp_root("src");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/samples",
                          std::filesystem::path(src) / "samples",
                          std::filesystem::copy_options::recursive);
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/defs",
                          std::filesystem::path(src) / "defs",
                          std::filesystem::copy_options::recursive);

    const std::string out = tmp_root("out");
    auto res = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(res.ok, res.error);
    CHECK(res.counts.failures == 0);

    // manifest: the def gained a dep edge onto the glb content (#14 coupling).
    ve::core::cooked::Manifest mf;
    std::string err;
    REQUIRE(ve::cook::read_manifest(out + "/manifest.bin", mf, &err));
    REQUIRE(mf.count("defs/test_ship.def.toml") == 1u);
    // #52a/#52b: hull deps = consumed type defs (sorted), per-component
    // art edges, then the authored top-level refs.
    CHECK(mf.at("defs/test_ship.def.toml").deps == std::vector<std::string>{
        "defs/comptypes/berth_crew.def.toml", "defs/comptypes/door_hatch.def.toml",
        "defs/comptypes/panel_dash.def.toml", "defs/comptypes/power_battery.def.toml",
        "defs/comptypes/power_reactor.def.toml", "defs/comptypes/seat_pilot.def.toml",
        "defs/comptypes/sensor_array.def.toml", "defs/comptypes/thruster_main.def.toml",
        "defs/comptypes/thruster_rcs.def.toml", "defs/comptypes/turret_dorsal.def.toml",
        "samples/component_gizmos.glb", "samples/test_ship.glb"});

    // ---- decode DEF0 v2 exactly (layout pinned in cooked.h) ----
    ve::cook::Unit du;
    REQUIRE(ve::cook::read_unit_file(out + "/units/defs/test_ship.def.toml.ck", du, &err));
    CHECK(du.id == "defs/test_ship.def.toml");
    CHECK(du.type == ve::core::cooked::kUnitDef0);
    Reader s{du.payload};
    CHECK(s.u32() == 3u);  // payload version (v3: #52b art_ref + mount)
    CHECK(s.str() == "class.light_freighter");
    CHECK(s.u8() == 1); CHECK(s.f64() == 98000.0);           // hull_mass kg (#14)
    CHECK(s.u8() == 1);
    CHECK(s.f64() == 0.0); CHECK(s.f64() == 0.25); CHECK(s.f64() == 0.4);  // com m (#42)
    CHECK(s.u8() == 1);
    CHECK(s.f64() == 420000.0); CHECK(s.f64() == 980000.0); CHECK(s.f64() == 1120000.0);
    // grid (#44): unit strings coerced at cook time
    CHECK(s.f64() == 2.0);       // cell m
    CHECK(s.u32() == 4u);        // subcell_ratio
    CHECK(s.f64() == 2.2);       // cell_height m
    CHECK(s.f64() == 0.1);       // tolerance m
    // sections (#19a) — four tiling boxes, CELL coords, flat lo then hi
    CHECK(s.u32() == 4u);
    CHECK(s.str() == "bridge");
    CHECK(s.u32() == 1u);
    for (const int v : {-1, 0, -2, 1, 1, -2}) CHECK(s.i32() == v);
    CHECK(s.str() == "midship");
    CHECK(s.u32() == 1u);
    for (const int v : {-3, 0, -1, 3, 1, 0}) CHECK(s.i32() == v);
    CHECK(s.str() == "engine_bay");
    CHECK(s.u32() == 1u);
    for (const int v : {-3, 0, -3, 3, 1, -3}) CHECK(s.i32() == v);
    CHECK(s.str() == "engineering");
    CHECK(s.u32() == 1u);
    for (const int v : {-1, 0, 1, 1, 1, 3}) CHECK(s.i32() == v);
    // components: authored order, 11 rows (7 hp-mounted + 4 interior anchors
    // whose socket names pair 1:1 with the GLB's named empties, #46a)
    CHECK(s.u32() == 11u);
    auto expect_comp = [&](const std::string& id, const std::string& type, int cat,
                           const std::string& sock, int tags, int face = 0) {
        CHECK(s.str() == id);
        CHECK(s.str() == type);
        CHECK(static_cast<int>(s.u8()) == cat);
        CHECK(s.str() == sock);
        CHECK(static_cast<int>(s.u8()) == face);  // #46c1
        CHECK(s.u32() == static_cast<std::uint32_t>(tags));
        for (int t = 0; t < tags; ++t) (void)s.str();
    };
    auto skip_attr = [&](int n) {
        CHECK(s.u32() == static_cast<std::uint32_t>(n));
        for (int a = 0; a < n; ++a) { (void)s.str(); (void)s.f64(); }
    };
    auto skip_facet = [&](int n) {
        CHECK(s.u32() == static_cast<std::uint32_t>(n));
        for (int f = 0; f < n; ++f) {
            (void)s.str(); (void)s.f64(); (void)s.u8();
            CHECK(s.u8() == 0u);  // no target
        }
    };
    auto skip_art_mount = [&]() {  // #52b DEF0 v3 tail
        CHECK(s.str() == "samples/component_gizmos.glb");  // type-def art (#52a)
        CHECK(s.u8() == 1u);                        // every entry is socketed
        for (int i = 0; i < 7; ++i) (void)s.f64();  // anchor t[3] + q[4]
    };
    expect_comp("engine_L", "thruster.main", 0, "hp_engine_L", 1);
    {  // attrs in key order (std::map) — #42 coercion already applied;
       // thrust.gimbal_rad authored "15 deg" -> radians cooked (#46e)
        const std::vector<std::pair<std::string, double>> want = {
            {"mass", 900.0}, {"power.draw", 25000.0},
            {"thrust.force", 320000.0},
            {"thrust.gimbal_rad", 0.26179938779914945}, {"thrust.isp", 300.0}};
        CHECK(s.u32() == 5u);
        for (const auto& [k, v] : want) {
            CHECK(s.str() == k);
            CHECK(s.f64() == doctest::Approx(v).epsilon(1e-12));
        }
    }
    skip_facet(0);
    skip_art_mount();
    expect_comp("engine_R", "thruster.main", 0, "hp_engine_R", 1);
    skip_attr(5); skip_facet(0); skip_art_mount();
    expect_comp("rcs_wt", "thruster.rcs", 1, "hp_wingtip_R", 0);
    skip_attr(4); skip_facet(0); skip_art_mount();
    expect_comp("sensor_scan", "sensor.array", 3, "hp_wingtip_L", 0);
    skip_attr(4); skip_facet(0); skip_art_mount();  // sensor.range "120 km" -> 120000.0 m (#42)
    expect_comp("turret_dorsal", "turret.dorsal", 2, "hp_dorsal_turret", 0);
    skip_attr(2); skip_facet(1); skip_art_mount();
    expect_comp("seat_pilot", "seat.pilot", 3, "hp_seat_pilot", 1);
    skip_attr(0); skip_facet(1); skip_art_mount();  // use.seat 1.2 m (#28a)
    expect_comp("door_1", "door.hatch", 3, "door_1", 0, 5);  // face +z (#46c1)
    skip_attr(0); skip_facet(1); skip_art_mount();  // interior anchor, socket==name (#46a)
    expect_comp("panel_main", "panel.dash", 3, "panel_main", 0);
    skip_attr(0);
    CHECK(s.u32() == 2u);  // facets: use.panel plain...
    CHECK(s.str() == "use.panel"); CHECK(s.f64() == 1.2); CHECK(s.u8() == 0); CHECK(s.u8() == 0);
    {  // ...and repair.target naming a real wire id (#28a; existence gate = 5.9b)
        CHECK(s.str() == "repair.target"); CHECK(s.f64() == 1.2); CHECK(s.u8() == 0);
        CHECK(s.u8() == 1u); CHECK(s.str() == "w_engL");
    }
    skip_art_mount();
    expect_comp("berth_1", "berth.crew", 3, "berth_1", 0);
    skip_attr(0); skip_facet(1); skip_art_mount();
    expect_comp("reactor_1", "power.reactor", 3, "hp_reactor", 0);
    skip_attr(2); skip_facet(0); skip_art_mount();
    expect_comp("battery_1", "power.battery", 3, "hp_battery", 0);
    skip_attr(2); skip_facet(0); skip_art_mount();
    // catalog: type -> category, first-seen order
    CHECK(s.u32() == 10u);
    {
        const std::vector<std::pair<std::string, int>> want = {
            {"thruster.main", 0}, {"thruster.rcs", 1}, {"sensor.array", 3},
            {"turret.dorsal", 2}, {"seat.pilot", 3}, {"door.hatch", 3},
            {"panel.dash", 3}, {"berth.crew", 3}, {"power.reactor", 3},
            {"power.battery", 3}};
        for (const auto& [t, c] : want) { CHECK(s.str() == t); CHECK(static_cast<int>(s.u8()) == c); }
    }
    // wires (#26a)
    CHECK(s.u32() == 6u);
    auto expect_wire = [&](const std::string& id, int type, bool cap, double cap_v,
                           int fk, const std::string& f, int tk, const std::string& t) {
        CHECK(s.str() == id);
        CHECK(static_cast<int>(s.u8()) == type);
        CHECK(static_cast<int>(s.u8()) == (cap ? 1 : 0));
        if (cap) CHECK(s.f64() == cap_v);
        CHECK(static_cast<int>(s.u8()) == fk); CHECK(s.str() == f);
        CHECK(static_cast<int>(s.u8()) == tk); CHECK(s.str() == t);
    };
    expect_wire("w_engL", 0, false, 0.0, 1, "reactor_1", 0, "hp_engine_L");
    expect_wire("w_engR", 0, false, 0.0, 1, "reactor_1", 0, "hp_engine_R");
    expect_wire("w_rcs", 0, false, 0.0, 1, "battery_1", 0, "hp_wingtip_R");
    expect_wire("w_sensor", 0, false, 0.0, 1, "battery_1", 0, "hp_wingtip_L");
    expect_wire("w_turret", 0, true, 6000.0, 1, "reactor_1", 0, "hp_dorsal_turret");
    expect_wire("w_fire_ctrl", 2, false, 0.0, 1, "seat_pilot", 1, "turret_dorsal");
    // seams (#46d): parsed between wires and refs; sample has none (5.9b adds
    // a two-def seam gate). 5.8 payload had no seam field at all, so this
    // count is v2's new slot.
    CHECK(s.u32() == 0u);
    // refs ride the payload too (manifest deps resolve them separately)
    CHECK(s.u32() == 1u);
    CHECK(s.str() == "../samples/test_ship.glb");
    CHECK(s.at == du.payload.size());  // payload fully consumed: layout exact

    // golden: DEF0 payload bytes byte-exact pinned (static fixture).
    const auto golden = std::filesystem::path(VE_SOURCE_DIR) /
                        "tests" / "cook" / "fixtures" / "test_ship.def.golden";
    if (const char* upd = std::getenv("VE_GOLDEN_UPDATE"); upd && *upd) {
        ve::core::cooked::write_file_bytes(golden.string(), du.payload, &err);
        REQUIRE_MESSAGE(err.empty(), err);
    }
    ve::cook::Bytes gbytes;
    REQUIRE(ve::core::cooked::read_file_bytes(golden.string(), gbytes, &err));
    CHECK(du.payload == gbytes);
}

TEST_CASE("cook_hull_def_gates") {
    std::string err;
    auto cook = [&](const std::string& text, ve::cook::Bytes& payload,
                    std::vector<std::string>& refs) {
        return ve::cook::cook_hulldef_unit(text, payload, refs, &err);
    };

    // no class_tag -> raw passthrough (existing non-hull def behavior kept).
    {
        const std::string text = "refs = [\"a.toml\"]\nname = \"docking_manifest\"\n";
        ve::cook::Bytes payload;
        std::vector<std::string> refs;
        REQUIRE(cook(text, payload, refs));
        CHECK(std::string(payload.begin(), payload.end()) == text);
        CHECK(refs == std::vector<std::string>{"a.toml"});
    }

    // minimal valid hull def parses and emits.
    const std::string minimal =
        "class_tag = \"class.mini\"\n"
        "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n";
    {
        ve::cook::Bytes payload;
        std::vector<std::string> refs;
        REQUIRE_MESSAGE(cook(minimal, payload, refs), err);
        CHECK(payload.size() > 8u);
    }

    // #47b: ratified dock attrs + port.undock facet parse at cook time
    // (bare SI numbers per #42; inline fixture, zero golden drift).
    {
        ve::cook::Bytes payload;
        std::vector<std::string> refs;
        const std::string dock_def = minimal +
            "[[component]]\nid = \"dp\"\ntype = \"dock.port\"\ncategory = \"utility\"\nsocket = \"hp\"\n"
            "[component.attrs]\nmass = 800\ndock.clamp_strength = 500000\n"
            "[[component.facet]]\naction = \"port.undock\"\nrange = \"1.2 m\"\n";
        CHECK_MESSAGE(cook(dock_def, payload, refs), err);
    }

    // schema gates: each must fail with the reason in err.
    auto must_fail = [&](const std::string& text, const std::string& why) {
        ve::cook::Bytes payload;
        std::vector<std::string> refs;
        CHECK_FALSE(cook(text, payload, refs));
        const std::string m = why + " -> " + err;
        CHECK_MESSAGE(err.find(why) != std::string::npos, m);
    };
    must_fail("class_tag = \"hull.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n",
              "class.<name>");                                   // #31a tag grammar
    must_fail("class_tag = \"class.mini\"\n", "grid");           // lattice required (#44)
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 0\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n",
              "subcell_ratio");
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n",
              "bare number");                                    // bare number length (#42)
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[section]]\nname = \"a\"\ncells = [[1, 0, 0, 0, 0, 0]]\n",
              "lo > hi");                                        // box inversion
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[component]]\nid = \"x\"\ntype = \"thruster.main\"\ncategory = \"engine\"\nsocket = \"hp\"\n",
              "main/rcs/gun/utility");                           // category gate (#21)
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[component]]\nid = \"x\"\ntype = \"thruster.main\"\ncategory = \"main\"\n",
              "socket");                                         // socket key required (#14)
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[component]]\nid = \"x\"\ntype = \"t\"\ncategory = \"main\"\nsocket = \"hp\"\n"
              "[component.attrs]\nwarp.drive = 1\n",
              "registry");                                       // ratified attr keys only (5.4)
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[component]]\nid = \"x\"\ntype = \"dock.port\"\ncategory = \"utility\"\nsocket = \"hp\"\n"
              "[component.attrs]\ndock.clamp_strengths = 500000\n",
              "registry");                                       // #47b key must match exactly
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[component]]\nid = \"x\"\ntype = \"t\"\ncategory = \"main\"\nsocket = \"hp\"\n"
              "[component.attrs]\nmass = \"900 kg\"\n",
              "unknown suffix");                                 // kg is bare SI (#42 scope)
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[wire]]\nid = \"w\"\ntype = \"power\"\nfrom = \"reactor_1\"\nto = \"socket.hp\"\n",
              "socket.<id>");  // #26a endpoint grammar
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[component]]\nid = \"x\"\ntype = \"t\"\ncategory = \"main\"\nsocket = \"a\"\n"
              "[[component]]\nid = \"x\"\ntype = \"t2\"\ncategory = \"rcs\"\nsocket = \"b\"\n",
              "duplicate component id");
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[section]]\nname = \"a\"\ncells = [[0, 0, 0, 0, 0, 0]]\n"
              "[[section]]\nname = \"a\"\ncells = [[1, 1, 1, 1, 1, 1]]\n",
              "duplicate section");
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[component]]\nid = \"a\"\ntype = \"t\"\ncategory = \"main\"\nsocket = \"s1\"\n"
              "[[component]]\nid = \"b\"\ntype = \"t\"\ncategory = \"rcs\"\nsocket = \"s2\"\n",
              "conflicting categories");                          // catalog one-category-per-type

    // #28a: facet action tags outside the game-side registry fail the cook.
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[component]]\nid = \"a\"\ntype = \"t\"\ncategory = \"main\"\nsocket = \"s\"\n"
              "[[component.facet]]\naction = \"use.ladder\"\nrange = \"2 m\"\n",
              "facet-action registry");  // #28a
    // #46c1: face axis spelling is strict.
    must_fail("class_tag = \"class.mini\"\n"
              "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
              "[[component]]\nid = \"a\"\ntype = \"door.hatch\"\ncategory = \"utility\"\nsocket = \"s\"\n"
              "face = \"north\"\n",
              "face");

    // TOML array-table law: sub-headers land on the LAST [[component]] entry.
    {
        const std::string text =
            "class_tag = \"class.mini\"\n"
            "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
            "[[component]]\nid = \"a\"\ntype = \"t1\"\ncategory = \"main\"\nsocket = \"s1\"\n"
            "[[component]]\nid = \"b\"\ntype = \"t2\"\ncategory = \"utility\"\nsocket = \"s2\"\n"
            "[component.attrs]\nmass = 5\n"
            "[[component.facet]]\naction = \"use.panel\"\nrange = \"2 m\"\n";
        ve::cook::Bytes payload;
        std::vector<std::string> refs;
        REQUIRE_MESSAGE(cook(text, payload, refs), err);
        Reader s{payload};
        (void)s.u32(); (void)s.str();
        (void)s.u8(); (void)s.u8(); (void)s.u8();
        (void)s.f64(); (void)s.u32(); (void)s.f64(); (void)s.f64();
        (void)s.u32();  // 0 sections
        CHECK(s.u32() == 2u);
        CHECK(s.str() == "a"); (void)s.str(); (void)s.u8(); (void)s.str(); (void)s.u8();
        CHECK(s.u32() == 0u);   // a: no tags,
        CHECK(s.u32() == 0u);   // no attrs,
        CHECK(s.u32() == 0u);   // no facets
        CHECK(s.str().empty());  // #52b: no art map -> art_ref ''
        CHECK(s.u8() == 0u);     // has_mount 0
        CHECK(s.str() == "b"); (void)s.str(); (void)s.u8(); (void)s.str(); (void)s.u8();
        CHECK(s.u32() == 0u);   // tags
        CHECK(s.u32() == 1u);   // attrs landed on b
        CHECK(s.str() == "mass"); CHECK(s.f64() == 5.0);
        CHECK(s.u32() == 1u);   // facet landed on b
        CHECK(s.str() == "use.panel"); CHECK(s.f64() == 2.0); CHECK(s.u8() == 0); CHECK(s.u8() == 0);
        CHECK(s.str().empty());  // #52b: no art map on either entry
        CHECK(s.u8() == 0u);
    }
}

// test: cook_component_type_defs — 9.1 (#52a, decision #55): component TYPE
// def grammar (component_type marker + required category + refs + [attrs]
// preset, closed field set, marker exclusivity), hull-entry inheritance with
// key-by-key override, category cross-check, entry art `refs` override, and
// the driver end-to-end: type-def manifest edges, hull effective sha (preset
// edit re-cooks an untouched hull), raw-passthrough type units, duplicate
// component_type and marker-conflict cook-fails.
namespace {
bool has_bytes(const ve::cook::Bytes& b, const std::string& pat) {
    return std::search(b.begin(), b.end(), pat.begin(), pat.end()) != b.end();
}
bool has_f64(const ve::cook::Bytes& b, double v) {
    unsigned char pat[8];  // must be unsigned char: Bytes is u8, and a
                           // signed char would never match the >=0x80 bytes
    std::memcpy(pat, &v, 8);
    return std::search(b.begin(), b.end(), pat, pat + 8) != b.end();
}
constexpr const char* kTypeDefSrc =
    "component_type = \"thruster.mx\"\n"
    "category = \"main\"\n"
    // gizmo rides the preset art candidates (#52b): the keyed-model gate
    // resolves entries whose id/type names a node of a candidate .glb.
    "refs = [\"../../art/main_x.bin\", \"../../art/gizmo.glb\"]\n"
    "[attrs]\nmass = 900\npower.draw = 25000\n";
constexpr const char* kMiniHull =
    "class_tag = \"class.mini\"\n"
    "refs = [\"../art/gizmo.glb\"]\n"  // BEFORE [[component]]: TOML table law
    "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
    // section covers the gizmo anchor's cell (#44: exactly one section).
    "[[section]]\nname = \"bay\"\ncells = [[0, 0, 0, 0, 0, 0]]\n"
    "[[component]]\nid = \"eng1\"\ntype = \"thruster.mx\"\ncategory = \"main\"\nsocket = "
    "\"hp_engine_L\"\n";  // socket names the gizmo's anchor (#46a)

// Minimal one-hardpoint gizmo glb: a mesh-less node named hp_* with a
// ratified socket_type (#21) cooks to a SOCK unit with that anchor. The
// second node is named by the mini hull's component id: #52b's keyed
// model gate only resolves art whose node set carries entry ids/types
// (plain node, no socket_type extras = model key only, never an anchor).
ve::cook::Bytes make_gizmo_glb() {
    std::string j = R"({"asset":{"version":"2.0"},"nodes":[)"
                    R"({"name":"hp_engine_L","extras":{"socket_type":"main"},)"
                    R"("translation":[0,0,0]},)"
                    R"({"name":"eng1","translation":[0,0,0]}]})";
    while (j.size() % 4) j += ' ';
    ve::cook::Bytes g;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) g.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
    };
    g.insert(g.end(), {'g', 'l', 'T', 'F'});
    u32(2);
    u32(static_cast<std::uint32_t>(12 + 8 + j.size()));
    u32(static_cast<std::uint32_t>(j.size()));
    g.insert(g.end(), {'J', 'S', 'O', 'N'});
    g.insert(g.end(), j.begin(), j.end());
    return g;
}
}  // namespace

TEST_CASE("cook_component_type_defs") {
    std::string err;

    // ---- grammar: valid type def parses fully ----
    {
        ve::cook::CompTypeDef td;
        REQUIRE_MESSAGE(ve::cook::cook_comptype_source(kTypeDefSrc, td, &err), err);
        CHECK(td.type == "thruster.mx");
        CHECK(static_cast<int>(td.category) == 0);  // main
        CHECK(td.refs == std::vector<std::string>{"../../art/main_x.bin",
                                                "../../art/gizmo.glb"});
        REQUIRE(td.attrs.size() == 2u);
        CHECK(td.attrs[0].first == "mass"); CHECK(td.attrs[0].second == 900.0);
        CHECK(td.attrs[1].first == "power.draw"); CHECK(td.attrs[1].second == 25000.0);
    }
    // #55a: empty refs allowed until the 9.2 strict art gate.
    {
        ve::cook::CompTypeDef td;
        const std::string src = "component_type = \"t.x\"\ncategory = \"utility\"\n";
        REQUIRE_MESSAGE(ve::cook::cook_comptype_source(src, td, &err), err);
        CHECK(td.refs.empty());
        CHECK(td.attrs.empty());
    }
    // marker exclusivity (#55a): two of the three markers is cook-fail.
    {
        ve::cook::CompTypeDef td;
        CHECK_FALSE(ve::cook::cook_comptype_source(
            "class_tag = \"c\"\ncomponent_type = \"t.x\"\ncategory = \"main\"\n", td, &err));
        CHECK(err.find("both class_tag and component_type") != std::string::npos);
        CHECK_FALSE(ve::cook::cook_comptype_source(
            "world_class = \"w\"\ncomponent_type = \"t.x\"\ncategory = \"main\"\n", td, &err));
        CHECK(err.find("both world_class and component_type") != std::string::npos);
    }
    // required fields + closed grammar + #42 at this end.
    {
        ve::cook::CompTypeDef td;
        CHECK_FALSE(ve::cook::cook_comptype_source("component_type = \"t.x\"\n", td, &err));
        CHECK(err.find("category") != std::string::npos);
        CHECK_FALSE(ve::cook::cook_comptype_source(
            "component_type = \"t.x\"\ncategory = \"warp\"\n", td, &err));
        CHECK(err.find("not one of main/rcs/gun/utility") != std::string::npos);
        CHECK_FALSE(ve::cook::cook_comptype_source(
            "component_type = \"t.x\"\ncategory = \"main\"\nfoo = 1\n", td, &err));
        CHECK(err.find("unknown field 'foo'") != std::string::npos);
        CHECK_FALSE(ve::cook::cook_comptype_source(
            "component_type = \"t.x\"\ncategory = \"main\"\n[attrs]\nnot.a.key = 1\n", td, &err));
    }

    // ---- hull merge: preset applies, entry key wins, key-order emit ----
    ve::cook::CompTypeDef td;
    REQUIRE(ve::cook::cook_comptype_source(kTypeDefSrc, td, &err));
    const std::map<std::string, ve::cook::CompTypeDef> types{{td.type, td}};
    {
        const std::string hull = std::string(kMiniHull) +
            "[component.attrs]\nmass = 1000\n";  // overrides the preset mass
        ve::cook::Bytes payload;
        std::vector<std::string> refs, used, erefs;
        REQUIRE_MESSAGE(ve::cook::cook_hulldef_unit(hull, payload, refs, &err, &types,
                                                    &used, &erefs), err);
        CHECK(used == std::vector<std::string>{"thruster.mx"});
        CHECK(erefs.empty());  // entry has no own art refs (inherited art
                               // rides the type def's own manifest edges)
        Reader s{payload};
        (void)s.u32(); (void)s.str();
        (void)s.u8(); (void)s.u8(); (void)s.u8();
        (void)s.f64(); (void)s.u32(); (void)s.f64(); (void)s.f64();
        CHECK(s.u32() == 1u);  // 1 section (bay, one cell box)
        (void)s.str(); CHECK(s.u32() == 1u);
        for (int i = 0; i < 6; ++i) (void)s.i32();
        CHECK(s.u32() == 1u);
        CHECK(s.str() == "eng1"); (void)s.str(); (void)s.u8(); (void)s.str(); (void)s.u8();
        CHECK(s.u32() == 0u);   // tags
        CHECK(s.u32() == 2u);   // merged attrs: preset + entry, entry wins
        CHECK(s.str() == "mass"); CHECK(s.f64() == 1000.0);        // entry key wins
        CHECK(s.str() == "power.draw"); CHECK(s.f64() == 25000.0);  // preset fills in
        CHECK(s.u32() == 0u);      // facets
        CHECK(s.str().empty());    // #52b art_ref: no art map at unit level
        CHECK(s.u8() == 0u);       // has_mount: no anchor table at unit level
    }
    // #52b (DEF0 v3) unit level: the art map + anchor table fill the
    // per-component art_ref and the mount TRS of the entry's socket.
    {
        ve::cook::Bytes payload;
        std::vector<std::string> refs;
        const std::map<std::string, std::string> art{{"eng1", "art/gizmo.glb"}};
        ve::cook::MountTRS mt;
        mt.t = {1.0, 2.0, 3.0};
        mt.q = {0.0, 0.0, 0.7071067811865475, 0.7071067811865475};
        const std::map<std::string, ve::cook::MountTRS> anchors{{"hp_engine_L", mt}};
        REQUIRE_MESSAGE(ve::cook::cook_hulldef_unit(kMiniHull, payload, refs, &err, &types,
                                                    nullptr, nullptr, &art, &anchors), err);
        CHECK(has_bytes(payload, "art/gizmo.glb"));
        CHECK(has_f64(payload, 3.0));   // mount t
        CHECK(has_f64(payload, 0.7071067811865475));  // mount q
    }
    // category cross-check (#55c): entry category must equal the type's.
    {
        const std::string hull =
            "class_tag = \"class.mini\"\n"
            "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
            "[[component]]\nid = \"eng1\"\ntype = \"thruster.mx\"\ncategory = \"rcs\"\nsocket = \"s1\"\n";
        ve::cook::Bytes payload;
        std::vector<std::string> refs;
        CHECK_FALSE(ve::cook::cook_hulldef_unit(hull, payload, refs, &err, &types));
        CHECK(err.find("does not match component type 'thruster.mx' category 'main'") !=
              std::string::npos);
    }
    // entry art override (#55b): the entry's own refs are the hull dep edges.
    {
        const std::string hull =
            "class_tag = \"class.mini\"\n"
            "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
            "[[component]]\nid = \"eng1\"\ntype = \"thruster.mx\"\ncategory = \"main\"\nsocket = \"s1\"\n"
            "refs = [\"art/own_x.bin\"]\n";
        ve::cook::Bytes payload;
        std::vector<std::string> refs, used, erefs;
        REQUIRE_MESSAGE(ve::cook::cook_hulldef_unit(hull, payload, refs, &err, &types,
                                                    &used, &erefs), err);
        CHECK(erefs == std::vector<std::string>{"art/own_x.bin"});
    }
    // entry whose type has NO type def: entry-only attrs, no used/edges.
    {
        ve::cook::Bytes payload;
        std::vector<std::string> refs, used, erefs;
        const std::string hull =
            "class_tag = \"class.mini\"\n"
            "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
            "[[component]]\nid = \"solo\"\ntype = \"t.other\"\ncategory = \"utility\"\nsocket = \"s9\"\n"
            "[component.attrs]\nmass = 7\n";
        REQUIRE_MESSAGE(ve::cook::cook_hulldef_unit(hull, payload, refs, &err, &types,
                                                    &used, &erefs), err);
        CHECK(used.empty());
        CHECK(erefs.empty());
    }
}

TEST_CASE("cook_component_type_defs_driver") {
    // End-to-end through cook_incremental: edges, effective-sha recook,
    // raw-passthrough type units, duplicate + conflict cook-fails.
    const std::string src = tmp_root("ct_src");
    const std::string out = tmp_root("ct_out");
    auto write = [&](const std::string& rel, const std::string& text) {
        const auto p = std::filesystem::path(src) / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << text;
    };
    write("defs/parts/thruster_mx.def.toml", kTypeDefSrc);
    write("defs/mini_hull.def.toml", kMiniHull);
    write("art/main_x.bin", "ART-v1");
    // 5.9b doctrine: the hull's socket must name an anchor in a cooked SOCK
    // — reached via the hull's own .glb manifest dep (#46a).
    const auto gizmo = make_gizmo_glb();
    ve::core::cooked::write_file_bytes((std::filesystem::path(src) / "art/gizmo.glb").string(),
                                       gizmo, nullptr);

    ve::core::cooked::Manifest mf;
    std::string err;
    auto r1 = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(r1.ok, r1.error);
    CHECK(r1.counts.cooked == 4u);
    REQUIRE(ve::cook::read_manifest(out + "/manifest.bin", mf, &err));
    // hull -> type def edge (#52b); the hull inherits art transitively.
    CHECK(mf.at("defs/mini_hull.def.toml").deps ==
          std::vector<std::string>{"art/gizmo.glb", "defs/parts/thruster_mx.def.toml"});
    // type def -> its art refs (authored against the TYPE DEF's directory).
    CHECK(mf.at("defs/parts/thruster_mx.def.toml").deps ==
          std::vector<std::string>{"art/gizmo.glb", "art/main_x.bin"});
    // the type def unit keeps raw source bytes (#55: no new payload kind).
    ve::cook::Unit tu;
    REQUIRE(ve::cook::read_unit_file(out + "/units/defs/parts/thruster_mx.def.toml.ck", tu, &err));
    CHECK(std::string(tu.payload.begin(), tu.payload.end()) == kTypeDefSrc);
    // merged preset is IN the hull payload (preset attrs + preset mass 900).
    ve::cook::Unit hu;
    REQUIRE(ve::cook::read_unit_file(out + "/units/defs/mini_hull.def.toml.ck", hu, &err));
    CHECK(has_bytes(hu.payload, "power.draw"));
    CHECK(has_f64(hu.payload, 25000.0));
    CHECK(has_f64(hu.payload, 900.0));
    // #52b (DEF0 v3; gate G11 passes by construction): eng1 resolves to
    // the gizmo (its node set carries the component id), the content ID
    // rides the hull's manifest deps, and the payload names the art.
    CHECK(has_bytes(hu.payload, "art/gizmo.glb"));

    // untouched sources: everything skips (hull's effective sha unchanged).
    auto r2 = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(r2.ok, r2.error);
    CHECK(r2.counts.skipped == 4u);
    CHECK(r2.counts.cooked == 0u);

    // edit ONLY the type def preset: hull re-cooks (effective sha covers the
    // type def bytes) — a preset edit can never leave a stale hull payload.
    write("defs/parts/thruster_mx.def.toml",
          "component_type = \"thruster.mx\"\ncategory = \"main\"\n"
          "refs = [\"../../art/main_x.bin\", \"../../art/gizmo.glb\"]\n[attrs]\nmass = 1234\npower.draw = 25000\n");
    auto r3 = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(r3.ok, r3.error);
    CHECK(r3.counts.cooked == 2u);  // the type def AND the hull
    CHECK(r3.counts.skipped == 2u);
    REQUIRE(ve::cook::read_unit_file(out + "/units/defs/mini_hull.def.toml.ck", hu, &err));
    CHECK(has_f64(hu.payload, 1234.0));
    CHECK_FALSE(has_f64(hu.payload, 900.0));

    // duplicate component_type across sources: cook-fail (#55a).
    write("defs/parts/thruster_mx_copy.def.toml", kTypeDefSrc);
    auto r4 = ve::cook::cook_incremental(src, out);
    CHECK_FALSE(r4.ok);
    CHECK(r4.error.find("declared by both 'defs/parts/thruster_mx.def.toml' and") !=
          std::string::npos);
    std::filesystem::remove(std::filesystem::path(src) / "defs/parts/thruster_mx_copy.def.toml");

    // one def carrying class_tag AND component_type: cook-fail (#55a), and
    // routing sends it to the type-def gate (marker precedence is exclusive).
    write("defs/conflict.def.toml",
          "class_tag = \"class.x\"\ncomponent_type = \"t.conflict\"\ncategory = \"main\"\n");
    auto r5 = ve::cook::cook_incremental(src, out);
    CHECK_FALSE(r5.ok);
    CHECK(r5.error.find("both class_tag and component_type") != std::string::npos);

    std::filesystem::remove_all(src);
    std::filesystem::remove_all(out);
}
