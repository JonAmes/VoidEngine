// test: author_gizmos — 8.19 mode-A gizmo authoring (decision #43: the
// editor drags bodies/stations/waypoints/pads in the live cooked world;
// every edit writes back to the TOML source and re-runs the real gates).
//
// Contract pinned here (headless Tier 1; the GPU half of the pick is
// test_author_gizmo_pick):
//   * the whole slice loads through the real incremental cooker; handles
//     ride the assembled rail_world (positions at tick == rail eval);
//   * a drag edits ONLY the touched lines of the source def text (git
//     diff stays human-reviewable, #16 rule: no binary scene files), the
//     emitted unit-suffix literals re-parse to the drag target, and the
//     assembled rails hot-swap — the moved body's position follows NOW;
//   * validity gates refuse bad drops: cursor inside the parent's surface
//     fails, the source text AND the live rails revert bit-exact, and the
//     root anchor refuses entirely (#45c);
//   * waypoint drags edit only the [[waypoint]] row; poi drags re-normalize
//     the center direction on the body's unit sphere;
//   * pick: nearest handle to the cursor wins (nearer-d wins overlaps),
//     and the picked def selects through the 8.3 selection_service — the
//     same service the inspector/browser/loadout panels share.
#include <ve/cook/author_gizmos.h>

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <string>
#include <vector>
#include "../ve_test_portability.h"  // getpid

namespace ve::cook {
namespace {

// Fixture slice: identical texts to test_worlddef.cpp (the validated 8.7
// slice) — one tweak: luna's orbit_phase line carries a trailing comment,
// so the write-back can be pinned to preserve it.
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

[[stamp]]
type = "crater"
center = [0.0, 0.9, 0.4]
radius = "0.08 rad"
level = "-250 m"
blend = 0.3

[[poi]]
name = "kepler_pad"
kind = "pad"
stamp = "flatten"
center = [0.6, 0.5, 0.7]
radius = "0.02 rad"
level = "120 m"
footprint = "60 m"

[[biome]]
lo = "0 m"
hi = "400 m"
color = [0.24, 0.45, 0.22]
roughness = 0.85
scatter = 3.5
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

std::vector<std::string> lines_of(const std::string& t) {
    std::vector<std::string> v;
    std::string ln;
    std::istringstream f(t);
    while (std::getline(f, ln)) v.push_back(ln);
    return v;
}

struct AuthorTree {
    std::filesystem::path src, out;
    std::map<std::string, std::string> sources;
    AuthorTree() {
        static int n = 0;
        auto base = std::filesystem::temp_directory_path() /
                    ("ve_author_" + std::to_string(ve_test::pid()) + "_" + std::to_string(++n));
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
        // #52a: component art lives on the hull fixture's TYPE defs + gizmo
        sources["samples/component_gizmos.glb"] =
            read_all(std::string(VE_SOURCE_DIR) + "/assets/samples/component_gizmos.glb");
        for (const auto& e : std::filesystem::directory_iterator(
                 std::string(VE_SOURCE_DIR) + "/assets/defs/comptypes"))
            sources["defs/comptypes/" + e.path().filename().string()] =
                read_all(e.path().string());
    }
};

const char* kSys = "world/system.toml";
const char* kVes = "world/bodies/vesper.toml";
const char* kLun = "world/bodies/luna.toml";
const char* kKep = "world/stations/kepler.toml";

}  // namespace

TEST_CASE("author_gizmos") {
    AuthorTree t;
    author_world aw;
    std::string err;
    REQUIRE_MESSAGE(aw.load(t.sources, t.src.string(), t.out.string(), &err), err);

    // ---- load: rails assembled, root first, moon rides vesper -----------
    const auto vid = aw.body_id(kVes);
    const auto lid = aw.body_id(kLun);
    const auto kid = aw.body_id(kKep);
    REQUIRE(vid >= 0);
    REQUIRE(lid >= 0);
    REQUIRE(kid >= 0);
    const auto* lb = aw.rails().body(lid);
    REQUIRE(lb);
    CHECK(lb->parent == vid);
    const auto vp = aw.rails().position(vid, 0);
    REQUIRE(vp);
    CHECK(*vp == core::math::dvec3{0.0, 0.0, 0.0});  // static root anchor (#45c)
    const auto lp0 = aw.rails().position(lid, 0);
    REQUIRE(lp0);
    // 384400 km? no: 384000 km at 45 deg: (r cos, 0, -r sin), r = 3.84e8.
    const double r0 = 3.84e8, q = 0.25 * std::numbers::pi;
    CHECK(lp0->x == doctest::Approx(r0 * std::cos(q)).epsilon(1e-12));
    CHECK(lp0->z == doctest::Approx(-r0 * std::sin(q)).epsilon(1e-12));

    // ---- success: drag luna to the cursor; text delta is exactly 2 lines;
    // the trailing comment rides; rails hot-swap so position follows NOW.
    const std::string luna_before = aw.sources().at(kLun);
    const core::math::dvec3 target{0.0, 0.0, 4.5e8};
    REQUIRE_MESSAGE(aw.drag_body(kLun, target, 0, &err), err);
    const std::string& luna_after = aw.sources().at(kLun);
    const auto b = lines_of(luna_before), a = lines_of(luna_after);
    REQUIRE(b.size() == a.size());
    int changed = 0;
    for (std::size_t i = 0; i < b.size(); ++i)
        if (b[i] != a[i]) {
            ++changed;
            CHECK((a[i].find("orbit_radius = \"450000000.000000 m\"") != std::string::npos ||
                   a[i].find("orbit_phase = \"4.712388980 rad\"  # art pick") != std::string::npos));
        }
    CHECK(changed == 2);  // only the touched keys moved; layout rides
    // On-disk source == live source (the write-back really landed).
    CHECK(read_all(t.src / kLun) == luna_after);
    const auto lp1 = aw.rails().position(lid, 0);
    REQUIRE(lp1);
    // %.9f rad quantization bounds the cursor error to ~0.2 m at 4.5e8 m.
    CHECK(std::abs(lp1->x) < 1.0);
    CHECK(std::abs(lp1->y) < 1.0);
    CHECK(std::abs(lp1->z - 4.5e8) < 1.0);

    // ---- gate: cursor inside the parent's surface -> refused, NOTHING
    // written (text, disk, units and rails all snap back bit-exact).
    const std::string luna_ok = aw.sources().at(kLun);
    const auto pos_ok = *lp1;
    CHECK_FALSE(aw.drag_body(kLun, core::math::dvec3{1.0e6, 0.0, 0.0}, 0, &err));
    CHECK(err.find("inside parent surface radius") != std::string::npos);
    CHECK(aw.sources().at(kLun) == luna_ok);
    CHECK(read_all(t.src / kLun) == luna_ok);
    const auto lp2 = aw.rails().position(lid, 0);
    REQUIRE(lp2);
    CHECK(*lp2 == pos_ok);  // rails unchanged to the last bit

    // ---- gate: the root anchor never moves (#45c).
    CHECK_FALSE(aw.drag_body(kVes, core::math::dvec3{1.0e6, 0.0, 0.0}, 0, &err));
    CHECK(err.find("static root anchor") != std::string::npos);
    CHECK(aw.sources().at(kVes) == kVesper);

    // ---- waypoint drag: only the [[waypoint]] row's `at` line moves; the
    // re-decoded def carries the new triple (lane/patrol edges ride it).
    const std::string sys_before = aw.sources().at(kSys);
    REQUIRE_MESSAGE(aw.drag_waypoint(kSys, "wp_patrol_gate", core::math::dvec3{9.0e6, 2.0e6, -3.0e6}, &err), err);
    const std::string& sys_after = aw.sources().at(kSys);
    const auto sb = lines_of(sys_before), sa = lines_of(sys_after);
    REQUIRE(sb.size() == sa.size());
    int sys_changed = 0;
    for (std::size_t i = 0; i < sb.size(); ++i)
        if (sb[i] != sa[i]) {
            ++sys_changed;
            CHECK(sb[i] == "at = [7500000.0, 0.0, 0.0]");           // the `at` row line only
            CHECK(sa[i] == "at = [9000000.000000, 2000000.000000, -3000000.000000]");
        }
    CHECK(sys_changed == 1);
    const auto* sd = aw.def(kSys);
    REQUIRE(sd);
    REQUIRE(sd->waypoints.size() == 1);
    CHECK(sd->waypoints[0].offset[0] == 9.0e6);
    CHECK(sd->waypoints[0].offset[1] == 2.0e6);
    CHECK(sd->waypoints[0].offset[2] == -3.0e6);
    CHECK_FALSE(aw.drag_waypoint(kSys, "wp_ghost", core::math::dvec3{1.0, 2.0, 3.0}, &err));
    CHECK(err.find("no [[waypoint]]") != std::string::npos);
    CHECK(aw.sources().at(kSys) == sys_after);  // unknown id wrote nothing

    // ---- poi drag: center re-normalized onto the unit sphere, other poi
    // fields (radius/level/footprint) ride untouched.
    REQUIRE_MESSAGE(aw.drag_poi(kVes, "kepler_pad", core::math::dvec3{1.0, 1.0, 0.0}, &err), err);
    const auto* vd = aw.def(kVes);
    REQUIRE(vd);
    REQUIRE(vd->pois.size() == 1);
    const auto& cd = vd->pois[0].stamp.center_dir;
    // the write-back emits 6 decimals; the re-parsed value is the EMITTED
    // literal (0.707107), a hair off the true 1/sqrt(2) — the round-trip
    // contract is source text -> decode, not bit-exact normalization.
    CHECK(cd.x == doctest::Approx(0.707107).epsilon(1e-12));
    CHECK(cd.y == doctest::Approx(0.707107).epsilon(1e-12));
    CHECK(cd.z == doctest::Approx(0.0).epsilon(1e-9).scale(1.0));
    CHECK(vd->pois[0].footprint_m == 60.0);
    const auto ves_lines = lines_of(aw.sources().at(kVes));
    bool center_line = false;
    for (const auto& ln : ves_lines)
        if (ln.find("center = [0.707107, 0.707107, 0.000000]") != std::string::npos) center_line = true;
    CHECK(center_line);
    CHECK_FALSE(aw.drag_poi(kVes, "pad_ghost", core::math::dvec3{0.0, 0.0, 1.0}, &err));
    CHECK(err.find("no [[poi]]") != std::string::npos);

    // park the waypoint 100 km INSIDE the pad handle along the eye->pad
    // ray (vesper is the static root at the origin; after its 45-deg drag
    // the pad center is 6.2e6 * (0.707107, 0.707107, 0)): same projected
    // pixel, nearer depth than the poi — but the root body handle sits at
    // the pixel ORIGIN (0.17 px away), so the 0.1 px cursor radius decides
    // between poi/waypoint on d alone.
    constexpr double kPad61 = 4.3133513652e6;  // 6.1e6 / sqrt(2)
    REQUIRE_MESSAGE(aw.drag_waypoint(kSys, "wp_patrol_gate", core::math::dvec3{kPad61, kPad61, 0.0}, &err), err);

    // ---- handles: 3 bodies/stations + 1 poi + 1 waypoint at this tick.
    const auto hs = aw.handles(0);
    CHECK(hs.size() == 5);
    int kinds[3] = {0, 0, 0};
    for (const auto& hp : hs) ++kinds[static_cast<int>(hp.h.k)];
    CHECK(kinds[0] == 3);  // gizmo_handle::kind::body
    CHECK(kinds[1] == 1);  // waypoint
    CHECK(kinds[2] == 1);  // poi

    // ---- pick: same off-axis pinhole x/y map as the 8.10 pass (Tier-2
    // test_author_gizmo_pick pins CPU px == GPU pixel placement).
    gizmo_view view;
    view.eye = {0.0, -2.0e9, 0.0};
    view.f = {0.0, 1.0, 0.0};
    view.s = {1.0, 0.0, 0.0};
    view.u = {0.0, 0.0, 1.0};
    view.tan_l = view.tan_b = -0.4;
    view.tan_r = view.tan_t = 0.4;
    view.near_d = 1.0;
    view.far_d = 1.0e12;
    view.width_px = view.height_px = 64;

    // cursor ON luna's projected px -> luna handle.
    const auto lp = aw.rails().position(lid, 0);
    REQUIRE(lp);
    const auto lpx = project_px(*lp, view);
    REQUIRE(lpx);
    const auto hit = aw.pick_at(lpx->first, lpx->second, 2.0, view, 0);
    REQUIRE(hit);
    CHECK(hit->def_path == kLun);
    CHECK(hit->k == gizmo_handle::kind::body);

    // overlap: pad handle (static root + 6.2e6*(0.707107,0.707107,0)) and
    // waypoint (parked 100 km nearer on the same ray) project to the SAME
    // pixel; nearer-d wins the tie: waypoint beats the poi. The root
    // body's own handle projects to (32,32), outside the 0.1 px cursor.
    constexpr double kPad62 = 4.3840620434e6;  // 6.2e6 / sqrt(2)
    const auto pp = project_px(core::math::dvec3{kPad62, kPad62, 0.0}, view);
    REQUIRE(pp);
    const auto ovp = aw.pick_at(pp->first, pp->second, 0.1, view, 0);
    REQUIRE(ovp);
    CHECK(ovp->k == gizmo_handle::kind::waypoint);
    CHECK(ovp->row == "wp_patrol_gate");

    // miss: empty sky picks nothing.
    CHECK_FALSE(aw.pick_at(5.0, 5.0, 1.0, view, 0));

    // ---- selection service (8.3): the pick selects through the SAME
    // service the inspector/browser/loadout panels share.
    game::selection_service sel;
    std::map<std::string, core::entity_id> ents;
    ents[kLun] = core::entity_id{3, 7};
    ents[kVes] = core::entity_id{1, 0};
    aw.bind_selection(&sel, ents);
    const auto hit2 = aw.pick_at(lpx->first, lpx->second, 2.0, view, 0);
    REQUIRE(hit2);
    REQUIRE(sel.selected());
    CHECK(*sel.selected() == (core::entity_id{3, 7}));
}

// test: author_glue_rows — 8.20.2 mode-B glue write-back: add_row /
// set_row_field on the SYSTEM def, riding the same recook gate with
// bit-exact revert (checklist: good [[edge]] lands in source; dangling
// edge refused with ZERO bytes changed; lane flow edit).
TEST_CASE("author_glue_rows") {
    AuthorTree t;
    author_world aw;
    std::string err;
    REQUIRE_MESSAGE(aw.load(t.sources, t.src.string(), t.out.string(), &err), err);

    // ---- schematic_source side: nodes = station + waypoint + markets,
    // positions are the analytic rail evaluations at the tick.
    const auto nodes = aw.schematic_nodes(0);
    REQUIRE(nodes.size() == 4);
    CHECK(nodes[0].key == kKep);
    CHECK(nodes[0].station);
    const auto kid = aw.body_id(kKep);
    const auto kp = aw.rails().position(kid, 0);
    REQUIRE(kp);
    CHECK(nodes[0].pos == *kp);
    CHECK(nodes[1].key == "node:wp_patrol_gate");  // anchored to vesper at (0,0,0)
    CHECK(nodes[1].pos == core::math::dvec3{7500000.0, 0.0, 0.0});
    CHECK(nodes[2].key == "node:kepler_market");
    CHECK(nodes[2].pos == *kp);
    CHECK(nodes[3].key == "node:vesper_orbit_market");
    CHECK(nodes[3].pos == core::math::dvec3{0.0, 0.0, 0.0});
    REQUIRE(aw.sys_def());
    CHECK(aw.sys_def()->lanes.size() == 1);
    CHECK(aw.sys_def()->nav_edges.size() == 2);

    // ---- accepted add: the new [[edge]] block lands in the live source
    // text, on disk, and in the freshly cooked def.
    const std::string sys_before = aw.sources().at(kSys);
    REQUIRE_MESSAGE(aw.add_glue_row(
        "edge", {{"kind", "\"lane\""}, {"from", "\"wp_patrol_gate\""},
                 {"to", "\"vesper_orbit_market\""}}, &err), err);
    const std::string& sys_after = aw.sources().at(kSys);
    CHECK(sys_after.rfind("[[edge]]\nkind = \"lane\"\nfrom = \"wp_patrol_gate\"\n"
                          "to = \"vesper_orbit_market\"") != std::string::npos);
    CHECK(read_all(t.src / kSys) == sys_after);
    CHECK(aw.sys_def()->nav_edges.size() == 3);
    const auto b0 = lines_of(sys_before), a0 = lines_of(sys_after);
    bool prefix_same = a0.size() > b0.size();
    for (std::size_t i = 0; i < b0.size() && prefix_same; ++i)
        if (b0[i] != a0[i]) prefix_same = false;
    CHECK(prefix_same);  // append-only: existing lines untouched

    // ---- refused add: dangling endpoint -> the 8.8 gate fails the recook;
    // live text, disk text and cooked def all stay BIT-EXACT, rails live.
    const std::string sys_ok = aw.sources().at(kSys);
    CHECK_FALSE(aw.add_glue_row(
        "edge", {{"kind", "\"lane\""}, {"from", "\"wp_patrol_gate\""},
                 {"to", "\"ghost_node\""}}, &err));
    CHECK(!err.empty());
    CHECK(aw.sources().at(kSys) == sys_ok);
    CHECK(read_all(t.src / kSys) == sys_ok);
    CHECK(aw.sys_def()->nav_edges.size() == 3);
    CHECK(aw.rails().position(kid, 0).has_value());  // revert re-cooked the old tree

    // ---- glue-field edit: [[lane]] flow 1.75 -> 2.25 rides the same gate;
    // exactly one source line changes.
    REQUIRE_MESSAGE(aw.set_glue_field("lane", "id", "lane_ore_kepler_vesper",
                                      "flow", "2.25", &err), err);
    CHECK(aw.sources().at(kSys).find("flow = 2.25") != std::string::npos);
    CHECK(aw.sys_def()->lanes[0].flow_rate == doctest::Approx(2.25));
    const auto a1 = lines_of(aw.sources().at(kSys));
    REQUIRE(a1.size() == a0.size());
    int changed = 0;
    for (std::size_t i = 0; i < a1.size(); ++i)
        if (a1[i] != a0[i]) ++changed;
    CHECK(changed == 1);

    // refused field edit (no such row): zero bytes changed
    const std::string sys_ok2 = aw.sources().at(kSys);
    CHECK_FALSE(aw.set_glue_field("lane", "id", "lane_ghost", "flow", "9.0", &err));
    CHECK(!err.empty());
    CHECK(aw.sources().at(kSys) == sys_ok2);
}

TEST_CASE("author_world load keeps defs the sources map does not hold") {
    // ve_edit --src shape: every def is on disk in the tree; the forms map
    // holds only the system def (panels open other defs on demand). The old
    // semantics (wipe src_dir, rewrite only held sources) deleted the member
    // defs and the gate re-cook died with "references missing".
    AuthorTree t;
    std::filesystem::create_directories(t.src);
    for (const auto& [path, text] : t.sources) {
        const auto p = t.src / path;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f << text;
        REQUIRE(f.good());
    }
    std::map<std::string, std::string> partial;
    partial[kSys] = t.sources[kSys];

    author_world aw;
    std::string err;
    REQUIRE_MESSAGE(aw.load(partial, t.src.string(), t.out.string(), &err), err);

    // member defs cooked off disk: full rail chain assembled...
    const auto vid = aw.body_id(kVes);
    const auto lid = aw.body_id(kLun);
    CHECK(vid >= 0);
    CHECK(lid >= 0);
    CHECK(aw.body_id(kKep) >= 0);
    const auto* lb = aw.rails().body(lid);
    REQUIRE(lb);
    CHECK(lb->parent == vid);
    // ...and every tree file is still byte-exact on disk.
    for (const auto& [path, text] : t.sources)
        CHECK(read_all((t.src / path).string()) == text);
}

}  // namespace ve::cook
