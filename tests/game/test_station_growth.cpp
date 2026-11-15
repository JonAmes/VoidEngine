// 8.21.0 — station build-plan scheduler + mount application.
// Tier 1: pure logic, hand-authored WorldDef structs (same fixture idiom
// as the 7.10 docking test). Pins: the schedule formula (exact ticks from
// the authored timeline + the seeded lane delay), determinism, bounds,
// congestion sensitivity; and the landing: fresh-Loadout growth through
// core::assemble (refusals = first violation), stats merge hand-computed.

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/fnv.h>
#include <ve/core/sim/loadout.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/time.h>
#include <ve/game/station_growth.h>

namespace {

using namespace ve;
using namespace ve::game;

// Hand-authored system def: seed + the lane state around "kepler_b".
WorldDef make_system(std::uint64_t seed) {
    WorldDef sys;
    sys.world_class = WorldClass::system;
    sys.name = "kepler_system";
    sys.seed = seed;
    sys.waypoints = {
        WorldNavNode{.id = "wp_dock", .body_ref = "kepler_b"},
        WorldNavNode{.id = "wp_far", .body_ref = "other_b"},
    };
    sys.markets = {WorldMarketNode{.id = "mk_kepler", .anchor_ref = "kepler_b",
                                   .commodities = {}}};
    sys.spawns = {
        WorldSpawn{.id = "sp_a", .at = "wp_dock", .faction = "pirate",
                   .ship_class = "Corsair", .rate = 1.0e-4},
        WorldSpawn{.id = "sp_b", .at = "wp_dock", .faction = "pirate",
                   .ship_class = "Corsair", .rate = 1.0e-4},
        WorldSpawn{.id = "sp_c", .at = "mk_kepler", .faction = "hanse",
                   .ship_class = "Freighter", .rate = 2.0e-4},
        WorldSpawn{.id = "sp_d", .at = "wp_far", .faction = "hanse",
                   .ship_class = "pilot", .rate = 1.0e-5},
    };
    return sys;
}

// Station def pinned to kepler_b (the body three spawn-row nodes anchor on).
WorldDef make_station() {
    WorldDef st;
    st.world_class = WorldClass::station;
    st.name = "newhaven";
    st.parent_ref = "kepler_b";
    return st;
}

WorldBuildPlan make_plan() {
    return WorldBuildPlan{
        .id = "ring_expansion",
        .steps = {
            WorldBuildStep{.module_def_ref = "ring_module",
                           .target_socket = "hp_ring_A",
                           .at_time_s = 43200.0},   // "0.5 day"
            WorldBuildStep{.module_def_ref = "hub_module",
                           .target_socket = "hp_ring_B",
                           .at_time_s = 129600.0},  // "1.5 day"
        },
    };
}

// Independent re-run of the header's documented mixing order: seed,
// plan.id bytes, step_idx, congestion; delay = (h >> 32) % bound.
// The test's congestion literal (3) is hand-counted from the fixture.
std::int64_t expected_delay(std::uint64_t seed, std::string_view plan_id,
                            std::uint64_t step_idx, std::uint64_t congestion) {
    core::Fnv1a64 h;
    h.mix_u64(seed);
    const std::string id(plan_id);
    h.mix_bytes(std::as_bytes(std::span(id)));
    h.mix_u64(step_idx);
    h.mix_u64(congestion);
    return static_cast<std::int64_t>((h.value() >> 32) %
                                     static_cast<std::uint64_t>(
                                         kMaxBuildDelayTicks));
}

core::sim::tick_t base(std::double_t at_time_s) {
    return static_cast<core::sim::tick_t>(
        std::ceil(at_time_s * core::sim::kSim_tick_hz));
}

}  // namespace

TEST_CASE("station_growth_schedule") {
    const auto sys = make_system(0x5EED24BBull);
    const auto station = make_station();
    const auto plan = make_plan();

    // ---- exact pin: base tick (0.5 day -> 2,592,000 ticks) + the seeded
    // lane delay recomputed in-test (pins the mixing order AND congestion).
    const auto t0 = build_step_clamp_tick(sys, station, plan, 0);
    REQUIRE(t0.has_value());
    CHECK(*t0 == base(43200.0) + expected_delay(0x5EED24BBull, "ring_expansion",
                                                0, 3));
    CHECK(base(43200.0) == 2592000);  // hand pin: 0.5 day at 60 Hz

    const auto t1 = build_step_clamp_tick(sys, station, plan, 1);
    REQUIRE(t1.has_value());
    CHECK(*t1 == base(129600.0) + expected_delay(0x5EED24BBull, "ring_expansion",
                                                 1, 3));
    CHECK(*t1 > *t0);  // steps stay ordered (bases 5.18M ticks apart >> bound)

    // ---- out of range step.
    CHECK_FALSE(build_step_clamp_tick(sys, station, plan, 2).has_value());

    // ---- determinism: same inputs, bit-identical ticks (twice).
    CHECK(build_step_clamp_tick(sys, station, plan, 0) == t0);

    // ---- bounds over a seed sweep: delay in [0, 3600) ticks (1 minute).
    for (std::uint64_t seed = 0; seed < 64; ++seed) {
        const auto s = make_system(seed);
        const auto t = build_step_clamp_tick(s, station, plan, 0);
        REQUIRE(t.has_value());
        CHECK(*t >= base(43200.0));
        CHECK(*t < base(43200.0) + kMaxBuildDelayTicks);
    }

    // ---- seeded-RNG sensitivity: a different world seed shifts the delay
    // (checked via the independent re-run, so the wiring is pinned).
    const auto sys2 = make_system(0x5EED24BCull);
    const auto t0b = build_step_clamp_tick(sys2, station, plan, 0);
    REQUIRE(t0b.has_value());
    CHECK(*t0b == base(43200.0) + expected_delay(0x5EED24BCull, "ring_expansion",
                                                 0, 3));
    CHECK(*t0b != *t0);

    // ---- lane-state sensitivity: quieting the station's neighborhood
    // (spawn rows move off kepler_b) changes the delay.
    auto quiet = sys;
    for (auto& sp : quiet.spawns) sp.at = "wp_far";
    const auto t0q = build_step_clamp_tick(quiet, station, plan, 0);
    REQUIRE(t0q.has_value());
    CHECK(*t0q == base(43200.0) + expected_delay(0x5EED24BBull, "ring_expansion",
                                                 0, 0));
    CHECK(*t0q != *t0);

    // ---- congestion counts rows, not distinct nodes: doubling one row's
    // nodes changes the draw (sp_a row duplicated).
    auto doubled = sys;
    doubled.spawns.push_back(WorldSpawn{.id = "sp_e", .at = "wp_dock",
                                        .faction = "pirate",
                                        .ship_class = "Corsair",
                                        .rate = 1.0e-4});
    const auto t0d = build_step_clamp_tick(doubled, station, plan, 0);
    REQUIRE(t0d.has_value());
    CHECK(*t0d == base(43200.0) + expected_delay(0x5EED24BBull, "ring_expansion",
                                                 0, 4));
}

TEST_CASE("station_growth_mount") {
    using namespace core::sim;

    // Station loadout: 3 sockets, one mount shipped (a comm array on the
    // main socket). hp_ring_* are the free prefab-module sockets.
    hull_info hull;
    hull.class_tag = "class.station";
    hull.hull_mass = 1.0e6;
    hull.grid = grid_block{.cell = 1.0, .subcell_ratio = 1,
                           .cell_height = 1.0, .tolerance = 0.1};
    std::vector<socket_desc> sockets = {
        socket_desc{.id = "hp_core_main", .category = socket_category::main_},
        socket_desc{.id = "hp_ring_A", .category = socket_category::utility_},
        socket_desc{.id = "hp_ring_B", .category = socket_category::utility_},
    };
    std::vector<component_type> catalog = {
        component_type{.type_id = "comm.array",
                       .category = socket_category::main_},
    };
    std::vector<mount_request> mounts = {
        mount_request{
            .instance_id = "comm_arr",
            .type_id = "comm.array",
            .socket_id = "hp_core_main",
            .attrs = {{.key = "comms.range_m", .value = 5.0e6},
                      {.key = "power.draw", .value = 2000.0}},
            .tags = {"facet.operate.station"},
        },
    };
    auto st0 = Loadout::assemble(hull, sockets, catalog, mounts);
    REQUIRE(st0.has_value());
    CHECK(st0->free_socket_count() == 2);

    // The prefab module (#30 hull-with-interior unit distilled to one mount).
    prefab_module mod;
    mod.type_id = "hull.ring_module";
    mod.category = socket_category::utility_;
    mod.attrs = {{.key = "mass", .value = 4800.0},
                 {.key = "power.gen", .value = 500.0},
                 {.key = "comms.range_m", .value = 2.0e6}};
    mod.tags = {"tag.ring"};
    WorldBuildStep step{.module_def_ref = "ring_module",
                        .target_socket = "hp_ring_A",
                        .at_time_s = 43200.0};

    // ---- landing grows the loadout through the real assembler.
    auto st1 = apply_build_step(*st0, step, mod, "ring_expansion.0");
    REQUIRE(st1.has_value());
    CHECK(st1->mounts().size() == 2);
    CHECK(st1->free_socket_count() == 1);
    const auto* m = st1->mount_at_socket("hp_ring_A");
    REQUIRE(m != nullptr);
    CHECK(m->type_id == "hull.ring_module");
    CHECK(m->instance_id == "ring_expansion.0");
    CHECK(m->category == socket_category::utility_);
    REQUIRE(m->tags.size() == 1);
    CHECK(m->tags[0] == "tag.ring");
    // Existing mount rode through verbatim.
    const auto* comm = st1->mount("comm_arr");
    REQUIRE(comm != nullptr);
    REQUIRE(comm->attrs.size() == 2);
    for (std::size_t i = 0; i < comm->attrs.size(); ++i) {
        CHECK(comm->attrs[i].key == mounts[0].attrs[i].key);
        CHECK(comm->attrs[i].value == mounts[0].attrs[i].value);
    }
    CHECK(comm->tags == mounts[0].tags);

    // ---- aggregate stats update on the landing tick, hand-computed
    // (all pins exact fp32: the fixture values are small integers).
    core::StatsSet stats;
    REQUIRE(stats.set_base("comms.range_m", 5.0e6f));
    REQUIRE(stats.set_base("power.draw", 2000.0f));
    REQUIRE(stats.set_base("hull.mass_kg", 1.0e6f));
    const auto rev_before = stats.revision();
    CHECK(merge_module_stats(stats, mod.attrs));
    // Exact float== pins (no Approx): every value is a small integer or a
    // dyadic product, so fp32 holds them bit-exactly — the merge order
    // must never perturb them.
    CHECK(*stats.base("power.draw") == 2000.0f);   // untouched: not in the module
    CHECK(*stats.base("comms.range_m") == 7.0e6f); // 5e6 base += 2e6 module
    CHECK(*stats.base("mass") == 4800.0f);         // new key
    CHECK(*stats.base("power.gen") == 500.0f);     // new key
    CHECK(*stats.base("hull.mass_kg") == 1.0e6f);  // untouched
    CHECK(stats.size() == 5);
    CHECK(stats.revision() > rev_before);
    // Insertion order: existing keys kept position, new keys appended.
    REQUIRE(stats.entries().size() == 5);
    CHECK(stats.entries()[0].key == "comms.range_m");
    CHECK(stats.entries()[3].key == "mass");
    CHECK(stats.entries()[4].key == "power.gen");

    // ---- refusals: first violation, loadout untouched (st0 stays).
    auto again = apply_build_step(*st0, step, mod, "ring_expansion.1");
    // Occupied socket: same socket, still mounted...
    auto occ = apply_build_step(*st1, step, mod, "ring_expansion.1");
    REQUIRE_FALSE(occ.has_value());
    CHECK(occ.error() == "loadout: socket_occupied");

    auto unk = apply_build_step(
        *st0,
        WorldBuildStep{.module_def_ref = "ring_module",
                       .target_socket = "hp_ring_Z", .at_time_s = 1.0},
        mod, "x");
    REQUIRE_FALSE(unk.has_value());
    CHECK(unk.error() == "loadout: unknown_socket");

    auto mismatch = mod;
    mismatch.category = socket_category::main_;
    auto bad = apply_build_step(*st0, step, mismatch, "x");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == "loadout: category_mismatch");

    auto dup = apply_build_step(*st0, step, mod, "comm_arr");
    REQUIRE_FALSE(dup.has_value());
    CHECK(dup.error() == "loadout: duplicate_instance_id");

    // st0 is const + refusals return errors: the live loadout never moved.
    CHECK(st0->mounts().size() == 1);
    CHECK(st0->mount_at_socket("hp_ring_A") == nullptr);

    // ---- merge finiteness: NaN poisons nothing (all-or-nothing).
    prefab_module poison;
    poison.attrs = {{.key = "mass", .value = 100.0},
                    {.key = "power.gen",
                     .value = std::numeric_limits<double>::quiet_NaN()}};
    CHECK_FALSE(merge_module_stats(stats, poison.attrs));
    CHECK(*stats.base("mass") == 4800.0f);
    CHECK(stats.size() == 5);
}
