// Phase 8 task 8.7 — world-def TOML schema + WORLD cook unit (#45d).
// worlddef_cook: a whole authored slice (system + root body + moon +
// station) cooks to WRLD units; the shared codec round-trips byte-for-byte
// and decoded SI spot values pin the #42 unit coercion; decode gates refuse
// tampered payloads.
// worlddef_manifest_deps: members/parents/hull refs become sorted manifest
// edges (hull==module dedups); a dangling ref fails the cook at cook time.
// worlddef_gate_order: schema gates run in fixed order and report the
// FIRST failure only — the test walks the error ladder by fixing one gate
// at a time.
#include <ve/cook/cook.h>
#include <ve/cook/worlddef.h>
#include <ve/core/cooked.h>
#include <ve/core/sim/terrain.h>
#include <ve/game/world_load.h>

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <utility>
#include <vector>
#include "../ve_test_portability.h"  // getpid

#ifndef VE_SOURCE_DIR
#error "VE_SOURCE_DIR must be defined by CMake"
#endif

using namespace ve;
using ve::game::WorldClass;
using ve::game::WorldDef;

namespace {

struct Tree {
    std::filesystem::path src, out;
    Tree() {
        static int n = 0;
        auto base = std::filesystem::temp_directory_path() /
                    ("ve_worlddef_" + std::to_string(ve_test::pid()) + "_" + std::to_string(++n));
        src = base / "src";
        out = base / "out";
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(src / "world/bodies");
        std::filesystem::create_directories(src / "world/stations");
        std::filesystem::create_directories(src / "defs");
        std::filesystem::create_directories(src / "samples");
        // real hull fixture as the station's hull/module def
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
orbit_phase = "45 deg"
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

// 8.8: station + module hulls with REAL #46d seam pairs, so the build-plan
// clamp-pairing gate has a proven pair to bind onto (5.9b G9 validates the
// rows themselves; the ring defs share the ship mesh/SOCK on purpose).
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

// A body no system claims: exercised by the member/anchor/waypoint gates.
constexpr const char* kOrphan = R"toml(
world_class = "body"
name = "Orphan Rock"
radius = "180 km"
well_radius = "600 km"
surface_g = 0.3
well_taper_start = "400 km"
)toml";

void write_slice(const Tree& t) {
    t.write("world/system.toml", kSystem);
    t.write("world/bodies/vesper.toml", kVesper);
    t.write("world/bodies/luna.toml", kLuna);
    t.write("world/bodies/orphan.toml", kOrphan);
    t.write("world/stations/kepler.toml", kKepler);
    t.write("defs/station_core.def.toml", t.ship_text + kSeamsStation);
    t.write("defs/ring_module.def.toml", t.ship_text + kSeamsRing);
    t.write("defs/ring_module_b.def.toml", t.ship_text + kSeamsRingB);
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

} // namespace

namespace {

const char* kBody = "world/bodies/vesper.toml";
const char* kMoon = "world/bodies/luna.toml";
const char* kSys = "world/system.toml";
const char* kKep = "world/stations/kepler.toml";
} // namespace

TEST_CASE("worlddef_cook") {
    Tree t;
    write_slice(t);
    auto res = cook::cook_incremental(t.src.string(), t.out.string());
    REQUIRE_MESSAGE(res.ok, res.error);
    CHECK(res.counts.cooked >= 5); // 4 WRLD + the DEF0 hull def (+ its deps)

    // routing: world defs became WRLD units, the hull def stayed DEF0
    CHECK(unit_of(t, "defs/test_ship.def.toml").type == core::cooked::kUnitDef0);
    for (const char* id : {"world/system.toml", "world/bodies/vesper.toml",
                           "world/bodies/luna.toml", "world/stations/kepler.toml"}) {
        const auto u = unit_of(t, id);
        CHECK(u.type == core::cooked::kUnitWrld);

        // ONE codec both directions: re-encode == encode, byte-for-byte.
        auto dec = game::decode_world(u.payload);
        REQUIRE_MESSAGE(dec, dec.error());
        CHECK(game::encode_world(*dec) == u.payload);
    }

    // ---- decoded spot values (SI; #42 coercion pinned at the edges) ----
    auto sys = game::decode_world(unit_of(t, "world/system.toml").payload);
    REQUIRE(sys);
    CHECK(sys->world_class == WorldClass::system);
    CHECK(sys->name == "Vesper");
    CHECK(sys->seed == 12345u);
    REQUIRE(sys->members.size() == 3);
    CHECK(sys->members[0] == "bodies/vesper.toml"); // authored strings survive
    REQUIRE(sys->factions.size() == 2);
    CHECK(sys->factions[0].name == "vesper_authority");
    REQUIRE(sys->factions[0].tags.size() == 1);
    CHECK(sys->factions[0].tags[0] == "stance.hostile.pirate_confederacy");
    REQUIRE(sys->markets.size() == 2);
    CHECK(sys->markets[0].id == "kepler_market");
    CHECK(sys->markets[0].anchor_ref == "stations/kepler.toml");
    REQUIRE(sys->markets[0].commodities.size() == 1); // rows joined by market id
    CHECK(sys->markets[0].commodities[0].commodity == "refined_ore");
    CHECK(sys->markets[0].commodities[0].production == 2.5);
    REQUIRE(sys->lanes.size() == 1);
    CHECK(sys->lanes[0].commodity == "refined_ore");
    CHECK(sys->lanes[0].flow_rate == 1.75);
    REQUIRE(sys->waypoints.size() == 1);
    CHECK(sys->waypoints[0].kind == game::NavKind::patrol);
    REQUIRE(sys->nav_edges.size() == 2);
    CHECK(sys->nav_edges[0].kind == game::NavKind::lane);
    CHECK(sys->nav_edges[1].kind == game::NavKind::patrol);
    REQUIRE(sys->job_templates.size() == 1);
    CHECK(sys->job_templates[0].id == "opening_cargo_run");
    REQUIRE(sys->job_templates[0].steps.size() == 3); // steps joined by job id, authored order
    CHECK(sys->job_templates[0].steps[1].verb == "cargo_run");
    CHECK(sys->job_templates[0].steps[1].target_ref == "lane_ore_kepler_vesper");
    CHECK(sys->job_templates[0].steps[1].amount == 2500.0);

    auto v = game::decode_world(unit_of(t, "world/bodies/vesper.toml").payload);
    REQUIRE(v);
    CHECK(v->world_class == WorldClass::body);
    const auto& b = v->rails;
    CHECK(b.surface_radius == doctest::Approx(6.2e6));   // "6200 km"
    CHECK(b.well_radius == doctest::Approx(9.0e6));      // "9000 km"
    CHECK(b.surface_g == doctest::Approx(9.4));          // bare SI (composite unit)
    CHECK(b.well_taper_start == doctest::Approx(8.0e6)); // "8000 km"
    CHECK(b.atm_rho0_kg_m3 == doctest::Approx(1.18));
    CHECK(b.atm_scale_height_m == doctest::Approx(11000.0)); // "11 km"
    CHECK(b.orbit_period_s == 0.0);             // root anchor
    // "15 deg/day" through the ratified table ((pi/180)/86400 grouping)
    CHECK(b.spin_rate_rad_s == doctest::Approx(15.0 * (std::numbers::pi / 180.0) / 86400.0));
    REQUIRE(v->tags.size() == 1);
    CHECK(v->tags[0] == "owner.vesper_authority");
    CHECK(v->terrain.seed == 777u);
    CHECK(v->terrain.relief_m == doctest::Approx(1400.0));
    CHECK(v->terrain.octaves == 5);
    CHECK(v->terrain.lacunarity == doctest::Approx(2.0));
    CHECK(v->terrain.gain == doctest::Approx(0.5));
    REQUIRE(v->stamps.size() == 1);
    CHECK(v->stamps[0].type == core::sim::stamp_type::crater);
    CHECK(v->stamps[0].level_m == doctest::Approx(-250.0)); // negative length suffix string
    CHECK(v->stamps[0].radius_rad == doctest::Approx(0.08));
    REQUIRE(v->pois.size() == 1);
    CHECK(v->pois[0].name == "kepler_pad");
    CHECK(v->pois[0].footprint_m == doctest::Approx(60.0));
    CHECK(v->pois[0].stamp.type == core::sim::stamp_type::flatten);
    REQUIRE(v->biomes.size() == 1);
    CHECK(v->biomes[0].hi_m == doctest::Approx(400.0));
    CHECK(v->biomes[0].color[1] == doctest::Approx(0.45)); // palette card #29d/#49
    CHECK(v->biomes[0].scatter_density == doctest::Approx(3.5));

    auto luna = game::decode_world(unit_of(t, "world/bodies/luna.toml").payload);
    REQUIRE(luna);
    CHECK(luna->parent_ref == "../bodies/vesper.toml"); // authored ref string
    CHECK(luna->rails.orbit_period_s == doctest::Approx(27 * 86400.0));
    CHECK(luna->rails.orbit_radius_m == doctest::Approx(3.84e8));
    CHECK(luna->rails.orbit_phase_rad == doctest::Approx(0.25 * std::numbers::pi));
    CHECK(luna->terrain.relief_m == 0.0); // no [terrain] = flat body (7.2 doctrine)

    auto st = game::decode_world(unit_of(t, "world/stations/kepler.toml").payload);
    REQUIRE(st);
    CHECK(st->world_class == WorldClass::station);
    CHECK(st->hull_def_ref == "../../defs/station_core.def.toml");
    CHECK(st->root_socket == "hp_engine_L");
    REQUIRE(st->build_plans.size() == 1); // plans in first-seen order
    CHECK(st->build_plans[0].id == "expansion_a");
    REQUIRE(st->build_plans[0].steps.size() == 1);
    CHECK(st->build_plans[0].steps[0].at_time_s == doctest::Approx(43200.0)); // "0.5 day"
    CHECK(st->build_plans[0].steps[0].target_socket == "hp_engine_R");
    CHECK(st->build_plans[0].steps[0].module_def_ref == "../../defs/ring_module.def.toml");

    // ---- decode gates refuse tampered payloads ----
    const auto good = unit_of(t, "world/bodies/vesper.toml").payload;
    CHECK(not game::decode_world(core::cooked::Bytes{}).error().empty());
    {
        auto p = good; p.resize(p.size() / 2); // truncated mid-field
        CHECK(game::decode_world(p).error().find("truncated") != std::string::npos);
    }
    {
        auto p = good; p.push_back(0); // trailing junk
        CHECK(game::decode_world(p).error().find("trailing") != std::string::npos);
    }
    {
        auto p = good; p[4] = 9; // world_class byte (after payload_ver) out of range
        CHECK(game::decode_world(p).error().find("world_class") != std::string::npos);
    }
}

TEST_CASE("worlddef_manifest_deps") {
    Tree t;
    write_slice(t);
    auto res = cook::cook_incremental(t.src.string(), t.out.string());
    REQUIRE_MESSAGE(res.ok, res.error);

    core::cooked::Manifest m;
    std::string err;
    REQUIRE(core::cooked::read_manifest((t.out / "manifest.bin").string(), m, &err));

    const auto* sys = entry_of(m, "world/system.toml");
    REQUIRE(sys);
    // market anchors + waypoint bodies are refs too (8.8) but they point at
    // members -> dedup keeps exactly one edge per member.
    CHECK(sys->deps == std::vector<std::string>{"world/bodies/luna.toml",
                                               "world/bodies/vesper.toml",
                                               "world/stations/kepler.toml"}); // sorted
    const auto* luna = entry_of(m, "world/bodies/luna.toml");
    REQUIRE(luna);
    CHECK(luna->deps == std::vector<std::string>{"world/bodies/vesper.toml"});
    const auto* kepler = entry_of(m, "world/stations/kepler.toml");
    REQUIRE(kepler);
    // hull def + build module (distinct defs) + parent body.
    CHECK(kepler->deps == std::vector<std::string>{"defs/ring_module.def.toml",
                                                   "defs/station_core.def.toml",
                                                   "world/bodies/vesper.toml"});

    // dangling ref fails the cook at cook time (#27a doctrine)
    Tree t2;
    write_slice(t2);
    // inject a dangling entry into the members array (the schema's ref key
    // for system -> bodies/stations; unknown keys are NOT schema data).
    std::string sysdoc{kSystem};
    const std::string mtail = "stations/kepler.toml\"]";  // quote-then-bracket
    const auto pos = sysdoc.find(mtail);
    REQUIRE(pos != std::string::npos);
    sysdoc.insert(pos + mtail.size() - 1, ", \"bodies/nothere.toml\"");  // before the closing ]
    t2.write("world/system.toml", sysdoc);
    auto bad = cook::cook_incremental(t2.src.string(), t2.out.string());
    CHECK_FALSE(bad.ok);
    CHECK_MESSAGE(bad.error.find("references missing") != std::string::npos, bad.error);
}

TEST_CASE("worlddef_gate_order") {
    // One body doc with THREE simultaneous violations (radius #42 gate,
    // stamp enum, biome band order). The ladder pins the fixed gate order
    // and that only the FIRST failure is ever reported.
    std::string doc = R"toml(
world_class = "body"
name = "Broken"
radius = "6200000"
well_radius = "9000 km"

[[stamp]]
type = "cratr"
center = [0.0, 1.0, 0.0]
radius = "0.1 rad"

[[biome]]
lo = "0 m"
hi = "-100 m"
color = [1.0, 1.0, 1.0]
)toml";
    std::vector<std::uint8_t> payload;
    std::vector<std::string> refs;
    std::string err;

    auto fix = [&](std::string& d, const char* from, const char* to) {
        const auto pos = d.find(from);
        REQUIRE(pos != std::string::npos);
        d.replace(pos, std::string(from).size(), to);
    };

    // step 0: the unit-grammar gate fires; nothing later leaks into the error
    REQUIRE_FALSE(cook::cook_worlddef_source(doc, payload, refs, &err));
    CHECK_MESSAGE(err.find("radius: unit coercion: bare number '6200000'") != std::string::npos, err);
    CHECK(err.find("cratr") == std::string::npos);
    CHECK(err.find("biome band") == std::string::npos);

    // step 1: fix radius -> next gate in schema order is the stamp enum
    fix(doc, "radius = \"6200000\"", "radius = \"6200 km\"");
    REQUIRE_FALSE(cook::cook_worlddef_source(doc, payload, refs, &err));
    CHECK_MESSAGE(err.find("stamp type 'cratr' must be flatten, crater or mesa") != std::string::npos, err);
    CHECK(err.find("biome band") == std::string::npos);

    // step 2: fix the stamp -> biome band gate fires
    fix(doc, "type = \"cratr\"", "type = \"crater\"");
    REQUIRE_FALSE(cook::cook_worlddef_source(doc, payload, refs, &err));
    CHECK_MESSAGE(err.find("biome band needs lo < hi") != std::string::npos, err);

    // rung: bad world_class outranks everything (gate 1)
    REQUIRE_FALSE(cook::cook_worlddef_source("world_class = \"moon\"\nradius = 42\n", payload, refs, &err));
    CHECK_MESSAGE(err.find("world_class") != std::string::npos, err);
    CHECK(err.find("name") == std::string::npos);

    // rung: missing name fires before radius (#42) on an otherwise-empty body
    REQUIRE_FALSE(cook::cook_worlddef_source("world_class = \"body\"\nradius = \"1000 km\"\n", payload, refs, &err));
    CHECK_MESSAGE(err.find("name: missing required key") != std::string::npos, err);

    // rung: orbit coupling gates
    std::string orb = "world_class = \"body\"\nname = \"X\"\nradius = \"1000 km\"\nparent = \"../bodies/other.toml\"\n";
    REQUIRE_FALSE(cook::cook_worlddef_source(orb, payload, refs, &err));
    CHECK_MESSAGE(err.find("orbiting entry needs orbit_period > 0") != std::string::npos, err);

    // rung: station needs its hull def; rows must join to declared tables
    std::string stn = "world_class = \"station\"\nname = \"S\"\n";
    REQUIRE_FALSE(cook::cook_worlddef_source(stn, payload, refs, &err));
    CHECK_MESSAGE(err.find("station needs a hull def ref") != std::string::npos, err);
    std::string join = "world_class = \"system\"\nname = \"S\"\n[[production]]\nmarket = \"nope\"\ncommodity = \"x\"\nrate = 1.0\n";
    REQUIRE_FALSE(cook::cook_worlddef_source(join, payload, refs, &err));
    CHECK_MESSAGE(err.find("references unknown market 'nope'") != std::string::npos, err);

    // rung: duplicate member refs are caught before dep existence
    std::string dup = "world_class = \"system\"\nname = \"S\"\nmembers = [\"a.toml\", \"a.toml\"]\n";
    REQUIRE_FALSE(cook::cook_worlddef_source(dup, payload, refs, &err));
    CHECK_MESSAGE(err.find("duplicate member ref") != std::string::npos, err);

    // valid minimal body: no error, payload decodes
    REQUIRE_MESSAGE(cook::cook_worlddef_source(
        "world_class = \"body\"\nname = \"Ok\"\nradius = \"1000 km\"\nsurface_g = 3.0\n",
        payload, refs, &err), err);
    auto dec = game::decode_world(payload);
    REQUIRE(dec);
    CHECK(dec->rails.surface_radius == doctest::Approx(1e6));
    CHECK(dec->rails.surface_g == doctest::Approx(3.0));
}

// ---------------------------------------------------------------------------
// 8.8 — world-def cross-field / cross-file validation gates (cook side).
// Every variant mutates ONE fixture line of the green slice and expects the
// cook to fail with the fixed-order first-error text from wrldvalidate.cpp.
// ---------------------------------------------------------------------------
namespace {

std::string repl(const std::string& s, const std::string& from, const std::string& to) {
    std::string out{s};
    const auto p = out.find(from);
    if (p == std::string::npos) throw std::logic_error("fixture anchor not found: " + from);
    out.replace(p, from.size(), to);
    return out;
}

// cook the full slice with these files overwritten; returns the cook error
// (empty string = cook stayed green).
std::string gate_error(std::vector<std::pair<std::string, std::string>> muts) {
    Tree t;
    write_slice(t);
    for (const auto& [f, text] : muts) t.write(f, text);
    auto res = cook::cook_incremental(t.src.string(), t.out.string());
    return res.ok ? std::string{} : res.error;
}

} // namespace

TEST_CASE("worlddef_validate_stamps") {
    auto expect_gate = [&](std::vector<std::pair<std::string, std::string>> muts,
                           const char* needle) {
        const std::string err = gate_error(std::move(muts));
        CHECK_MESSAGE(err.find(needle) != std::string::npos,
                      "wanted '" << std::string{needle} << "', cook said: " << err);
    };
    // POI stamp coverage + pad shape + blend-core gates (#29c).
    // ---- POI stamp gates (#29c) ----
    // Ladder pins the FIXED ORDER: coverage fires before the pad-shape gate.
    expect_gate({{kBody, repl(repl(kVesper, "footprint = \"60 m\"", "footprint = \"400 km\""),
                              "stamp = \"flatten\"", "stamp = \"crater\"")}},
                "does not cover footprint");
    expect_gate({{kBody, repl(kVesper, "stamp = \"flatten\"", "stamp = \"crater\"")}},
                "pad stamp must be flatten or mesa, not crater");
    // blend ring too fat: the pad plane could sit off the bit-constant core,
    // breaking the pad-normal ∥ well-normal proof (#29c).
    const std::string blendy =
        repl(repl(kVesper, "level = \"120 m\"", "level = \"120 m\"\nblend = 0.9"),
             "footprint = \"60 m\"", "footprint = \"100 km\"");
    expect_gate({{kBody, blendy}}, "plateau under a");
}


TEST_CASE("worlddef_validate_safe") {
    // [safe] body gates (#50, 8.13), fixed order, first failure only.
    auto expect_gate = [&](std::vector<std::pair<std::string, std::string>> muts,
                           const char* needle) {
        const std::string err = gate_error(std::move(muts));
        CHECK_MESSAGE(err.find(needle) != std::string::npos,
                      "wanted '" << std::string{needle} << "', cook said: " << err);
    };
    const std::string safe_ok = repl(kVesper, "tags = [\"owner.vesper_authority\"]",
        "tags = [\"owner.vesper_authority\"]\n[safe]\nbubble = \"9500 km\"\nspeed = 3000");
    // control: a legal [safe] trio (wall 9500 km > well 9000 km) cooks green.
    CHECK(gate_error({{kBody, safe_ok}}).empty());

    // bubble inside the well: the warp wall cannot sit inside the well edge.
    expect_gate({{kBody, repl(safe_ok, "bubble = \"9500 km\"", "bubble = \"7000 km\"")}},
                "is inside well_radius");
    // bubble without a speed ceiling: half-authored.
    expect_gate({{kBody, repl(kVesper, "tags = [\"owner.vesper_authority\"]",
        "tags = [\"owner.vesper_authority\"]\n[safe]\nbubble = \"9500 km\"")}},
                "[safe].bubble set without [safe].speed");
    // negative impact: the >= 0 gate fires FIRST even when the bubble is also
    // inside the well (ladder pins the fixed order).
    expect_gate({{kBody, repl(kVesper, "tags = [\"owner.vesper_authority\"]",
        "tags = [\"owner.vesper_authority\"]\n[safe]\nbubble = \"7000 km\"\nspeed = 3000\nimpact = -5")}},
                "[safe] fields must be >= 0");
}

TEST_CASE("worlddef_validate_graph_refs") {
    auto expect_gate = [&](std::vector<std::pair<std::string, std::string>> muts,
                           const char* needle) {
        const std::string err = gate_error(std::move(muts));
        CHECK_MESSAGE(err.find(needle) != std::string::npos,
                      "wanted '" << std::string{needle} << "', cook said: " << err);
    };
    // ---- rail parent chain (#45c) + graph refs ----
    const std::string vesper_moon = repl(
        kVesper, "tags = [\"owner.vesper_authority\"]",
        "tags = [\"owner.vesper_authority\"]\nparent = \"../bodies/luna.toml\"\n"
        "orbit_period = \"20 day\"\norbit_radius = \"900000 km\"");
        expect_gate({{kBody, vesper_moon}}, "rail parent cycle");  // luna->vesper->luna

    const std::string vesper_bad_parent = repl(
        kVesper, "tags = [\"owner.vesper_authority\"]",
        "tags = [\"owner.vesper_authority\"]\nparent = \"../../defs/test_ship.def.toml\"\n"
    "orbit_period = \"20 day\"\norbit_radius = \"900000 km\"");
    expect_gate({{kBody, vesper_bad_parent}}, "is not a cooked world def (#45c)");


    // member rail parent must live inside the system (#39)
    expect_gate({{kMoon, repl(kLuna, "../bodies/vesper.toml", "../bodies/orphan.toml")}},
                "rail parent 'world/bodies/orphan.toml' is not a member of this system (#39)");


    // ---- system graph gates ----
    expect_gate({{kSys, repl(kSystem, "stations/kepler.toml\"]",
                             "stations/kepler.toml\", \"../defs/test_ship.def.toml\"]")}},
                "is not a cooked world def (#45d)");

    expect_gate({{kBody, repl(kVesper, "owner.vesper_authority", "owner.mars_republic")}},
                "names undefined faction 'mars_republic' (#25a)");

    // (duplicate faction NAMES are already a schema gate in 8.7 — the
    // wrldvalidate dup check is defensive and unreachable through TOML.)
    expect_gate({{kSys, repl(kSystem, "stance.hostile.pirate_confederacy",
                             "stance.hostile.vesper_authority")}},
                "targets itself (#25b)");
    expect_gate({{kSys, repl(kSystem, "stance.hostile.pirate_confederacy",
                             "stance.hostile.mars_republic")}},
                "names undefined faction 'mars_republic' (#25b)");

    expect_gate({{kSys, repl(kSystem, "anchor = \"bodies/vesper.toml\"",
                             "anchor = \"bodies/orphan.toml\"")}},
                "anchor 'world/bodies/orphan.toml' is not a member of this system (#45d)");
    expect_gate({{kSys, repl(kSystem, "anchor = \"bodies/vesper.toml\"",
                             "anchor = \"poi:nope_pad\"")}},
                "POI anchor 'poi:nope_pad' is defined by no member body (#29c)");

    expect_gate({{kSys, repl(kSystem, "from = \"kepler_market\"", "from = \"ghost_market\"")}},
                "names no market node (#20b)");
    expect_gate({{kSys, repl(kSystem, "commodity = \"refined_ore\"\nflow",
                             "commodity = \"water\"\nflow")}},
                "produces no such commodity (#20b)");
    expect_gate({{kSys, repl(kSystem, "flow = 1.75", "flow = 0.0")}},
                "flow_rate must be > 0 (#20b)");

    expect_gate({{kSys, repl(kSystem, "body = \"bodies/vesper.toml\"",
                             "body = \"bodies/orphan.toml\"")}},
                "body 'world/bodies/orphan.toml' is not a member of this system (#27a)");
    expect_gate({{kSys, repl(kSystem, "from = \"wp_patrol_gate\"\nto = \"vesper_orbit_market\"",
                             "from = \"wp_patrol_gate\"\nto = \"wp_ghost\"")}},
                "to-endpoint names no market or waypoint node (#27a)");
    expect_gate({{kSys, repl(kSystem, "target = \"vesper_orbit_market\"",
                             "target = \"nowhere\"")}},
                "names no node, lane or station (#33a)");

    // two simultaneous system faults: faction gates run before lane gates —
    // identical inputs, identical first-error text.
    const std::string two_faults =
        repl(repl(kSystem, "stance.hostile.pirate_confederacy", "stance.hostile.self_ghost"),
             "from = \"kepler_market\"", "from = \"ghost_market\"");
    expect_gate({{kSys, two_faults}}, "stance tag 'stance.hostile.self_ghost' names undefined faction");

}

TEST_CASE("worlddef_validate_plans") {
    auto expect_gate = [&](std::vector<std::pair<std::string, std::string>> muts,
                           const char* needle) {
        const std::string err = gate_error(std::move(muts));
        CHECK_MESSAGE(err.find(needle) != std::string::npos,
                      "wanted '" << std::string{needle} << "', cook said: " << err);
    };

    // ---- station hull + build plans (#24a/#24b/#46d) ----
    expect_gate({{kKep, repl(kKepler, "root_socket = \"hp_engine_L\"",
                             "root_socket = \"door_1\"")}},
                "not a hardpoint on the station hull (#46d)");
    expect_gate({{kKep, repl(kKepler, "module = \"../../defs/ring_module.def.toml\"",
                             "module = \"../bodies/luna.toml\"")}},
                "is not a cooked hull def (#24b)");
    expect_gate({{kKep, repl(kKepler, "socket = \"hp_engine_R\"",
                             "socket = \"hp_dorsal_turret\"")}},
                "declares no clamp seam pairing socket 'hp_dorsal_turret'");
    expect_gate({{kKep, repl(kKepler, "at = \"0.5 day\"",
                             "at = \"0.5 day\"\n\n[[build_step]]\nplan = \"expansion_b\"\n"
                             "module = \"../../defs/ring_module_b.def.toml\"\n"
                             "socket = \"hp_engine_L\"\nat = \"2 day\"")}},
                "mounts a module on the station's own root socket");
    expect_gate({{kKep, repl(kKepler, "at = \"0.5 day\"",
                             "at = \"0.5 day\"\n\n[[build_step]]\nplan = \"expansion_c\"\n"
                             "module = \"../../defs/ring_module.def.toml\"\n"
                             "socket = \"hp_engine_R\"\nat = \"1 day\"")}},
                "two modules on socket 'hp_engine_R' (#24b)");
}