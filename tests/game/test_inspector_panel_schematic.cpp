// test: inspector_panel_schematic — 8.20 authoring suite mode B (decision
// #43: the 2D schematic is a different PROJECTION of the same world def).
//
// Contract pinned here:
//   * the panel edits the REAL cooklet author_world (schematic_source):
//     nodes = stations + waypoints + market nodes from the cooked def,
//     positions the analytic rail evaluations (tick 0 = map of the def);
//   * projection: ref rule = selected node, else first station, else system
//     origin; top-down x/east, -z/north px; max-range node lands on the
//     kSchemRadiusPx ring; co-located nodes ring deterministically (never
//     stacked invisibly);
//   * edges = [[edge]] nav rows + [[lane]] rows keyed by "node:<id>"; lanes
//     carry commodity + flow; side-field readout rows for lanes, spawns and
//     faction stances (the #39/#25b glue a map cannot project);
//   * click = rising mouse_down edge: node marker under the cursor selects
//     through the SHARED 8.3 selection_service (def entities when bound;
//     waypoints have no entity -> selection clears, never goes stale);
//     empty space clears;
//   * edits ride the real incremental cooker: schematic_add_edge appends a
//     [[edge]] row (accepted -> live text + cooked def grow; dangling
//     endpoint -> wrldvalidate refuses, source bit-exact unchanged);
//     schematic_set_flow edits one [[lane]] literal.
// Rendering itself (line/quad emission) is pinned separately in the gfx
// suite (8.20.0 ui_line_primitive); this test owns the queryable model.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/cook/author_gizmos.h>
#include <ve/core/ecs.h>
#include <ve/game/inspector_panel.h>
#include <ve/gfx/ui.h>

#include "../ve_test_portability.h"  // getpid

#ifndef VE_SOURCE_DIR
#define VE_SOURCE_DIR "."
#endif

namespace ve::game {
namespace {

// ---- fixture: the validated 8.7/8.19 slice (texts shared with
// test_author_gizmos.cpp) + one [[spawn]] row so the spawn side-field row
// has something to project.
constexpr const char* kSystem = R"toml(
world_class = "system"
name = "Vesper"
seed = 12345
members = ["bodies/vesper.toml", "bodies/luna.toml", "stations/kepler.toml"]

[[faction]]
name = "vesper_authority"
power = 12.5
stance = ["stance.hostile.pirate_confederacy"]

[[faction]]
name = "pirate_confederacy"
power = 3.0

[[market]]
id = "kepler_market"
anchor = "stations/kepler.toml"

[[market]]
id = "vesper_orbit_market"
anchor = "bodies/vesper.toml"

[[production]]
market = "kepler_market"
commodity = "refined_ore"
rate = 2.5

[[production]]
market = "vesper_orbit_market"
commodity = "food"
rate = 7.0

[[lane]]
id = "lane_ore_kepler_vesper"
from = "kepler_market"
to = "vesper_orbit_market"
commodity = "refined_ore"
flow = 1.75

[[waypoint]]
id = "wp_patrol_gate"
kind = "patrol"
body = "bodies/vesper.toml"
at = [7500000.0, 0.0, 0.0]

[[edge]]
kind = "lane"
from = "kepler_market"
to = "wp_patrol_gate"

[[edge]]
kind = "patrol"
from = "wp_patrol_gate"
to = "vesper_orbit_market"

[[spawn]]
id = "pirate_patrol_lanes"
at = "wp_patrol_gate"
faction = "pirate_confederacy"
class = "fighter"
rate = 0.0003

[[job]]
id = "opening_cargo_run"

[[job_step]]
job = "opening_cargo_run"
verb = "undock"

[[job_step]]
job = "opening_cargo_run"
verb = "cargo_run"
target = "lane_ore_kepler_vesper"
amount = 2500.0

[[job_step]]
job = "opening_cargo_run"
verb = "dock"
target = "vesper_orbit_market"
)toml";

constexpr const char* kVesper = R"toml(
world_class = "body"
name = "Vesper Prime"
radius = "6200 km"
well_radius = "9000 km"
surface_g = 9.4
well_taper_start = "8000 km"
atm_rho0 = 1.18
atm_scale_height = "11 km"
spin_axis = [0.0, 1.0, 0.0]
spin_rate = "15 deg/day"
tags = ["owner.vesper_authority"]

[terrain]
seed = 777
relief = "1400 m"
base_frequency = 0.7
octaves = 5
lacunarity = 2.0
gain = 0.5
)toml";

constexpr const char* kLuna = R"toml(
world_class = "body"
name = "Luna Minor"
radius = "900 km"
well_radius = "1500 km"
surface_g = 1.6
well_taper_start = "1200 km"
parent = "../bodies/vesper.toml"
orbit_period = "27 day"
orbit_radius = "384000 km"
orbit_phase = "45 deg"  # art pick
spin_rate = "6.8 deg/day"
)toml";

constexpr const char* kKepler = R"toml(
world_class = "station"
name = "Kepler Station"
hull_def = "../../defs/station_core.def.toml"
parent = "../bodies/vesper.toml"
orbit_period = "3.2 day"
orbit_radius = "7000 km"
root_socket = "hp_engine_L"

[[build_step]]
plan = "expansion_a"
module = "../../defs/ring_module.def.toml"
socket = "hp_engine_R"
at = "0.5 day"
)toml";

constexpr const char* kSeamsStation = R"toml(

[[seam]]
partner = "ring_module.def.toml"
socket = "hp_engine_R"

[[seam]]
partner = "ring_module_b.def.toml"
socket = "hp_engine_L"
)toml";

constexpr const char* kSeamsRing = R"toml(

[[seam]]
partner = "station_core.def.toml"
socket = "hp_engine_R"
)toml";

constexpr const char* kSeamsRingB = R"toml(

[[seam]]
partner = "station_core.def.toml"
socket = "hp_engine_L"
)toml";

std::string read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct SchTree {
    std::filesystem::path src, out;
    std::map<std::string, std::string> sources;
    SchTree() {
        static int n = 0;
        auto base = std::filesystem::temp_directory_path() /
                    ("ve_schem_" + std::to_string(ve_test::pid()) + "_" + std::to_string(++n));
        src = base / "src";
        out = base / "out";
        std::filesystem::remove_all(base);
        const std::string ship = read_all(std::string(VE_SOURCE_DIR) + "/assets/defs/test_ship.def.toml");
        REQUIRE(!ship.empty());
        sources["world/system.toml"] = kSystem;
        sources["world/bodies/vesper.toml"] = kVesper;
        sources["world/bodies/luna.toml"] = kLuna;
        sources["world/stations/kepler.toml"] = kKepler;
        sources["defs/station_core.def.toml"] = ship + kSeamsStation;
        sources["defs/ring_module.def.toml"] = ship + kSeamsRing;
        sources["defs/ring_module_b.def.toml"] = ship + kSeamsRingB;
        sources["samples/test_ship.glb"] = read_all(std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb");
        REQUIRE(!sources["samples/test_ship.glb"].empty());
    }
};

const char* kSys = "world/system.toml";
const char* kKep = "world/stations/kepler.toml";

gfx::ui_io mkio(float x, float y, bool down) {
    gfx::ui_io io;
    io.mouse_x = x;
    io.mouse_y = y;
    io.mouse_down = down;
    return io;
}

const inspector_panel::schem_view_node* find_node(
        const std::vector<inspector_panel::schem_view_node>& v, std::string_view key) {
    const auto it = std::ranges::find(v, key, &inspector_panel::schem_view_node::key);
    return it == v.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("inspector_panel_schematic") {
    SchTree t;
    cook::author_world aw;
    std::string err;
    REQUIRE_MESSAGE(aw.load(t.sources, t.src.string(), t.out.string(), &err), err);

    selection_service sel;
    aw.bind_selection(&sel, {{kKep, core::entity_id{3, 7}},
                             {"world/bodies/vesper.toml", core::entity_id{5, 1}}});

    inspector_panel panel(sel);
    CHECK_FALSE(panel.schematic_active());
    panel.begin_schematic(&aw);
    CHECK(panel.schematic_active());

    // ---- nodes: station, waypoint, markets (that order); rail positions.
    const auto nodes = panel.schematic_nodes();
    REQUIRE(nodes.size() == 4);
    CHECK(nodes[0].key == kKep);
    CHECK(nodes[0].station);
    CHECK(nodes[1].key == "node:wp_patrol_gate");
    CHECK(nodes[2].key == "node:kepler_market");
    CHECK(nodes[3].key == "node:vesper_orbit_market");

    // ---- projection: no selection -> first station (kepler) centers the
    // map. Vesper (and its orbit market) sit 7000 km -x; scale puts the
    // max-range node on the 400 px ring. kepler_market is CO-LOCATED with
    // the center station -> ringed at (0, -30), never stacked on it.
    const auto v = panel.schematic_view();
    CHECK(v.center_key == kKep);
    CHECK(v.scale == doctest::Approx(400.0 / 7.0e6));
    const auto* kep = find_node(v.nodes, kKep);
    const auto* wpx = find_node(v.nodes, "node:wp_patrol_gate");
    const auto* kmx = find_node(v.nodes, "node:kepler_market");
    const auto* vmx = find_node(v.nodes, "node:vesper_orbit_market");
    REQUIRE(kep); REQUIRE(wpx); REQUIRE(kmx); REQUIRE(vmx);
    CHECK(kep->x == 0.0f); CHECK(kep->y == 0.0f);
    CHECK(vmx->x == doctest::Approx(-400.0f)); CHECK(vmx->y == doctest::Approx(0.0f));
    // wp is anchored on vesper: (7.5e6,0,0) - kepler (7e6,0,0) = +0.5e6 m.
    CHECK(wpx->x == doctest::Approx(0.5e6 * (400.0 / 7.0e6)));
    CHECK(wpx->y == doctest::Approx(0.0f));
    CHECK(kmx->x == doctest::Approx(0.0f));
    CHECK(kmx->y == doctest::Approx(-30.0f));  // first ring slot (-pi/2)

    // ---- click select: rising edge, marker radius, shared service.
    panel.click_schematic(mkio(0.0f, 0.0f, true));
    CHECK(panel.schematic_selected() == kKep);
    REQUIRE(sel.selected()); CHECK(*sel.selected() == (core::entity_id{3, 7}));
    // held (no rising edge): unchanged
    panel.click_schematic(mkio(600.0f, 600.0f, true));
    CHECK(panel.schematic_selected() == kKep);

    // waypoint node: no entity behind it -> service clears, never stale.
    panel.click_schematic(mkio(300.0f, 300.0f, false));  // release
    panel.click_schematic(mkio(float(wpx->x) + 5.0f, wpx->y, true));
    CHECK(panel.schematic_selected() == "node:wp_patrol_gate");
    CHECK_FALSE(sel.selected().has_value());

    // empty space clears.
    panel.click_schematic(mkio(0.0f, 0.0f, false));
    panel.click_schematic(mkio(500.0f, 500.0f, true));
    CHECK(panel.schematic_selected().empty());

    // selecting a node re-centers the map on it (#43 bearing view).
    panel.click_schematic(mkio(0.0f, 0.0f, false));
    panel.click_schematic(mkio(vmx->x, vmx->y, true));
    CHECK(panel.schematic_selected() == "node:vesper_orbit_market");
    const auto v2 = panel.schematic_view();
    CHECK(v2.center_key == "node:vesper_orbit_market");
    CHECK(find_node(v2.nodes, "node:vesper_orbit_market")->x == 0.0f);
    CHECK(find_node(v2.nodes, "node:vesper_orbit_market")->y == 0.0f);

    // ---- edges: 2 nav rows then the trade lane (side fields attached).
    auto edges = panel.schematic_edges();
    REQUIRE(edges.size() == 3);
    CHECK(edges[0].from == "node:kepler_market");
    CHECK(edges[0].to == "node:wp_patrol_gate");
    CHECK(edges[0].kind == "lane");
    CHECK(edges[1].kind == "patrol");
    CHECK(edges[2].lane_id == "lane_ore_kepler_vesper");
    CHECK(edges[2].commodity == "refined_ore");
    CHECK(edges[2].flow == doctest::Approx(1.75));

    // ---- side fields: lane flow, spawn table, faction stance (#39/#25b).
    const auto sf = panel.schematic_side_fields();
    REQUIRE(sf.size() == 3);
    CHECK(sf[0] == "lane lane_ore_kepler_vesper refined_ore flow 1.75");
    CHECK(sf[1] == "spawn pirate_patrol_lanes wp_patrol_gate pirate_confederacy fighter rate 0.0003");
    CHECK(sf[2] == "stance vesper_authority hostile pirate_confederacy");

    // ---- edits ride the REAL incremental cooker (8.20.2 glue):
    const std::string sys_ok = aw.sources().at(kSys);
    REQUIRE_MESSAGE(panel.schematic_add_edge("wp_patrol_gate", "vesper_orbit_market", &err), err);
    const std::string& sys_after = aw.sources().at(kSys);
    CHECK(sys_after.rfind("[[edge]]\nkind = \"lane\"\nfrom = \"wp_patrol_gate\"\n"
                          "to = \"vesper_orbit_market\"") != std::string::npos);
    edges = panel.schematic_edges();
    CHECK(edges.size() == 4);

    // refused: dangling endpoint -> wrldvalidate gate fails the recook;
    // live source bytes are BIT-EXACT and the map never saw the row.
    CHECK_FALSE(panel.schematic_add_edge("wp_patrol_gate", "ghost_node", &err));
    CHECK(!err.empty());
    CHECK(aw.sources().at(kSys) == sys_after);
    edges = panel.schematic_edges();
    CHECK(edges.size() == 4);

    // flow edit through the panel -> one [[lane]] literal changes.
    REQUIRE_MESSAGE(panel.schematic_set_flow("lane_ore_kepler_vesper", 2.25, &err), err);
    CHECK(aw.sources().at(kSys).find("flow = 2.25") != std::string::npos);
    edges = panel.schematic_edges();
    CHECK(edges[3].flow == doctest::Approx(2.25));

    // leaving schematic mode: the tree view is the same panel object.
    panel.end_schematic();
    CHECK_FALSE(panel.schematic_active());
    CHECK(panel.schematic_nodes().empty());
    CHECK(panel.schematic_view().nodes.empty());
}

}  // namespace ve::game
