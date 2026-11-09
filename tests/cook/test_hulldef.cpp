// test: cook_hull_def — 5.8: the sample hull def (assets/defs) cooks with
// the glb fixture into the normalized DEF0 v2 payload (grid, sections,
// components + attrs + facets, catalog, wires, refs; #42 SI coercion), the
// manifest records the def -> glb dependency edge, and payload bytes are
// golden-pinned. test: cook_hull_def_gates — schema failures are loud:
// bad class_tag, missing grid, unknown category, attr keys outside the
// ratified registry (#42 bare/unknown suffixes), bad wire endpoints, dup
// ids, bad section boxes; defs WITHOUT class_tag stay raw passthrough.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
    // Stage the two real fixtures under one src tree so the def's
    // ../samples/test_ship.glb ref resolves into the cook set.
    const std::string src = tmp_root("src");
    std::filesystem::create_directories(std::filesystem::path(src) / "samples");
    std::filesystem::create_directories(std::filesystem::path(src) / "defs");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb",
                          std::filesystem::path(src) / "samples" / "test_ship.glb");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/defs/test_ship.def.toml",
                          std::filesystem::path(src) / "defs" / "test_ship.def.toml");

    const std::string out = tmp_root("out");
    auto res = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(res.ok, res.error);
    CHECK(res.counts.failures == 0);

    // manifest: the def gained a dep edge onto the glb content (#14 coupling).
    ve::core::cooked::Manifest mf;
    std::string err;
    REQUIRE(ve::cook::read_manifest(out + "/manifest.bin", mf, &err));
    REQUIRE(mf.count("defs/test_ship.def.toml") == 1u);
    CHECK(mf.at("defs/test_ship.def.toml").deps == std::vector<std::string>{"samples/test_ship.glb"});

    // ---- decode DEF0 v2 exactly (layout pinned in cooked.h) ----
    ve::cook::Unit du;
    REQUIRE(ve::cook::read_unit_file(out + "/units/defs/test_ship.def.toml.ck", du, &err));
    CHECK(du.id == "defs/test_ship.def.toml");
    CHECK(du.type == ve::core::cooked::kUnitDef0);
    Reader s{du.payload};
    CHECK(s.u32() == 2u);  // payload version (v2: facet targets + seams)
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
                           const std::string& sock, int tags) {
        CHECK(s.str() == id);
        CHECK(s.str() == type);
        CHECK(static_cast<int>(s.u8()) == cat);
        CHECK(s.str() == sock);
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
    expect_comp("engine_R", "thruster.main", 0, "hp_engine_R", 1);
    skip_attr(5); skip_facet(0);
    expect_comp("rcs_wt", "thruster.rcs", 1, "hp_wingtip_R", 0);
    skip_attr(4); skip_facet(0);
    expect_comp("sensor_scan", "sensor.array", 3, "hp_wingtip_L", 0);
    skip_attr(4); skip_facet(0);  // sensor.range "120 km" -> 120000.0 m (#42)
    expect_comp("turret_dorsal", "turret.dorsal", 2, "hp_dorsal_turret", 0);
    skip_attr(2); skip_facet(1);
    expect_comp("seat_pilot", "seat.pilot", 3, "hp_seat_pilot", 1);
    skip_attr(0); skip_facet(1);  // use.seat 1.2 m (#28a)
    expect_comp("door_1", "door.hatch", 3, "door_1", 0);
    skip_attr(0); skip_facet(1);  // interior anchor, socket==name (#46a)
    expect_comp("panel_main", "panel.dash", 3, "panel_main", 0);
    skip_attr(0);
    CHECK(s.u32() == 2u);  // facets: use.panel plain...
    CHECK(s.str() == "use.panel"); CHECK(s.f64() == 1.2); CHECK(s.u8() == 0); CHECK(s.u8() == 0);
    {  // ...and repair.target naming a real wire id (#28a; existence gate = 5.9b)
        CHECK(s.str() == "repair.target"); CHECK(s.f64() == 1.2); CHECK(s.u8() == 0);
        CHECK(s.u8() == 1u); CHECK(s.str() == "w_engL");
    }
    expect_comp("berth_1", "berth.crew", 3, "berth_1", 0);
    skip_attr(0); skip_facet(1);
    expect_comp("reactor_1", "power.reactor", 3, "hp_reactor", 0);
    skip_attr(2); skip_facet(0);
    expect_comp("battery_1", "power.battery", 3, "hp_battery", 0);
    skip_attr(2); skip_facet(0);
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

    // TOML array-table law: sub-headers land on the LAST [[component]] entry.
    {
        const std::string text =
            "class_tag = \"class.mini\"\n"
            "[grid]\ncell = \"1 m\"\nsubcell_ratio = 2\ncell_height = \"1 m\"\ntolerance = \"0 m\"\n"
            "[[component]]\nid = \"a\"\ntype = \"t1\"\ncategory = \"main\"\nsocket = \"s1\"\n"
            "[[component]]\nid = \"b\"\ntype = \"t2\"\ncategory = \"utility\"\nsocket = \"s2\"\n"
            "[component.attrs]\nmass = 5\n"
            "[[component.facet]]\naction = \"use.x\"\nrange = \"2 m\"\n";
        ve::cook::Bytes payload;
        std::vector<std::string> refs;
        REQUIRE_MESSAGE(cook(text, payload, refs), err);
        Reader s{payload};
        (void)s.u32(); (void)s.str();
        (void)s.u8(); (void)s.u8(); (void)s.u8();
        (void)s.f64(); (void)s.u32(); (void)s.f64(); (void)s.f64();
        (void)s.u32();  // 0 sections
        CHECK(s.u32() == 2u);
        CHECK(s.str() == "a"); (void)s.str(); (void)s.u8(); (void)s.str();
        CHECK(s.u32() == 0u);   // a: no tags,
        CHECK(s.u32() == 0u);   // no attrs,
        CHECK(s.u32() == 0u);   // no facets
        CHECK(s.str() == "b"); (void)s.str(); (void)s.u8(); (void)s.str();
        CHECK(s.u32() == 0u);   // tags
        CHECK(s.u32() == 1u);   // attrs landed on b
        CHECK(s.str() == "mass"); CHECK(s.f64() == 5.0);
        CHECK(s.u32() == 1u);   // facet landed on b
        CHECK(s.str() == "use.x"); CHECK(s.f64() == 2.0); CHECK(s.u8() == 0);
    }
}
