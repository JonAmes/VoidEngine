// 8.21 — station build plans, end-to-end (done-when (e)): a prefab module
// (#30 hull-with-interior unit, carried by the construction tender — its
// clamp seam rides the module itself, so the unit flies its own final
// approach) reaches a FREE station socket and clamps via the #23 dock
// machinery at the scheduled tick; the station's aggregate stats update
// that same tick; the replay hash matches a second identical run.
//
// Station def stays a plain loadout entity pinned to a rail (#24a): a
// root-pinned body + Loadout + StatsSet, exactly the 7.10 dock fixture
// shape. The schedule is 8.21.0's build_step_clamp_tick — the pinned
// clamp tick here IS what that function returns, tying schedule to scene.
// Tier 1: pure sim, hand-authored structs.

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/fnv.h>
#include <ve/core/sim/capture.h>
#include <ve/core/sim/dock.h>
#include <ve/core/sim/rails.h>
#include <ve/core/sim/replay.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_hash.h>
#include <ve/core/sim/time.h>
#include <ve/game/station_growth.h>
#include <ve/game/world_load.h>

namespace {

using namespace ve;
using namespace ve::game;
using namespace core::sim;
using ve::core::StatsSet;
using ve::core::stats_state_hash;

// ---- schedule fixture (same def data as the 8.21.0 unit test) ----------

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
        .steps = {WorldBuildStep{.module_def_ref = "ring_module",
                                 .target_socket = "hp_ring_A",
                                 .at_time_s = 43200.0}},
    };
}

// Independent re-run of the 8.21.0 delay formula (congestion 3 hand-counted
// from the fixture above): the scenario pins the exact clamp tick.
std::int64_t expected_delay(std::uint64_t seed, std::string_view plan_id,
                            std::uint64_t step_idx, std::uint64_t congestion) {
    core::Fnv1a64 h;
    h.mix_u64(seed);
    const std::string id(plan_id);
    h.mix_bytes(std::as_bytes(std::span(id)));
    h.mix_u64(step_idx);
    h.mix_u64(congestion);
    return static_cast<std::int64_t>((h.value() >> 32) %
                                     kMaxBuildDelayTicks);
}

// ---- station: rail-pinned hull + loadout + stats (#24a shape) ----------

rail_world station_world() {
    rail_world rw;
    body_def st;
    st.surface_radius = 100.0;  // 7.10 fixture idiom; static root, no spin
    const auto id = rw.add_body(std::move(st));
    REQUIRE(id.has_value());
    return rw;
}

dock_port_def ring_port(std::int32_t body) {
    return dock_port_def{
        .body = body,
        .local_pos = {0.0, 0.0, 110.0},          // +Z rim, hp_ring_A seam
        .local_normal = {0.0, 0.0, 1.0},
        .capture_radius_m = 100.0,
        .safe_speed_m_s = 2.0,
        .rest_depth_m = 0.5,
        .clamp_strength_N = 500'000.0,
    };
}

dock_gear_def module_gear() {
    return dock_gear_def{.local_pos = {0.0, -2.0, 0.0}};  // module seam gear
}

// Station loadout before growth: one comm array mounted; hp_ring_A free.
Loadout make_station_loadout() {
    hull_info hull;
    hull.class_tag = "class.station";
    hull.hull_mass = 1.0e6;
    hull.grid = grid_block{.cell = 1.0, .subcell_ratio = 1,
                           .cell_height = 1.0, .tolerance = 0.1};
    auto lo = Loadout::assemble(
        hull,
        std::vector<socket_desc>{
            socket_desc{.id = "hp_core_main",
                        .category = socket_category::main_},
            socket_desc{.id = "hp_ring_A", .category = socket_category::utility_},
        },
        std::vector<component_type>{
            component_type{.type_id = "comm.array",
                           .category = socket_category::main_},
        },
        std::vector<mount_request>{mount_request{
            .instance_id = "comm_arr",
            .type_id = "comm.array",
            .socket_id = "hp_core_main",
            .attrs = {{.key = "comms.range_m", .value = 5.0e6},
                      {.key = "power.draw", .value = 2000.0}},
            .tags = {"facet.operate.station"},
        }});
    REQUIRE(lo.has_value());
    return *lo;
}

StatsSet make_station_stats() {
    StatsSet stats;
    REQUIRE(stats.set_base("comms.range_m", 5.0e6f));
    REQUIRE(stats.set_base("power.draw", 2000.0f));
    REQUIRE(stats.set_base("hull.mass_kg", 1.0e6f));
    return stats;
}

prefab_module make_module() {
    prefab_module mod;
    mod.type_id = "hull.ring_module";
    mod.category = socket_category::utility_;
    mod.attrs = {{.key = "comms.range_m", .value = 2.0e6},  // += existing base
                 {.key = "mass", .value = 4800.0},          // new keys
                 {.key = "power.gen", .value = 500.0}};
    mod.tags = {"tag.ring"};
    return mod;
}

// One full scenario run: approach the ring port, clamp at the scheduled
// tick, land the build step (loadout grow + stats merge), then ride the
// clamp. Returns per-tick (rigid state x station stats) hash stamps;
// `merge` toggles the stats landing so the canary proves the stamp covers
// the aggregate update.
std::vector<std::uint64_t> run_scenario(std::uint64_t seed, bool merge,
                                        std::int64_t& clamp_tick_out) {
    const auto sys = make_system(seed);
    const auto stn = make_station();
    const auto plan = make_plan();

    // The clamp tick is whatever the 8.21.0 schedule says.
    const auto scheduled = build_step_clamp_tick(sys, stn, plan, 0);
    REQUIRE(scheduled.has_value());
    const tick_t T = *scheduled;
    clamp_tick_out = T;

    const rail_world rw = station_world();
    const dock_port_def port = ring_port(0);
    const dock_gear_def gear = module_gear();
    const WorldBuildStep step = plan.steps[0];
    const prefab_module mod = make_module();
    Loadout station = make_station_loadout();
    StatsSet stats = make_station_stats();

    // Final approach along the port normal, started 60 s (3600 ticks)
    // before the scheduled tick: 1.0 m/s (< safe 2.0). Geometry pins the
    // first swept sphere-crossing to tick T exactly: capture_engage probes
    // the segment [pos(t), pos(t) - v*dt] (descending), so pos(T) must sit
    // ABOVE the boundary by less than one tick of travel — tick T's segment
    // dips under it, tick T-1's segment starts 1.25 ticks' travel above
    // and stays fully outside.
    const double v = 1.0;
    const double eps = 0.25 / 60.0;  // quarter tick of travel above the rim
    const tick_t t0 = T - 3600;
    const tick_t t_end = T + 300;
    const double boundary_z = 110.0 + port.capture_radius_m + eps;  // z(T)

    rigid_state ship;
    ship.att = core::math::dquat{};  // identity
    bool docked = false;
    core::math::dquat clamp_att{};
    std::vector<std::uint64_t> stamps;
    std::uint64_t clamp_at = 0;

    for (tick_t t = t0; t <= t_end; ++t) {
        const auto pw = port_on_rail(rw, port, t);
        REQUIRE(pw.has_value());
        if (!docked) {
            // Analytic constant-velocity approach: bit-identical per run.
            ship.pos = {pw->pos.x, pw->pos.y,
                        boundary_z + v * static_cast<double>(T - t) *
                                               kSim_tick_dt};
            ship.vel = {0.0, 0.0, -v};
            const auto hit = capture_engage(port, *pw, ship.pos, ship.vel, t);
            if (t <= T - 1) {
                CHECK_FALSE(hit.has_value());  // no early clamp
            }
            if (hit.has_value()) {
                clamp_at = static_cast<std::uint64_t>(t);
                CHECK(t == T);  // clamp lands exactly on the scheduled tick
                ship = snap_to_dock(ship, *pw, port, gear);
                clamp_att = ship.att;  // host att identity (static root)
                docked = true;

                // --- the landing tick: loadout grows, stats update now.
                CHECK(station.mount_at_socket("hp_ring_A") == nullptr);
                auto grown = apply_build_step(station, step, mod,
                                              "ring_expansion.0");
                REQUIRE(grown.has_value());
                station = std::move(*grown);
                CHECK(station.mount_at_socket("hp_ring_A") != nullptr);
                CHECK(station.free_socket_count() == 0);
                if (merge) CHECK(merge_module_stats(stats, mod.attrs));
            }
            // Stats update on the landing tick only.
            CHECK(stats.size() == (t < T || !merge ? 3u : 5u));
        } else {
            // Rigid bind: exact pose per tick (the clamp), pair rides port.
            ship = docked_pose(ship, *pw, port, gear, clamp_att);
        }

        // Per-tick stamp: rigid state + the station's aggregate stats —
        // the replayed state includes the growth outcome.
        core::Fnv1a64 mix;
        mix.mix_u64(state_hash(t, ship));
        mix.mix_u64(stats_state_hash(stats));
        stamps.push_back(mix.value());
    }
    CHECK(clamp_at == static_cast<std::uint64_t>(T));
    return stamps;
}

}  // namespace

TEST_CASE("station_growth") {
    // ---- run A. ----
    std::int64_t T = 0;
    const auto stamps_a = run_scenario(0x5EED24BBull, true, T);

    // The scheduled tick is the 8.21.0 formula, pinned independently:
    // base 2,592,000 (0.5 day at 60 Hz) + the seeded congestion delay.
    CHECK(T == 2592000 + expected_delay(0x5EED24BBull, "ring_expansion", 0, 3));
    CHECK(T >= 2592000);
    CHECK(T < 2592000 + kMaxBuildDelayTicks);

    // ---- replay hash matches: identical re-run, bit-identical stamps. ----
    std::int64_t T_b = 0;
    const auto stamps_b = run_scenario(0x5EED24BBull, true, T_b);
    CHECK(T_b == T);
    REQUIRE(stamps_b.size() == stamps_a.size());
    CHECK(stamps_b == stamps_a);
    CHECK(replay_interval_hash(stamps_a, 0, stamps_a.size() - 1) ==
          replay_interval_hash(stamps_b, 0, stamps_b.size() - 1));

    // ---- 1-ULP sensitivity canary: a run WITHOUT the stats landing
    // diverges exactly after the clamp tick — the stamp covers the
    // aggregate update, not just the docked pose. ----
    std::int64_t T_c = 0;
    const auto stamps_c = run_scenario(0x5EED24BBull, false, T_c);
    REQUIRE(stamps_c.size() == stamps_a.size());
    const std::size_t idx_T = static_cast<std::size_t>(T - (T - 3600));
    CHECK(stamps_c[idx_T] != stamps_a[idx_T]);
    CHECK(stamps_c[idx_T - 1] == stamps_a[idx_T - 1]);  // identical before
    CHECK(replay_interval_hash(stamps_c, 0, stamps_c.size() - 1) !=
          replay_interval_hash(stamps_a, 0, stamps_a.size() - 1));

    // ---- seed sensitivity: a different world schedule clamps at a
    // different tick (the seeded lane delay is live in the scenario). ----
    std::int64_t T_d = 0;
    (void)run_scenario(0x5EED24BCull, true, T_d);
    CHECK(T_d != T);

    // ---- aggregate stats delta after the clamp, hand-computed from the
    // fixture (module attrs): comms.range_m 5e6 += 2e6 -> 7e6; new mass
    // 4800 and power.gen 500; untouched power.draw 2000, hull.mass_kg 1e6.
    // (Re-derived here at scenario level; run_scenario asserted size 3->5
    // at the landing tick.)
    StatsSet stats = make_station_stats();
    const auto mod = make_module();
    REQUIRE(merge_module_stats(stats, mod.attrs));
    CHECK(*stats.base("comms.range_m") == 7.0e6f);
    CHECK(*stats.base("power.draw") == 2000.0f);
    CHECK(*stats.base("mass") == 4800.0f);
    CHECK(*stats.base("power.gen") == 500.0f);
    CHECK(*stats.base("hull.mass_kg") == 1.0e6f);
    CHECK(stats.size() == 5);
    CHECK(stats_state_hash(stats) !=
          [] {
              StatsSet fresh = make_station_stats();
              return stats_state_hash(fresh);
          }());
}
