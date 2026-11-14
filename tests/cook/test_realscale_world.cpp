// Phase 8 task 8.11 — real-scale slice system def authored + cooked (#17c,
// #22a, #20a, #24a, #25d). Sable Reach: two Earth/Mars-class planets at a
// REAL 1.52 AU separation, an airless Earth-Moon-class moon nested inside
// the root planet's well (the #17b testbed at true scale), a rail-pinned
// station in low orbit (#24a), and an outpost POI hosting the second market
// node (#20a). #22a: real radii and AU separations, but wells stay authored
// multiples of body radius (100x here, inside the 10..100 band).
//
// The rails half of this test is the checklist's "rails eval fp64-checked
// at AU scale": every pinned value was hand-computed in an exact op-order
// port of core/src/rails.cpp (Python, glibc libm, same expression grouping
// as the cook's unit table and quat.h's left-to-right Hamilton product).
// The cooked slice is the fixture for 8.12-8.16 (FTL, bubbles, time
// compression, persistence) and 8.21 (station build plans at runtime).
#include <ve/cook/cook.h>
#include <ve/cook/worlddef.h>
#include <ve/core/cooked.h>
#include <ve/core/math/base.h>
#include <ve/core/sim/rails.h>
#include <ve/core/sim/terrain.h>
#include <ve/core/sim/time.h>
#include <ve/game/units.h>
#include <ve/game/world_load.h>

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>
#include "../ve_test_portability.h"  // getpid

#ifndef VE_SOURCE_DIR
#error "VE_SOURCE_DIR must be defined by CMake"
#endif

using namespace ve;
using ve::game::WorldClass;
using ve::game::WorldDef;
using ve::core::sim::rail_world;
using ve::core::sim::body_def;
using ve::core::sim::tick_t;

namespace {

struct Tree {
    std::filesystem::path src, out;
    Tree() {
        static int n = 0;
        auto base = std::filesystem::temp_directory_path() /
                    ("ve_realscale_" + std::to_string(ve_test::pid()) + "_" + std::to_string(++n));
        src = base / "src";
        out = base / "out";
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(src / "world/bodies");
        std::filesystem::create_directories(src / "world/stations");
        std::filesystem::create_directories(src / "defs");
        std::filesystem::create_directories(src / "samples");
        std::filesystem::copy_file(std::string(VE_SOURCE_DIR) + "/assets/defs/test_ship.def.toml", src / "defs/test_ship.def.toml");
        std::filesystem::copy_file(std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb", src / "samples/test_ship.glb");
        std::ifstream sf(src / "defs/test_ship.def.toml");
        ship_text = std::string((std::istreambuf_iterator<char>(sf)),
                                std::istreambuf_iterator<char>());
        REQUIRE(!ship_text.empty());
    }
    std::string ship_text;  // base text for the station/ring hull defs
    void write(const std::string& rel, const std::string& text) const {
        std::ofstream f(src / rel);
        f << text;
        REQUIRE(f.good());
    }
};

// ---- the real-scale slice (#22a) ----
// Rail fixture has no star: Ferrous rides a WIDE kinematic orbit about
// Thalassa (#17a rails are kinematic, not Keplerian) so the two planets get
// a real 1.52 AU separation while one static root anchors the system.
constexpr const char* kSystem = R"toml(
world_class = "system"
name = "Sable Reach"
seed = 987654321
members = ["bodies/thalassa.toml", "bodies/ferrous.toml", "bodies/pale.toml", "stations/haven.toml"]

[[faction]]
name = "merchant_concord"
power = 12.5
stance = ["stance.hostile.rust_clan"]

[[faction]]
name = "rust_clan"
power = 4.0

[[market]]
id = "haven_market"
anchor = "stations/haven.toml"

[[market]]
id = "rustflare_market"
anchor = "poi:rustflare"

[[production]]
market = "haven_market"
commodity = "food"
rate = 6.0

[[production]]
market = "rustflare_market"
commodity = "iron_ore"
rate = 2.25

[[lane]]
id = "lane_ore_haven"
from = "rustflare_market"
to = "haven_market"
commodity = "iron_ore"
flow = 1.75

[[waypoint]]
id = "wp_high_gate"
kind = "patrol"
body = "bodies/thalassa.toml"
at = [7100000.0, 0.0, 0.0]

[[edge]]
kind = "lane"
from = "rustflare_market"
to = "wp_high_gate"

[[edge]]
kind = "patrol"
from = "wp_high_gate"
to = "haven_market"

[[job]]
id = "opening_cargo_run"

[[job_step]]
job = "opening_cargo_run"
verb = "undock"

[[job_step]]
job = "opening_cargo_run"
verb = "cargo_run"
target = "lane_ore_haven"
amount = 2000.0

[[job_step]]
job = "opening_cargo_run"
verb = "dock"
target = "haven_market"

[[job]]
id = "bounty_patrol"

[[job_step]]
job = "bounty_patrol"
verb = "patrol"
target = "wp_high_gate"

[[job_step]]
job = "bounty_patrol"
verb = "interdict"
target = "wp_high_gate"
amount = 1.0
)toml";

// Thalassa: Earth-class (#22a: 6,371 km), well 100x radius (top of the
// 10..100 band — must clear Pale's orbit + its own well for the #17b nest).
constexpr const char* kThalassa = R"toml(
world_class = "body"
name = "Thalassa"
radius = "6371 km"
well_radius = "637100 km"
surface_g = 9.81
well_taper_start = "550000 km"
atm_rho0 = 1.225
atm_scale_height = "8.5 km"
spin_axis = [0.0, 1.0, 0.0]
spin_rate = "360 deg/day"
tags = ["owner.merchant_concord"]

[safe]
bubble = "1200000 km"
speed = 3000
impact = 250

[terrain]
seed = 4001
relief = "8800 m"
base_frequency = 0.6
octaves = 6
lacunarity = 2.0
gain = 0.5

[[stamp]]
type = "crater"
center = [0.0, 0.9, 0.4]
radius = "0.06 rad"
level = "-900 m"
blend = 0.3

[[poi]]
name = "dawn_pad"
kind = "pad"
stamp = "flatten"
center = [0.3, 0.9, 0.31]
radius = "0.02 rad"
level = "120 m"
footprint = "60 m"

[[biome]]
lo = "0 m"
hi = "400 m"
color = [0.13, 0.36, 0.16]
roughness = 0.82
scatter = 4.0

[[biome]]
lo = "400 m"
hi = "2200 m"
color = [0.34, 0.31, 0.24]
roughness = 0.71
scatter = 6.0
)toml";

// Ferrous: Mars-class at 1.52 AU from the root (#22a real separation),
// oblique spin axis (25.2 deg off, like Mars), rust_clan-owned, thin
// atmosphere, outpost market node (#20a).
constexpr const char* kFerrous = R"toml(
world_class = "body"
name = "Ferrous"
radius = "3389.5 km"
well_radius = "338950 km"
surface_g = 3.71
well_taper_start = "300000 km"
atm_rho0 = 0.02
atm_scale_height = "11.1 km"
parent = "../bodies/thalassa.toml"
orbit_period = "400000 day"
orbit_radius = "1.52 AU"
orbit_phase = "30 deg"
spin_axis = [0.4226, 0.9063, 0.0]
spin_rate = "354.6 deg/day"
tags = ["owner.rust_clan"]

[terrain]
seed = 4002
relief = "2100 m"
base_frequency = 0.5
octaves = 5
lacunarity = 2.0
gain = 0.5

[[poi]]
name = "rustflare"
kind = "outpost"
stamp = "flatten"
center = [0.6, 0.6, 0.52]
radius = "0.004 rad"
level = "-40 m"
footprint = "500 m"

[[biome]]
lo = "0 m"
hi = "700 m"
color = [0.55, 0.24, 0.13]
roughness = 0.9
scatter = 2.0

[[biome]]
lo = "700 m"
hi = "1500 m"
color = [0.42, 0.20, 0.12]
roughness = 0.8
scatter = 1.0
)toml";

// Pale: Earth-Moon-class, airless (landing testbed #17c), tidally locked
// with a spin_rate authored to be BIT-EXACT with the orbit mean motion
// (360/28 deg/day through the ratified (pi/180)/86400 factor == tau/P at
// these values; the rails test below pins the equality at exactly 0.0).
constexpr const char* kPale = R"toml(
world_class = "body"
name = "Pale"
radius = "1737.4 km"
well_radius = "173740 km"
surface_g = 1.62
well_taper_start = "150000 km"
parent = "../bodies/thalassa.toml"
orbit_period = "28 day"
orbit_radius = "384400 km"
orbit_phase = "0 deg"
spin_rate = "12.857142857142858 deg/day"

[terrain]
seed = 4003
relief = "1800 m"
base_frequency = 0.55
octaves = 5

[[stamp]]
type = "crater"
center = [0.2, 0.8, -0.5]
radius = "0.1 rad"
level = "-600 m"
blend = 0.25

[[biome]]
lo = "0 m"
hi = "900 m"
color = [0.42, 0.42, 0.44]
roughness = 0.95
scatter = 0.5
)toml";

// Haven: station pinned to a low Thalassa orbit (#24a), merchant-owned,
// with an authored build plan for 8.21 (module on the free hp_engine_R).
constexpr const char* kHaven = R"toml(
world_class = "station"
name = "Haven Station"
hull_def = "../../defs/station_core.def.toml"
parent = "../bodies/thalassa.toml"
orbit_period = "5400 s"
orbit_radius = "7000 km"
orbit_phase = "90 deg"
tags = ["owner.merchant_concord"]
root_socket = "hp_engine_L"

[[build_step]]
plan = "expansion_a"
module = "../../defs/ring_module.def.toml"
socket = "hp_engine_R"
at = "2 day"
)toml";

constexpr const char* kSeamsStation = R"toml(

[[seam]]
partner = "ring_module.def.toml"
socket = "hp_engine_R"
)toml";

constexpr const char* kSeamsRing = R"toml(

[[seam]]
partner = "station_core.def.toml"
socket = "hp_engine_R"
)toml";

void write_slice(const Tree& t) {
    t.write("world/system.toml", kSystem);
    t.write("world/bodies/thalassa.toml", kThalassa);
    t.write("world/bodies/ferrous.toml", kFerrous);
    t.write("world/bodies/pale.toml", kPale);
    t.write("world/stations/haven.toml", kHaven);
    t.write("defs/station_core.def.toml", t.ship_text + kSeamsStation);
    t.write("defs/ring_module.def.toml", t.ship_text + kSeamsRing);
}

core::cooked::Unit unit_of(const Tree& t, const std::string& id) {
    core::cooked::Unit u;
    std::string err;
    REQUIRE_MESSAGE(core::cooked::read_unit_file((t.out / ("units/" + id + ".ck")).string(), u, &err), err);
    return u;
}

const core::cooked::ManifestEntry* entry_of(const core::cooked::Manifest& m, const std::string& id) {
    const auto it = m.find(id);
    return it == m.end() ? nullptr : &it->second;
}

// Ratified unit factors re-expressed exactly as the table groups them
// (game/include/ve/game/units.h): deg = kPi_d/180, deg/day =
// (kPi_d/180)/86400 — the hand pins below reuse these groupings.
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;
constexpr double kDeg_per_day = (kPi / 180.0) / 86400.0;

}  // namespace

TEST_CASE("realscale_world") {
    Tree t;
    write_slice(t);
    auto res = cook::cook_incremental(t.src.string(), t.out.string());
    REQUIRE_MESSAGE(res.ok, res.error);
    // 5 WRLD + 2 hull DEF0 (+ mesh deps)
    CHECK(res.counts.cooked >= 7);

    // ---- routing + one-codec byte-exact re-encode (8.7 doctrine) ----
    for (const char* id : {"world/system.toml", "world/bodies/thalassa.toml",
                           "world/bodies/ferrous.toml", "world/bodies/pale.toml",
                           "world/stations/haven.toml"}) {
        const auto u = unit_of(t, id);
        CHECK_MESSAGE(u.type == core::cooked::kUnitWrld, id);
        auto dec = game::decode_world(u.payload);
        REQUIRE_MESSAGE(dec, dec.error());
        CHECK(game::encode_world(*dec) == u.payload);
    }
    CHECK(unit_of(t, "defs/station_core.def.toml").type == core::cooked::kUnitDef0);
    CHECK(unit_of(t, "defs/ring_module.def.toml").type == core::cooked::kUnitDef0);

    // ---- decoded spot values: REAL radii and AU separations (#22a) ----
    auto thal = game::decode_world(unit_of(t, "world/bodies/thalassa.toml").payload);
    auto fer  = game::decode_world(unit_of(t, "world/bodies/ferrous.toml").payload);
    auto pale = game::decode_world(unit_of(t, "world/bodies/pale.toml").payload);
    auto hav  = game::decode_world(unit_of(t, "world/stations/haven.toml").payload);
    auto sys  = game::decode_world(unit_of(t, "world/system.toml").payload);
    REQUIRE(thal); REQUIRE(fer); REQUIRE(pale); REQUIRE(hav); REQUIRE(sys);

    CHECK(sys->name == "Sable Reach");
    CHECK(sys->seed == 987654321u);
    REQUIRE(sys->members.size() == 4);
    REQUIRE(sys->factions.size() == 2);
    REQUIRE(sys->markets.size() == 2);
    // Outpost market node anchored to the POI, not a station (#20a) — the
    // "poi:<name>" anchor is not a file-graph ref and survives decode verbatim.
    const auto& rk = sys->markets[0].id == "haven_market" ? sys->markets[0] : sys->markets[1];
    const auto& rm = sys->markets[0].id == "rustflare_market" ? sys->markets[0] : sys->markets[1];
    CHECK(rk.anchor_ref == "stations/haven.toml");
    CHECK(rm.anchor_ref == "poi:rustflare");
    REQUIRE(rm.commodities.size() == 1);
    CHECK(rm.commodities[0].commodity == "iron_ore");
    CHECK(rm.commodities[0].production == doctest::Approx(2.25));
    REQUIRE(sys->lanes.size() == 1);
    CHECK(sys->lanes[0].from == "rustflare_market");
    CHECK(sys->lanes[0].flow_rate == doctest::Approx(1.75));
    REQUIRE(sys->waypoints.size() == 1);
    CHECK(sys->waypoints[0].kind == game::NavKind::patrol);
    REQUIRE(sys->nav_edges.size() == 2);
    // Opening contract chain + the director's bounty link (#33a/#20d/#33)
    REQUIRE(sys->job_templates.size() == 2);
    const auto& bounty = sys->job_templates[1].id == "bounty_patrol" ? sys->job_templates[1]
                                                                    : sys->job_templates[0];
    REQUIRE(bounty.steps.size() == 2);
    CHECK(bounty.steps[0].target_ref == "wp_high_gate");
    CHECK(bounty.steps[1].verb == "interdict");

    // Real Earth/Mars/Moon numbers straight out of the cooked payload.
    CHECK(thal->rails.surface_radius == 6.371e6);        // "6371 km"
    CHECK(thal->rails.well_radius == 6.371e8);           // 100x (top of #22a band)
    CHECK(thal->rails.well_taper_start == 5.5e8);
    CHECK(thal->rails.atm_rho0_kg_m3 == doctest::Approx(1.225));
    CHECK(thal->rails.atm_scale_height_m == doctest::Approx(8500.0));
    CHECK(thal->rails.spin_rate_rad_s == 360.0 * kDeg_per_day);  // sidereal-day rate
    // 8.13 [safe] trio rides the v7 payload (#50).
    CHECK(thal->rails.safe_bubble_radius == 1.2e9);  // "1200000 km" >= well 6.371e8 (#22a band)
    CHECK(thal->rails.safe_speed == 3000.0);         // bare SI m/s
    CHECK(thal->safe_impact == 250.0);               // game-side flat value, never core
    CHECK(fer->rails.safe_bubble_radius == 0.0);     // bodies without [safe]: no wall

    CHECK(fer->rails.surface_radius == 3.3895e6);        // Mars mean radius
    CHECK(fer->rails.well_radius == 3.3895e8);
    CHECK(fer->rails.orbit_radius_m == 1.52 * ve::game::kMeters_per_AU);
    CHECK(fer->rails.orbit_radius_m == 227388763464.0);  // 1.52 AU, exact double
    CHECK(fer->rails.orbit_period_s == 400000.0 * 86400.0);
    CHECK(fer->rails.orbit_phase_rad == 30.0 * kDeg);
    CHECK(fer->rails.spin_rate_rad_s == 354.6 * kDeg_per_day);  // Mars sol
    CHECK(fer->rails.spin_axis.x == doctest::Approx(0.4226));   // 25.2 deg obliquity
    REQUIRE(fer->pois.size() == 1);
    CHECK(fer->pois[0].name == "rustflare");
    CHECK(fer->pois[0].kind == game::PoiKind::outpost);
    CHECK(fer->pois[0].footprint_m == doctest::Approx(500.0));

    CHECK(pale->rails.surface_radius == 1.7374e6);       // Moon mean radius
    CHECK(pale->rails.orbit_radius_m == 3.844e8);        // Moon semi-major axis
    CHECK(pale->rails.orbit_period_s == 28.0 * 86400.0);
    CHECK(pale->rails.atm_rho0_kg_m3 == 0.0);            // airless (#17c)
    CHECK(pale->rails.atm_scale_height_m == 0.0);

    CHECK(hav->rails.orbit_radius_m == 7.0e6);           // low parking orbit
    CHECK(hav->rails.orbit_period_s == 5400.0);          // authored, not Kepler
    CHECK(hav->hull_def_ref == "../../defs/station_core.def.toml");
    CHECK(hav->root_socket == "hp_engine_L");
    REQUIRE(hav->build_plans.size() == 1);               // fixture for 8.21
    CHECK(hav->build_plans[0].id == "expansion_a");
    REQUIRE(hav->build_plans[0].steps.size() == 1);
    CHECK(hav->build_plans[0].steps[0].at_time_s == 172800.0);
    CHECK(hav->build_plans[0].steps[0].target_socket == "hp_engine_R");

    // ---- well/atmosphere sanity (#22a + #17) ----
    // Well radii stay authored multiples of body radius, inside 10..100x.
    for (const auto* d : {&*thal, &*fer, &*pale}) {
        const double ratio = d->rails.well_radius / d->rails.surface_radius;
        CHECK_MESSAGE(ratio >= 10.0, d->name);
        CHECK_MESSAGE(ratio <= 100.0, d->name);
        CHECK(d->rails.well_taper_start > d->rails.surface_radius);
        CHECK(d->rails.well_taper_start < d->rails.well_radius);
        if (d->rails.atm_rho0_kg_m3 > 0.0)
            CHECK(d->rails.atm_scale_height_m > 0.0);
    }
    // #17b testbed at real scale: the moon's whole well nests inside the
    // parent's well, and the station orbits deep inside it too.
    CHECK(pale->rails.orbit_radius_m + pale->rails.well_radius < thal->rails.well_radius);
    CHECK(hav->rails.orbit_radius_m < thal->rails.well_radius);
    // #22a AU-class topology: the far planet sits ~357 parent-wells away —
    // comfortably outside any sublight reach, inside one warp leg (8.12+).
    CHECK(fer->rails.orbit_radius_m > 300.0 * thal->rails.well_radius);

    // ---- manifest edges (#39): members/parent/hull refs, sorted ----
    core::cooked::Manifest m;
    std::string err;
    REQUIRE(core::cooked::read_manifest((t.out / "manifest.bin").string(), m, &err));
    const auto* sys_e = entry_of(m, "world/system.toml");
    REQUIRE(sys_e);
    // member edges only; "poi:rustflare" is deliberately NOT an edge, and
    // the market/waypoint anchors dedup onto existing member edges.
    const std::vector<std::string> kSysDeps{"world/bodies/ferrous.toml",
                                            "world/bodies/pale.toml",
                                            "world/bodies/thalassa.toml",
                                            "world/stations/haven.toml"};
    CHECK(sys_e->deps == kSysDeps);
    const auto* fer_e = entry_of(m, "world/bodies/ferrous.toml");
    REQUIRE(fer_e);
    const std::vector<std::string> kFerDeps{"world/bodies/thalassa.toml"};
    CHECK(fer_e->deps == kFerDeps);
    const auto* hav_e = entry_of(m, "world/stations/haven.toml");
    REQUIRE(hav_e);
    const std::vector<std::string> kHavDeps{"defs/ring_module.def.toml",
                                            "defs/station_core.def.toml",
                                            "world/bodies/thalassa.toml"};
    CHECK(hav_e->deps == kHavDeps);

    // ================= rails, fp64-checked at AU scale =================
    // Assemble the rail world FROM THE COOKED PAYLOADS (the fixture's real
    // use: decode -> body_def -> rail_world), parents resolved through the
    // cook's own lexical resolver.
    rail_world rw;
    std::vector<std::string> member_ids;  // content id by body index
    for (const auto& mem : sys->members) {
        std::string mid;
        REQUIRE(cook::resolve_ref("world/system.toml", mem, mid));
        const WorldDef* d = mem == "bodies/thalassa.toml" ? &*thal
                        : mem == "bodies/ferrous.toml"    ? &*fer
                        : mem == "bodies/pale.toml"       ? &*pale
                                                          : &*hav;
        body_def b = d->rails;
        if (!d->parent_ref.empty()) {
            std::string pid;
            REQUIRE(cook::resolve_ref(mid, d->parent_ref, pid));
            auto pit = std::ranges::find(member_ids, pid);
            REQUIRE(pit != member_ids.end());  // members list is parent-first
            b.parent = static_cast<std::int32_t>(pit - member_ids.begin());
        }
        if (d->world_class == WorldClass::station) {
            // Hull extent lives in DEF0 geometry; the add_body gate wants a
            // real >0 surface_radius. The true bound gets wired when 8.12+
            // assembles sim entities from cooked hulls.
            b.surface_radius = 100.0;
        }
        const auto id = rw.add_body(b);
        REQUIRE_MESSAGE(id, d->name);
        member_ids.push_back(std::move(mid));
    }
    REQUIRE(rw.body_count() == 4);
    const std::int32_t Thal = 0, Ferrous = 1, Pale = 2, Haven = 3;

    // Hand-pinned rail eval. Every constant below is the exact double from
    // the Python op-order port (realscale_pins.py); Approx(eps 1e-12) per
    // the trig-pin policy, hard == where the value is exact (zeros, ±R).
    constexpr double R_fer = 227388763464.0;  // 1.52 AU
    constexpr double R_pale = 384400000.0;
    constexpr double R_haven = 7000000.0;

    auto near12 = [](double got, double want) {
        return got == doctest::Approx(want).epsilon(1e-12);
    };
    auto check_pos = [&](std::int32_t id, tick_t tick,
                         double x, double y, double z) {
        const auto p = rw.position(id, tick);
        REQUIRE(p);
        if (x != 0.0) CHECK(p->x == doctest::Approx(x).epsilon(1e-12)); else CHECK(p->x == x);
        CHECK(p->y == y);
        if (z != 0.0) CHECK(p->z == doctest::Approx(z).epsilon(1e-12)); else CHECK(p->z == z);
    };

    // tick 0: phases as authored
    check_pos(Pale, 0, 384400000.0, 0.0, 0.0);
    check_pos(Haven, 0, 4.286263797015736e-10, 0.0, -7000000.0);   // cos(90 deg) residue
    check_pos(Ferrous, 0, 196924445694.95483, 0.0, -113694381731.99998);

    // exact quarter-orbit ticks: quarter periods are whole tick counts
    const tick_t tq_pale = 36288000, tq_haven = 81000, tq_fer = 518400000000LL;
    check_pos(Pale, tq_pale, 2.353771147961213e-08, 0.0, -384400000.0);
    check_pos(Haven, tq_haven, -7000000.0, 0.0, -8.572527594031472e-10);
    check_pos(Ferrous, tq_fer, -113694381731.99995, 0.0, -196924445694.95483);

    // deep future: ten years of ticks (time-compression range, 8.14+)
    const tick_t T10 = 18921600000LL;  // 3650 days at 60 Hz
    check_pos(Pale, T10, -239669479.83448222, 0.0, -300536021.86072254);
    check_pos(Haven, T10, 0.0001228453785437571, 0.0, -7000000.0);
    check_pos(Ferrous, T10, 190085879341.85452, 0.0, -124791859608.39297);

    // fp64 metrology AT AU scale: |position| returns to the authored orbit
    // radius EXACTLY (sqrt(R^2 (c^2+s^2)) rounds to R) — at 1.52 AU the fp64
    // ulp is 3.05e-05 m (pinned below), where fp32 would have a 16,384 m
    // quantum. This is the scale 8.9's origin math and 8.10's log-zoom ride.
    CHECK(*rw.position(Ferrous, T10) == *rw.position(Ferrous, T10));  // pure: bit-stable
    const auto pf = rw.position(Ferrous, T10);
    const auto pp = rw.position(Pale, T10);
    const auto ph = rw.position(Haven, T10);
    const double len_f = std::sqrt(pf->x * pf->x + pf->y * pf->y + pf->z * pf->z);
    const double len_p = std::sqrt(pp->x * pp->x + pp->y * pp->y + pp->z * pp->z);
    const double len_h = std::sqrt(ph->x * ph->x + ph->y * ph->y + ph->z * ph->z);
    CHECK(len_f - R_fer == 0.0);
    CHECK(len_p - R_pale == 0.0);
    CHECK(len_h - R_haven == 0.0);
    CHECK(std::nextafter(R_fer, INFINITY) - R_fer == 3.0517578125e-05);

    // AU-scale separations (the warp-leg distances 8.12 will cross):
    const double sep_pf = std::sqrt((pf->x - pp->x) * (pf->x - pp->x) +
                                    (pf->z - pp->z) * (pf->z - pp->z));
    CHECK(near12(sep_pf, 227424502159.73608));
    const double sep_hf = std::sqrt((pf->x - ph->x) * (pf->x - ph->x) +
                                    (pf->z - ph->z) * (pf->z - ph->z));
    CHECK(near12(sep_hf, 227384921911.39758));

    // velocities: central difference of the analytic position. Moon-class
    // speed ~1 km/s, LEO station ~8.1 km/s, far planet ~41 m/s.
    const auto vp = rw.velocity(Pale, T10), vh = rw.velocity(Haven, T10),
               vf = rw.velocity(Ferrous, T10);
    REQUIRE(vp); REQUIRE(vh); REQUIRE(vf);
    CHECK(near12(vp->x, -780.5561596155167));
    CHECK(vp->y == 0.0);
    CHECK(near12(vp->z, 622.4727666378021));
    CHECK(near12(vh->x, -8144.859456878014));
    CHECK(vh->y == 0.0);
    CHECK(near12(vh->z, -1.3969838619232178e-07));
    CHECK(near12(vf->x, -22.6885986328125));
    CHECK(vf->y == 0.0);
    CHECK(near12(vf->z, -34.55841064453125));
    CHECK(near12(std::sqrt(vp->x * vp->x + vp->z * vp->z), 998.3688013552124));
    CHECK(near12(std::sqrt(vh->x * vh->x + vh->z * vh->z), 8144.859456878014));

    // spin/orientation at T10
    const auto ot = rw.orientation(Thal, T10), op = rw.orientation(Pale, T10),
               oh = rw.orientation(Haven, T10);
    REQUIRE(ot); REQUIRE(op); REQUIRE(oh);
    CHECK(ot->x == 0.0);   // -0.0 == 0.0
    CHECK(near12(ot->y, -4.68312363761587e-13));
    CHECK(ot->z == 0.0);
    CHECK(ot->w == 1.0);
    CHECK(op->x == 0.0);
    CHECK(near12(op->y, 0.9009688679022052));   // world attitude = thal spin * pale spin
    CHECK(op->z == 0.0);
    CHECK(near12(op->w, 0.43388373911800243));
    CHECK(oh->y == ot->y);  // station spin 0 -> world attitude IS the parent's

    // tidal lock, bit-exact: authored spin rate == orbit mean motion at
    // the double level, so the spin angle and orbit theta are the SAME
    // double at every tick — "same face to the planet" with zero drift.
    const double sec = core::sim::tick_to_seconds(T10);
    const double spin_ang = pale->rails.spin_rate_rad_s * sec;
    const double orbit_theta = pale->rails.orbit_phase_rad +
                               ve::core::math::kTau_d * (sec / pale->rails.orbit_period_s);
    CHECK(spin_ang == orbit_theta);
    CHECK(spin_ang == 819.0580846859103);
}
