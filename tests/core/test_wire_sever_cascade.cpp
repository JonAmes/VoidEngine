// test: wire_sever_cascade — #51b done-when (g): hull hit -> section wire
// sever -> dark cascade -> repair from spares, ALL IN ONE REPLAY HASH, with
// the routes the designer drew surviving cook (9.5 editor -> 9.3 cook gates
// -> cooked section index consumed here: one chain end-to-end).
//
//   Case 1 (wire_section_facet_data): the loaded hull carries the COOK-
//     DERIVED section->wire index exactly as hand-computed from the fixture
//     routes x section boxes (bridge owns {w_engL, w_engR, w_fire_ctrl};
//     engine_bay owns {w_engL, w_engR}; every wire crosses midship), and
//     the #28a facet rows (panel_main names w_engL + w_engR as repair
//     targets).
//   Case 2 (wire_sever_cascade): sim network = the cooked wires verbatim;
//     the #46a mount pairing arrives through WiringReach::mount's
//     mount_socket ATTACH BOND, so severing a hardpoint-fed wire darkens
//     exactly the component bolted to that hardpoint. A hit on engine_bay
//     severs exactly the row's wires in ascending order; BOTH engines fall
//     dark (their only power path is cut) -> thrust 0 -> v_z bit-frozen on
//     the dark ticks (rail-pin idiom from 6.12). Facet/spare gates:
//     repairing w_engL at berth_1 -> no_facet; w_fire_ctrl at panel_main ->
//     no_facet; a live wire -> unknown_wire; unknown spare ->
//     unknown_spare; every refusal consumes NOTHING. Each legal splice
//     costs one spare; after w_engL's splice engine_R stays dark (shared
//     thrust attr still zeroed — partial repair visible); after w_engR's
//     splice the dark set is empty, the derived attr is bit-equal to
//     baseline, and stats_state_hash equals the pre-hit hash. One turret
//     wire (w_turret) is untouched by an engine_bay hit: turret_dorsal
//     never darkens. The full run repeats bit-identically (dark-set
//     hashes, v_z sequence, interval replay hash).
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../ve_test_portability.h"  // getpid

#include <ve/cook/cook.h>
#include <ve/core/cooked.h>
#include <ve/core/sim/cargo.h>
#include <ve/core/sim/loadout.h>
#include <ve/core/sim/loadout_load.h>
#include <ve/core/sim/replay.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/reservoir.h>  // kSim_dt
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_hash.h>
#include <ve/core/sim/stats_pipeline.h>
#include <ve/core/sim/wiring.h>

#ifndef VE_SOURCE_DIR
#error "VE_SOURCE_DIR must be defined by CMake"
#endif

namespace {

using namespace ve::core;
using namespace ve::core::sim;
using ve::core::math::dmat3;

std::string tmp_root(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_sever913_") + name + "_" + std::to_string(ve_test::pid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}

// Cook the fixture exactly like the 5.13 flight harness (#52a art model).
loaded_hull cook_and_load(const std::string& tag) {
    const std::string src = tmp_root("src_" + tag);
    const std::string out = tmp_root("out_" + tag);
    std::filesystem::create_directories(src + "/samples");
    std::filesystem::create_directories(src + "/defs");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb",
                          src + "/samples/test_ship.glb");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/defs/test_ship.def.toml",
                          src + "/defs/test_ship.def.toml");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/defs/comptypes",
                          src + "/defs/comptypes", std::filesystem::copy_options::recursive);
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/samples/component_gizmos.glb",
                          src + "/samples/component_gizmos.glb");
    auto cres = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(cres.ok, cres.error);
    std::string err;
    cooked::Unit def0, sock;
    REQUIRE(ve::cook::read_unit_file(out + "/units/defs/test_ship.def.toml.ck", def0, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/samples/test_ship.glb.sock.ck", sock, &err));
    auto loaded = load_hull_loadout(def0, sock, 0xA5EEDULL);
    REQUIRE_MESSAGE(loaded.has_value(), loaded.error());
    return *loaded;
}

// wire-table order: w_engL w_engR w_rcs w_sensor w_turret w_fire_ctrl
constexpr std::uint32_t kEngL = 0, kEngR = 1, kRcs = 2, kSensor = 3,
                        kTurret = 4, kFire = 5;

}  // namespace

TEST_CASE("wire_section_facet_data") {
    auto hull = cook_and_load("data");

    // ---- #51b section->wire rows (authored section order; hand-computed
    // from the fixture routes x section boxes, notes 9.13) ----
    REQUIRE(hull.section_wires.size() == 4);
    const std::vector<std::string> names = {"bridge", "midship", "engine_bay",
                                            "engineering"};
    const std::vector<std::vector<std::uint32_t>> rows = {
        {kEngL, kEngR, kFire},                          // bridge z=-2
        {kEngL, kEngR, kRcs, kSensor, kTurret, kFire},  // midship: every wire
        {kEngL, kEngR},                                 // engine_bay z=-3
        {kEngL, kEngR, kRcs, kSensor, kTurret},         // engineering z=1..3
    };
    for (std::size_t i = 0; i < names.size(); ++i) {
        CHECK(hull.section_wires[i].section == names[i]);
        CHECK(hull.section_wires[i].wire_indices == rows[i]);
        auto* q = wires_in_section(hull, names[i]);
        REQUIRE(q != nullptr);
        CHECK(*q == rows[i]);
    }
    CHECK(wires_in_section(hull, "no_such_section") == nullptr);

    // ---- #28a facet rows carried (interaction gate data) ----
    const component_facets* panel = nullptr;
    for (const auto& cf : hull.facets)
        if (cf.instance_id == "panel_main")
            panel = &cf;
    REQUIRE(panel != nullptr);
    REQUIRE(panel->facets.size() == 3);
    CHECK(panel->facets[0].action == "use.panel");
    CHECK(panel->facets[0].target == "");
    CHECK(panel->facets[1].action == "repair.target");
    CHECK(panel->facets[1].target == "w_engL");
    CHECK(panel->facets[2].action == "repair.target");
    CHECK(panel->facets[2].target == "w_engR");
    CHECK(panel->facets[1].range_m == doctest::Approx(1.2));  // "1.2 m" cooked
    CHECK(panel->facets[1].manual == false);                  // authored manual
}

TEST_CASE("wire_sever_cascade") {
    auto hull = cook_and_load("run");
    const Loadout& fixture = hull.loadout;

    struct run_out {
        std::vector<std::uint64_t> hashes;
        std::vector<std::string> dark_pins;
        std::vector<double> vz;
        std::uint64_t stats_pre = 0, stats_post = 0;
    };
    constexpr std::int64_t ticks = 18;
    auto run = [&](bool apply_damage) {
        StatsSet st;
        st.set_base("thrust.force", 1200.0f);
        st.set_base("rcs.force", 300.0f);
        st.set_base("gun.fire.turret", 1.0f);
        ModifierStack ms;
        StatsPipeline sp(st, ms);

        // Network = the COOKED wires verbatim. Mounts arrive with their
        // #46a socket pairing: the mount_socket attach bond is what ties a
        // hardpoint-fed wire to the part bolted there.
        std::vector<wire_entry> wires = hull.wires;
        std::vector<wired_component> comps = {
            {"reactor_1", "", "", false, false, true, false},
            {"battery_1", "", "", false, false, true, false},
            {"seat_pilot", "", "", false, false, false, true},
            {"engine_L", "thrust.force", "", true, false, false, false},
            {"engine_R", "thrust.force", "", true, false, false, false},
            {"rcs_wt", "rcs.force", "", true, false, false, false},
            {"turret_dorsal", "gun.fire.turret", "", true, true, false, false},
        };
        WiringReach net(sp, ms, std::move(wires), std::move(comps));
        // Sockets no wire names (reactor/battery/seat hardpoints here) =
        // legal no-op attach; those parts ride their own endpoint wires.
        const std::vector<std::pair<std::string_view, std::string_view>> mounts = {
            {"reactor_1", "hp_reactor"},   {"battery_1", "hp_battery"},
            {"seat_pilot", "hp_seat_pilot"},
            {"engine_L", "hp_engine_L"},   {"engine_R", "hp_engine_R"},
            {"rcs_wt", "hp_wingtip_R"},    {"turret_dorsal", "hp_dorsal_turret"},
        };
        for (const auto& [ref, sock] : mounts) REQUIRE(net.mount(ref, sock));
        // The attach bonds light every gated part: drop the mount hop from
        // flood() and this fails immediately, poisoning every ladder below.
        CHECK(net.dark_set().empty());
        CHECK(net.lit("engine_L"));
        CHECK(net.lit("engine_R"));
        CHECK(net.lit("rcs_wt"));
        CHECK(net.lit("turret_dorsal"));

        CargoManifest cargo(fixture, "cargo.hold", "cargo.capacity_m3", "cargo.gate.");
        REQUIRE(cargo.add_spare("spare_w1", "wire.spool").has_value());
        REQUIRE(cargo.add_spare("spare_w2", "wire.spool").has_value());

        // A legal-looking repair on a LIVE wire is refused (unknown_wire)
        // and costs nothing: repairs only splice severed wires.
        if (apply_damage) {
            CHECK(repair_wire_at_facet(net, cargo, hull, "repair.target",
                                       "panel_main", "w_engL", "spare_w1")
                      .error() == wire_repair_error::unknown_wire);
            CHECK(cargo.spares().size() == 2);
        }

        rigid_params p{};
        p.mass = 1000.0;
        p.inertia_tensor = dmat3::diag({100.0, 100.0, 100.0});
        ReplayLog log;
        rail_world w;
        rigid_state s{};

        run_out o;
        for (std::int64_t t = 0; t < ticks; ++t) {
            if (apply_damage && t == 7) {
                // =================== THE HULL HIT ===================
                o.stats_pre = stats_state_hash(st);  // pre-hit bit state
                auto cut = sever_wires_in_section(net, hull, "engine_bay");
                CHECK(cut == std::vector<std::string>{"w_engL", "w_engR"});
                // Second hit same section: already-severed counts as not-cut.
                CHECK(sever_wires_in_section(net, hull, "engine_bay").empty());
                CHECK(net.dark_set() ==
                      std::vector<std::string>{"engine_L", "engine_R"});
                // rcs/sensor/turret never used engine_bay cells: still lit.
                CHECK(net.lit("turret_dorsal"));
                CHECK(net.lit("rcs_wt"));
            }
            if (apply_damage && t == 9) {
                // Gate refusals consume nothing.
                CHECK(repair_wire_at_facet(net, cargo, hull, "repair.target",
                                           "berth_1", "w_engL", "spare_w1")
                          .error() == wire_repair_error::no_facet);
                CHECK(repair_wire_at_facet(net, cargo, hull, "repair.target",
                                           "panel_main", "w_fire_ctrl", "spare_w1")
                          .error() == wire_repair_error::no_facet);
                CHECK(repair_wire_at_facet(net, cargo, hull, "repair.target",
                                           "panel_main", "w_engL", "nope")
                          .error() == wire_repair_error::unknown_spare);
                CHECK(cargo.spares().size() == 2);
                // First legal splice: one engine back; the OTHER engine's
                // dark source still zeroes the shared attr -> still 0.
                CHECK(repair_wire_at_facet(net, cargo, hull, "repair.target",
                                           "panel_main", "w_engL", "spare_w1")
                          .has_value());
                CHECK(cargo.spares().size() == 1);
                CHECK(net.dark_set() == std::vector<std::string>{"engine_R"});
            }
            if (apply_damage && t == 12) {
                CHECK(repair_wire_at_facet(net, cargo, hull, "repair.target",
                                           "panel_main", "w_engR", "spare_w2")
                          .has_value());
                CHECK(cargo.spares().empty());
                CHECK(net.dark_set().empty());
            }

            // Glue reads the AGGREGATED function attr: dark engines => 0 N.
            const float f = *sp.derived("thrust.force");
            (void)*sp.derived("rcs.force");
            (void)*sp.derived("gun.fire.turret");  // warm for the state hash
            if (apply_damage && t == 17)
                o.stats_post = stats_state_hash(st);  // recompute-on-read:
                                                      // post-repair bits only
                                                      // visible after a read
            tick_command cmd{};
            cmd.thrust_body = {0.0, 0.0, -static_cast<double>(f)};  // body -Z
            log.append(cmd);
            replay_step(w, log, static_cast<std::size_t>(t), static_cast<tick_t>(t), p, s);
            o.hashes.push_back(state_hash(static_cast<tick_t>(t), s));
            o.vz.push_back(s.vel.z);
            std::string pin;
            for (const auto& d : net.dark_set()) { pin += d; pin += ','; }
            o.dark_pins.push_back(pin.empty() ? "-" : pin);
        }
        return o;
    };

    const auto dmg = run(true);
    const auto clean = run(false);

    // ---- Post-repair pipeline bits EXACTLY restore the pre-hit state
    // (#51b: sever+repair leaves no pipeline scar).
    CHECK(dmg.stats_post != 0);
    CHECK(dmg.stats_post == dmg.stats_pre);

    // ---- v_z law for the damaged run: thrusting steps 0..6, dark 7..11
    // (hit lands before step 7's read), thrusting 12+ (second splice lands
    // before step 12's read). kSim_dt = 1/60 is NOT dyadic, so the ladder
    // pins ride 6.12's Approx-epsilon idiom; only the frozen coast leg can
    // be exact (dark ticks add +0.0 -> v_z persists bit-identically).
    constexpr double kStep = 1200.0 * kSim_dt / 1000.0;
    for (std::int64_t t = 0; t < ticks; ++t) {
        const double thrusting = t <= 6 ? double(t + 1) : (t <= 11 ? 7.0 : double(t - 4));
        CHECK(dmg.vz[t] == doctest::Approx(-thrusting * kStep).epsilon(1e-12));
    }
    for (std::int64_t t = 7; t <= 11; ++t)
        CHECK(dmg.vz[t] == dmg.vz[6]);  // exact freeze, dark leg
    CHECK(clean.vz[17] == doctest::Approx(-18.0 * kStep).epsilon(1e-12));

    // ---- cascade pins are exactly the dark intervals ----
    CHECK(dmg.dark_pins[6] == "-");
    CHECK(dmg.dark_pins[7] == "engine_L,engine_R,");
    CHECK(dmg.dark_pins[8] == "engine_L,engine_R,");
    CHECK(dmg.dark_pins[9] == "engine_R,");   // after first splice
    CHECK(dmg.dark_pins[11] == "engine_R,");
    CHECK(dmg.dark_pins[12] == "-");          // both spliced before step 12
    CHECK(clean.dark_pins[17] == "-");

    // ---- (g): the whole damaged run is deterministic: rerun identical ----
    const auto dmg2 = run(true);
    CHECK(dmg2.hashes == dmg.hashes);
    CHECK(dmg2.vz == dmg.vz);
    CHECK(dmg2.dark_pins == dmg.dark_pins);
    CHECK(replay_interval_hash(dmg.hashes, 0, ticks - 1) ==
          replay_interval_hash(dmg2.hashes, 0, ticks - 1));
    CHECK(replay_interval_hash(dmg.hashes, 0, ticks - 1) !=
          replay_interval_hash(clean.hashes, 0, ticks - 1));
}
