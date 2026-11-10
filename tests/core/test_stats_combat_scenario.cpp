// test: stats_combat_scenario — 6.17 done-when (h): a scripted ~10-minute
// combat scenario (36,000 fixed ticks at 1/60) replays to an IDENTICAL
// stats-state hash stream, and the 6.15 scrubber API steps that replay to
// any tick with a hash matching the live run bit-exact. Composition only:
// NO new core code. One tick's loop wires every Phase-6 subsystem:
//   expire_due -> scripted events -> power rebalance -> empty-tank gate ->
//   propellant glue (load = force/Isp) -> reservoir step -> market step +
//   stock/price attrs -> reputation bands -> read-all -> stats hash ->
//   flight command + replay_step + sim hash.
// Every number is hand-computed from dyadic rates (the Phase-6 pinning
// style): force 1200 N / Isp 20 -> 1.0 unit/tick; lane +60/s -> +1/tick.
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/fnv.h>
#include <ve/core/sim/assist.h>
#include <ve/core/sim/curve.h>
#include <ve/core/sim/effects.h>
#include <ve/core/sim/loadout.h>
#include <ve/core/sim/modifiers.h>
#include <ve/core/sim/occupancy.h>
#include <ve/core/sim/power.h>
#include <ve/core/sim/replay.h>
#include <ve/core/sim/reservoir.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/scrubber.h>
#include <ve/core/sim/seat_link.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_hash.h>
#include <ve/core/sim/stats_pipeline.h>
#include <ve/core/sim/tags.h>
#include <ve/core/sim/wiring.h>

namespace {
using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;

constexpr std::int64_t kTotal = 36000;  // 10 minutes at 1/60
constexpr std::int64_t kCp = 3600;      // checkpoint every 60 s

// --- ship (same analytic body as the 6.8 glue) --------------------------
constexpr double kMass = 50000.0;
constexpr double kIy = 8.0e5;
constexpr float kForce = 1200.0f;  // ship.thrust.force base (N)
constexpr float kIsp = 20.0f;      // effective exhaust velocity -> 60/s = 1/tick
constexpr double kStabBase = 1.0e5;
constexpr double kSpinT = 1.6e6;  // evade-spin torque; domega/tick = 1/30

// --- scripted event ticks (all hand-computed against the streams) -------
constexpr std::int64_t kThrustHit = 1200;   // thruster pod: force x0.5
constexpr std::int64_t kThrustRepaired = 2400;  // remove_source -> exact baseline
constexpr std::int64_t kBusKilled = 4800;   // wiring cascade dark set
constexpr std::int64_t kBusRevived = 5400;
constexpr std::int64_t kOverload = 6000;    // shield draw +60, expires 6600
constexpr std::int64_t kOverloadGone = 6600;
constexpr std::int64_t kGunnerKO = 10800;
constexpr std::int64_t kGunnerBack = 11400;
constexpr std::int64_t kSpin1a = 12000, kSpin1b = 12600;
constexpr std::int64_t kRefill = 9000;      // propellant market refill
constexpr std::int64_t kSell = 18000;       // market transaction (+600 units)
constexpr std::int64_t kRepNudge = 18001;   // reputation effect -> wary tag
constexpr std::int64_t kSpin2a = 29400, kSpin2b = 30000;
constexpr std::int64_t kPilotKO = 30000;    // stab zeroed -> omega freezes
constexpr std::int64_t kPilotBack = 31200;
constexpr std::int64_t kWireSevered = 24000;  // engines-only dark
constexpr std::int64_t kWireRepaired = 24600;

// Dry pin: draw = 1200 + 600 + 2400 (A) + 0 (blackout) + 3600 (B) = 7800
// -> value 4200 at 8999; refill -> 12000; 12000 ticks of 1.0 -> value 0.0
// exactly after tick 9000 + 11999 = 20999.
constexpr std::int64_t kDryTick = 20999;

// Reserved source ids (far from seat/power/wiring ranges).
constexpr std::uint64_t kBurnSrc = 200;   // glue: rewrites propellant.load
constexpr std::uint64_t kLaneSrc = 201;   // market lane supply +60/s
constexpr std::uint64_t kEmptySrc = 202;  // empty-tank x0 on thrust
constexpr std::uint64_t kFxThrustHit = 300;
constexpr std::uint64_t kFxOverload = 301;
constexpr std::uint64_t kFxGunnerKO = 302;
constexpr std::uint64_t kFxPilotKO = 303;
constexpr std::uint64_t kFxRepNudge = 304;

Loadout ship_loadout() {
    hull_info h;
    h.class_tag = "class.poc";
    h.grid = grid_block{2.0, 4, 2.5, 0.05};
    h.hull_mass = kMass;
    std::vector<socket_desc> socks = {
        {"hp_seat_pilot", socket_category::utility_, {}, {}},
        {"hp_seat_gunner", socket_category::utility_, {}, {}},
    };
    std::vector<component_type> cat = {{"seat.station", socket_category::utility_}};
    std::vector<mount_request> mounts = {
        {"seat_pilot", "seat.station", "hp_seat_pilot", {}, {"station.pilot"}},
        {"seat_gunner", "seat.station", "hp_seat_gunner", {}, {"station.gunner"}},
    };
    auto r = Loadout::assemble(std::move(h), std::move(socks), std::move(cat),
                               std::move(mounts));
    REQUIRE(r.has_value());
    return std::move(*r);
}

std::vector<wire_entry> ship_wires() {
    auto comp = [](std::string_view ref) { return wire_endpoint{1, std::string(ref)}; };
    return {
        {"w_reactor_bus", wire_kind::power, {}, comp("reactor"), comp("bus")},
        {"w_bus_engines", wire_kind::power, {}, comp("bus"), comp("engines")},
        {"w_bus_gun", wire_kind::power, {}, comp("bus"), comp("gun")},
        {"w_firectrl_gun", wire_kind::signal, {}, comp("firectrl"), comp("gun")},
    };
}

std::vector<wired_component> ship_components() {
    return {
        {"reactor", "", "", false, false, true, false},
        {"bus", "", "", false, false, false, false},
        {"engines", "ship.thrust.force", "engines.draw", true, false, false, false},
        {"gun", "func.gun", "", true, true, false, false},
        // firectrl EMITS signal (gate_power false: it has no inbound power
        // wire; it goes dark only if killed).
        {"firectrl", "", "", false, false, false, true},
    };
}

// One per-tick observation of everything the scenario mutates (u8 flags so
// whole-run streams compare byte-wise across runs).
struct marker {
    std::uint8_t eng_lit = 0;     // wiring lit("engines")
    std::uint8_t gun_lit = 0;     // wiring lit("gun")
    std::uint8_t sys_gun = 0;     // power pool lit("sys.gun")
    std::uint8_t func_gun = 0;    // aggregated func.gun > 0 (manning/power/wiring)
    std::uint8_t thrust_pos = 0;  // aggregated thrust force > 0
    std::uint8_t prop_empty = 0;  // propellant.empty band tag
    std::uint8_t wary = 0;        // faction.hauler.wary band tag
    std::uint8_t pilot_in = 0, gunner_in = 0;
    std::uint8_t reflex = 0, aim = 0;  // pilot/gunner derived > 0
    friend bool operator==(const marker&, const marker&) = default;
};

struct run_result {
    ReplayLog log;
    std::vector<sim_checkpoint> cps;
    std::vector<std::uint64_t> sim_hash;    // state_hash per tick
    std::vector<std::uint64_t> stats_hash;  // folded canonical stats hash
    std::vector<marker> mk;
    std::vector<double> prop_val;           // reservoir state per tick
    std::vector<double> market_val;
    std::vector<double> omega_y;
    std::uint64_t stats_at_end = 0;
    float price_at_end = 0.0f;  // the re-derived price attr at the ceiling
};

// Fold one u64 per StatsSet, FIXED registration order: ship, pilot, gunner,
// faction. Same FNV primitive as the replay hash (6.5).
std::uint64_t fold_stats(const StatsSet& ship, const StatsSet& pilot, const StatsSet& gunner,
                         const StatsSet& faction) {
    Fnv1a64 f;
    f.mix_u64(stats_state_hash(ship));
    f.mix_u64(stats_state_hash(pilot));
    f.mix_u64(stats_state_hash(gunner));
    f.mix_u64(stats_state_hash(faction));
    return f.value();
}

// Run the whole scripted scenario. `no_pilot_ko` = variant where the KO at
// 30000 never happens (proves the KO measurably changes BOTH streams).
run_result run_scenario(bool no_pilot_ko = false) {
    run_result R{};
    const Loadout lo = ship_loadout();
    const std::vector<std::string> reg = {"station.pilot", "station.gunner"};
    Occupancy occ(lo, reg);

    // --- stats / stacks / pipelines (insertion order = hash order) ------
    StatsSet ship_stats, pilot_stats, gunner_stats, faction_stats;
    // Author every flow/modifier target attr with a base entry FIRST
    // (the 6.13 lesson: derived on an unauthored attr fails closed).
    ship_stats.set_base("ship.thrust.force", kForce);
    ship_stats.set_base("ship.isp", kIsp);
    ship_stats.set_base("ship.assist.stab_kd", float(kStabBase));
    ship_stats.set_base("power.supply", 300.0f);
    ship_stats.set_base("power.draw.life", 100.0f);
    ship_stats.set_base("power.draw.shield", 120.0f);
    ship_stats.set_base("power.draw.gun", 80.0f);
    ship_stats.set_base("power.prio.life", 30.0f);
    ship_stats.set_base("power.prio.shield", 20.0f);
    ship_stats.set_base("power.prio.gun", 10.0f);
    ship_stats.set_base("func.life", 1.0f);
    ship_stats.set_base("func.shield", 1.0f);
    ship_stats.set_base("func.gun", 1.0f);
    ship_stats.set_base("engines.draw", 0.0f);
    ship_stats.set_base("propellant.supply", 0.0f);
    ship_stats.set_base("propellant.load", 0.0f);
    ship_stats.set_base("propellant.leak", 0.0f);
    ship_stats.set_base("market.supply", 0.0f);
    ship_stats.set_base("market.load", 0.0f);
    ship_stats.set_base("market.leak", 0.0f);
    ship_stats.set_base("market.stock", 1000.0f);
    ship_stats.set_base("market.price", 0.0f);
    pilot_stats.set_base("pilot.reflex", 1.0f);
    gunner_stats.set_base("gunner.aim", 1.0f);
    faction_stats.set_base("rep.hauler", 0.0f);

    ModifierStack ship_stack, pilot_stack, gunner_stack, faction_stack;
    StatsPipeline ship_pipe(ship_stats, ship_stack);
    StatsPipeline pilot_pipe(pilot_stats, pilot_stack);
    StatsPipeline gunner_pipe(gunner_stats, gunner_stack);
    StatsPipeline faction_pipe(faction_stats, faction_stack);

    REQUIRE(ship_stack.add_source(kBurnSrc));
    REQUIRE(ship_stack.add_source(kLaneSrc));
    {  // lane traffic: +60/s forever -> +1 unit/tick
        auto m = modifier_desc{};
        m.target_attr = "market.supply";
        m.stack_id = "lane.traffic";
        m.op = mod_op::flat_add;
        m.value = 60.0f;
        m.policy = stack_policy::replace;
        REQUIRE(ship_stack.add_modifier(kLaneSrc, m) == mod_add_result::added);
    }

    // --- reservoirs (6.9): propellant + market stock ---------------------
    TagSet ship_tags, faction_tags;
    Reservoir prop(12000.0, {{0.0, "propellant.empty", band_dir::at_or_below}}, &ship_tags);
    prop.set_ceiling(12000.0);
    prop.bind(&ship_pipe, {"propellant.supply", "propellant.load", "propellant.leak"});
    Reservoir market(1000.0, {}, &ship_tags);
    market.set_ceiling(20000.0);
    market.bind(&ship_pipe, {"market.supply", "market.load", "market.leak"});
    piecewise_curve price_curve({{0.0, 10.0}, {1000.0, 4.0}, {20000.0, 0.5}});
    const std::vector<reservoir_band> rep_bands = {
        {10.0, "faction.hauler.wary", band_dir::at_or_above}};

    // --- power (6.10) + wiring (6.12) on the SAME stack, disjoint sources
    PowerPool pool(ship_pipe, ship_stack, "power.supply",
                   {{"sys.life", "power.draw.life", "func.life", "power.prio.life"},
                    {"sys.shield", "power.draw.shield", "func.shield", "power.prio.shield"},
                    {"sys.gun", "power.draw.gun", "func.gun", "power.prio.gun"}});
    WiringReach wiring(ship_pipe, ship_stack, ship_wires(), ship_components());
    for (const auto& c : {"reactor", "bus", "engines", "gun", "firectrl"})
        REQUIRE(wiring.mount(c));

    // --- crew: seats, affordances, arming (6.7; 6.8 pattern) -------------
    EffectEventBus bus;
    SeatLink link(occ, ship_stack, &bus);
    {
        seat_affordance pilot_aff;
        pilot_aff.role_tag = "station.pilot";
        auto grant = modifier_desc{};
        grant.target_attr = "pilot.reflex";
        grant.stack_id = "seat.pilot_reflex";
        grant.op = mod_op::multiplier;
        grant.value = 1.25f;
        grant.policy = stack_policy::exclusive;
        pilot_aff.occupant_grant = {grant};
        auto zero = modifier_desc{};
        zero.target_attr = "ship.assist.stab_kd";
        zero.stack_id = "seat.pilot_zero";
        zero.op = mod_op::multiplier;
        zero.value = 0.0f;
        zero.policy = stack_policy::exclusive;
        pilot_aff.when_unmanned = {zero};
        REQUIRE(link.add_affordance(pilot_aff));

        seat_affordance gunner_aff;
        gunner_aff.role_tag = "station.gunner";
        grant = modifier_desc{};
        grant.target_attr = "gunner.aim";
        grant.stack_id = "seat.gunner_aim";
        grant.op = mod_op::multiplier;
        grant.value = 1.25f;
        grant.policy = stack_policy::exclusive;
        gunner_aff.occupant_grant = {grant};
        zero = modifier_desc{};
        zero.target_attr = "func.gun";
        zero.stack_id = "seat.gun_zero";
        zero.op = mod_op::multiplier;
        zero.value = 0.0f;
        zero.policy = stack_policy::exclusive;
        gunner_aff.when_unmanned = {zero};
        REQUIRE(link.add_affordance(gunner_aff));
    }
    // Arm the unmanned zeroing through the unit machinery, then crew up.
    REQUIRE(link.sit(1, "seat_pilot", pilot_stack).has_value());
    REQUIRE(link.stand(1, "seat_pilot", pilot_stack).has_value());
    REQUIRE(link.sit(2, "seat_gunner", gunner_stack).has_value());
    REQUIRE(link.stand(2, "seat_gunner", gunner_stack).has_value());
    REQUIRE(link.sit(1, "seat_pilot", pilot_stack).has_value());
    REQUIRE(link.sit(2, "seat_gunner", gunner_stack).has_value());

    // --- physics (Phase 4) -----------------------------------------------
    rail_world w;
    rigid_params p{};
    p.mass = kMass;
    p.inertia_tensor = dmat3::diag({2.0e5, kIy, 7.0e5});
    rigid_state s{};
    R.cps.push_back({tick_t{-1}, s});

    // Empty-tank gate: x0 on thrust stays armed WHILE the band tag is set
    // (dark-not-dead), synced to the tag rather than churned per tick —
    // the force seen by the load attr, the flight cmd, and the markers is
    // always the gated one.
    bool empty_armed = false;
    auto sync_empty_gate = [&] {
        const bool dry = ship_tags.contains("propellant.empty");
        if (dry == empty_armed)
            return;
        if (dry) {
            REQUIRE(ship_stack.add_source(kEmptySrc));
            auto m = modifier_desc{};
            m.target_attr = "ship.thrust.force";
            m.stack_id = "empty.zero";
            m.op = mod_op::multiplier;
            m.value = 0.0f;
            m.policy = stack_policy::exclusive;
            REQUIRE(ship_stack.add_modifier(kEmptySrc, m) == mod_add_result::added);
        } else {
            REQUIRE(ship_stack.remove_source(kEmptySrc));
        }
        empty_armed = dry;
    };

    for (std::int64_t t = 0; t < kTotal; ++t) {
        // 1. expiry first: the +60 shield draw drops at kOverloadGone.
        ship_stack.expire_due(std::uint64_t(t));
        gunner_stack.expire_due(std::uint64_t(t));
        pilot_stack.expire_due(std::uint64_t(t));

        // 2. scripted events for THIS tick (all mutate stats BEFORE steps).
        if (t == kThrustHit) {  // thruster pod hit: force x0.5 until repaired
            effect_desc fx{};
            fx.effect_id = "hit.thruster_pod";
            auto m = modifier_desc{};
            m.target_attr = "ship.thrust.force";
            m.stack_id = "hit.pod_mult";
            m.op = mod_op::multiplier;
            m.value = 0.5f;
            m.policy = stack_policy::stack_n;
            m.stack_cap = 1;
            fx.bundle = {m};
            REQUIRE(apply_effect(ship_stack, kFxThrustHit, fx) == effect_result::applied);
        }
        if (t == kThrustRepaired)
            REQUIRE(ship_stack.remove_source(kFxThrustHit));
        if (t == kBusKilled) {
            REQUIRE(wiring.kill_component("bus"));
            // bus is a pure relay (ungated, not a source): killing it stops
            // relaying but reports no dark member itself — the gate-fed
            // consumers cascade into the dark set (6.12 dark-set formula).
            // reactor (source) and firectrl (ungated signal source) stay lit.
            CHECK(wiring.dark_set() == std::vector<std::string>{"engines", "gun"});
        }
        if (t == kBusRevived) {
            REQUIRE(wiring.revive_component("bus"));
            CHECK(wiring.dark_set().empty());
        }
        if (t == kOverload) {  // shield draw +60 -> shed lowest priority (gun)
            effect_desc fx{};
            fx.effect_id = "surge.shield_draw";
            fx.expire_tick = std::uint64_t(kOverloadGone);
            auto m = modifier_desc{};
            m.target_attr = "power.draw.shield";
            m.stack_id = "surge.draw";
            m.op = mod_op::flat_add;
            m.value = 60.0f;
            m.policy = stack_policy::stack_n;
            m.stack_cap = 1;
            fx.bundle = {m};
            REQUIRE(apply_effect(ship_stack, kFxOverload, fx) == effect_result::applied);
        }
        if (t == kWireSevered) {
            REQUIRE(wiring.sever_wire("w_bus_engines"));
            CHECK(wiring.dark_set() == std::vector<std::string>{"engines"});
        }
        if (t == kWireRepaired) {
            REQUIRE(wiring.repair_wire("w_bus_engines"));
            CHECK(wiring.dark_set().empty());
        }
        if (t == kGunnerKO) {  // KO: drop + recovery flat on the gunner stack
            REQUIRE(link.drop(2, "seat_gunner", gunner_stack).has_value());
            effect_desc fx{};
            fx.effect_id = "ko.gunner";
            fx.expire_tick = std::uint64_t(kGunnerBack);
            auto m = modifier_desc{};
            m.target_attr = "gunner.aim";
            m.stack_id = "ko.recovery";
            m.op = mod_op::flat_add;
            m.value = -1.0f;
            m.policy = stack_policy::stack_n;
            m.stack_cap = 1;
            fx.bundle = {m};
            REQUIRE(apply_effect(gunner_stack, kFxGunnerKO, fx) == effect_result::applied);
        }
        if (t == kGunnerBack)
            REQUIRE(link.sit(2, "seat_gunner", gunner_stack).has_value());
        if (t == kRefill)
            prop.set_value(12000.0);  // market refill through the authoring door
        if (t == kSell)              // market transaction: exact +600 units
            market.set_value(market.value() + 600.0);
        if (t == kRepNudge) {  // selling to a hostile contact: rep +15
            effect_desc fx{};
            fx.effect_id = "rep.hostile_contact";
            auto m = modifier_desc{};
            m.target_attr = "rep.hauler";
            m.stack_id = "rep.nudge";
            m.op = mod_op::flat_add;
            m.value = 15.0f;
            m.policy = stack_policy::stack_n;
            m.stack_cap = 1;
            fx.bundle = {m};
            REQUIRE(apply_effect(faction_stack, kFxRepNudge, fx) == effect_result::applied);
        }
        if (t == kPilotKO && !no_pilot_ko) {
            REQUIRE(link.drop(1, "seat_pilot", pilot_stack).has_value());
            effect_desc fx{};
            fx.effect_id = "ko.pilot";
            fx.expire_tick = std::uint64_t(kPilotBack);
            auto m = modifier_desc{};
            m.target_attr = "pilot.reflex";
            m.stack_id = "ko.recovery";
            m.op = mod_op::flat_add;
            m.value = -1.0f;
            m.policy = stack_policy::stack_n;
            m.stack_cap = 1;
            fx.bundle = {m};
            REQUIRE(apply_effect(pilot_stack, kFxPilotKO, fx) == effect_result::applied);
        }
        if (t == kPilotBack && !no_pilot_ko)
            REQUIRE(link.sit(1, "seat_pilot", pilot_stack).has_value());

        // 3. power shedding is idempotent — call after every mutation.
        pool.rebalance();

        // 4. empty-tank gate synced to the band tag (set last tick's step,
        // or cleared by a refill's band re-evaluation).
        sync_empty_gate();

        // 5. propellant glue (6.11): load attr = THIS tick's aggregated
        // force / Isp, replace policy.
        const float force = *ship_pipe.derived("ship.thrust.force");
        const float isp = *ship_pipe.derived("ship.isp");
        {
            auto m = modifier_desc{};
            m.target_attr = "propellant.load";
            m.stack_id = "burn.load";
            m.op = mod_op::flat_add;
            m.value = force / isp;
            m.policy = stack_policy::replace;
            const auto br = ship_stack.add_modifier(kBurnSrc, m);
            REQUIRE((br == mod_add_result::added || br == mod_add_result::replaced));
        }

        // 6. reservoirs integrate THIS tick's flows. Once the empty tag
        // sets in this step, the gate arms immediately: the markers and the
        // NEXT tick see the zeroed thrust (the burn that drained the last
        // unit already used this tick's full force — draw and thrust stay
        // consistent at the dry tick).
        prop.step();
        market.step();
        sync_empty_gate();

        // 7. market attrs: stock + re-derived price as base attrs (#20b).
        ship_stats.set_base("market.stock", float(market.value()));
        ship_stats.set_base("market.price", float(price_curve.evaluate(market.value()).value()));

        // 8. reputation bands, re-evaluated EVERY tick (never on edges).
        apply_bands(double(*faction_pipe.derived("rep.hauler")), rep_bands, faction_tags);

        // 9. read-all pass: force every pipeline to resolve its derived
        // values BEFORE the canonical hash (recompute-on-read cache).
        for (const char* k : {"ship.thrust.force", "ship.isp", "ship.assist.stab_kd",
                              "power.supply", "power.draw.life", "power.draw.shield",
                              "power.draw.gun", "func.life", "func.shield", "func.gun",
                              "propellant.supply", "propellant.load", "propellant.leak",
                              "market.supply", "market.load", "market.leak"})
            REQUIRE(ship_pipe.derived(k).has_value());
        REQUIRE(pilot_pipe.derived("pilot.reflex").has_value());
        REQUIRE(gunner_pipe.derived("gunner.aim").has_value());
        REQUIRE(faction_pipe.derived("rep.hauler").has_value());

        marker mk{};
        mk.eng_lit = wiring.lit("engines");
        mk.gun_lit = wiring.lit("gun");
        mk.sys_gun = pool.lit("sys.gun");
        mk.func_gun = *ship_pipe.derived("func.gun") > 0.0f;
        mk.thrust_pos = *ship_pipe.derived("ship.thrust.force") > 0.0f;
        mk.prop_empty = ship_tags.contains("propellant.empty");
        mk.wary = faction_tags.contains("faction.hauler.wary");
        mk.pilot_in = occ.seat_of(1).has_value();
        mk.gunner_in = occ.seat_of(2).has_value();
        mk.reflex = *pilot_pipe.derived("pilot.reflex") > 0.0f;
        mk.aim = *gunner_pipe.derived("gunner.aim") > 0.0f;
        R.mk.push_back(mk);
        R.stats_hash.push_back(fold_stats(ship_stats, pilot_stats, gunner_stats, faction_stats));
        R.prop_val.push_back(prop.value());
        R.market_val.push_back(market.value());

        // 10. flight command: thrust from the SAME aggregated force that
        // fed the burn this tick; assist sampled from the pipeline.
        tick_command cmd{};
        cmd.thrust_body = {0.0, 0.0, -double(force)};
        if ((t >= kSpin1a && t < kSpin1b) || (t >= kSpin2a && t < kSpin2b)) {
            cmd.torque_body = {0.0, kSpinT, 0.0};  // open-loop evade spin
        } else {
            assist_gains g{};
            g.stab_kd = double(*ship_pipe.derived("ship.assist.stab_kd"));
            cmd.torque_body = stabilizer_torque(s, g);
        }
        R.log.append(cmd);
        replay_step(w, R.log, std::size_t(t), tick_t(t), p, s);
        R.sim_hash.push_back(state_hash(tick_t(t), s));
        R.omega_y.push_back(s.omega_body.y);
        if ((t + 1) % kCp == 0)
            R.cps.push_back({tick_t(t), s});
    }
    R.price_at_end = *ship_pipe.derived("market.price");
    R.stats_at_end = R.stats_hash.back();
    return R;
}

std::size_t first_diff(const std::vector<std::uint64_t>& a, const std::vector<std::uint64_t>& b) {
    std::size_t i = 0;
    while (i < a.size() && a[i] == b[i])
        ++i;
    return i;
}

}  // namespace

TEST_CASE("stats_combat_scenario") {
    const run_result A = run_scenario();
    CHECK(std::size(A.sim_hash) == std::size_t(kTotal));
    CHECK(std::size(A.cps) == std::size_t(kTotal / kCp) + 1);  // -1 + ten

    SUBCASE("run is fully deterministic: fresh rerun -> identical streams") {
        const run_result B = run_scenario();
        CHECK(first_diff(A.sim_hash, B.sim_hash) == A.sim_hash.size());
        CHECK(first_diff(A.stats_hash, B.stats_hash) == A.stats_hash.size());
        CHECK(A.mk == B.mk);
        CHECK(A.prop_val == B.prop_val);
        CHECK(A.market_val == B.market_val);
        CHECK(A.omega_y == B.omega_y);
        CHECK(A.stats_at_end == B.stats_at_end);
    }

    SUBCASE("propellant drains hand-exactly; dry lands at tick 20999") {
        CHECK(A.prop_val[1199] == 10800.0);    // 1.0/tick during full force
        CHECK(A.prop_val[2399] == 10200.0);    // pod hit half-force ticks 1200..2399
        CHECK(A.prop_val[4799] == 7800.0);     // repaired leg to the blackout
        CHECK(A.prop_val[5399] == 7800.0);     // bus killed -> force 0 -> no draw
        CHECK(A.prop_val[8999] == 4200.0);     // burn B
        CHECK(A.prop_val[9000] == 11999.0);    // refill 12000 then one tick of draw
        CHECK(A.prop_val[kDryTick - 1] == 1.0);
        CHECK(A.prop_val[kDryTick] == 0.0);            // dry EXACTLY here
        CHECK(A.prop_val[kTotal - 1] == 0.0);          // never negative
        CHECK_FALSE(A.mk[kDryTick - 1].prop_empty);
        CHECK(A.mk[kDryTick].prop_empty);              // band tag lands with it
        CHECK_FALSE(A.mk[kDryTick].thrust_pos);        // dark-not-dead x0 armed
    }

    SUBCASE("wiring cascade + targeted sever give exact dark windows") {
        CHECK_FALSE(A.mk[kBusKilled].eng_lit);
        CHECK_FALSE(A.mk[kBusKilled].gun_lit);
        CHECK(A.mk[kBusRevived].eng_lit);
        CHECK(A.mk[kBusRevived].gun_lit);
        CHECK_FALSE(A.mk[kWireSevered].eng_lit);
        CHECK(A.mk[kWireSevered].gun_lit);  // engines-only dark
        CHECK(A.mk[kWireRepaired].eng_lit);
    }

    SUBCASE("power oversubscription sheds the gun, restores at expiry") {
        CHECK(A.mk[kOverload - 1].sys_gun);
        CHECK_FALSE(A.mk[kOverload].sys_gun);      // 360 draw > 300 supply
        CHECK_FALSE(A.mk[kOverloadGone - 1].sys_gun);
        CHECK(A.mk[kOverloadGone].sys_gun);        // effect expired -> trial-lift
    }

    SUBCASE("func.gun zeroed by all three causes, independently lifted") {
        CHECK_FALSE(A.mk[kBusKilled].func_gun);     // wiring dark
        CHECK(A.mk[kBusRevived].func_gun);
        CHECK_FALSE(A.mk[kOverload].func_gun);      // power shed
        CHECK(A.mk[kOverloadGone].func_gun);
        CHECK_FALSE(A.mk[kGunnerKO].func_gun);      // unmanned seat
        CHECK_FALSE(A.mk[kGunnerBack - 1].func_gun);
        CHECK(A.mk[kGunnerBack].func_gun);
    }

    SUBCASE("crew KO: grants drop, recovery expires, stats reflect it") {
        CHECK_FALSE(A.mk[kGunnerKO].gunner_in);
        CHECK_FALSE(A.mk[kGunnerKO].aim);      // base 1.0 + flat -1.0 = 0
        CHECK(A.mk[kGunnerBack].aim);          // recovery expired + reseated
        CHECK(A.mk[kPilotKO - 1].pilot_in);
        CHECK_FALSE(A.mk[kPilotKO].pilot_in);
        CHECK_FALSE(A.mk[kPilotKO].reflex);
        CHECK_FALSE(A.mk[kPilotBack - 1].reflex);
        CHECK(A.mk[kPilotBack].reflex);
    }

    SUBCASE("market transaction + reputation flip (done-when (h) data)") {
        CHECK(A.market_val[kSell - 1] == 19000.0);
        CHECK(A.market_val[kSell] == 19601.0);  // +1 flow +600 sold, EXACT
        CHECK(A.market_val[kSell + 1] == 19602.0);
        CHECK(A.market_val[kTotal - 1] == 20000.0);  // ceiling
        // Price is re-derived from stock EVERY tick, never stored (#20b):
        // at the ceiling the authored curve tail lands on exactly 0.5f.
        CHECK(std::bit_cast<std::uint32_t>(A.price_at_end) == 0x3f000000u);
        CHECK_FALSE(A.mk[kRepNudge - 1].wary);
        CHECK(A.mk[kRepNudge].wary);  // +15 >= 10 band flips the very next tick
    }

    SUBCASE("pilot KO measurably changes BOTH the sim and stats streams") {
        // Spin-up [29400,30000) brings omega to ~20 rad/s; the KO zeroes the
        // assist so omega FREEZES until reseating, then decays again.
        CHECK(A.omega_y[kPilotBack - 1] == A.omega_y[kPilotKO]);  // frozen EXACT
        CHECK(A.omega_y[kTotal - 1] < A.omega_y[kPilotBack - 1]);  // decayed again
        CHECK(std::fabs(A.omega_y[kTotal - 1]) < std::fabs(A.omega_y[kPilotKO]));
        const run_result V = run_scenario(true);  // no-KO variant
        CHECK(first_diff(A.sim_hash, V.sim_hash) == std::size_t(kPilotKO));
        CHECK(first_diff(A.stats_hash, V.stats_hash) == std::size_t(kPilotKO));
    }

    SUBCASE("scrubber API replays the scenario to any tick, hash-matching") {
        rail_world w;
        ReplayScrubber sc(w, [&] {
            rigid_params p{};
            p.mass = kMass;
            p.inertia_tensor = dmat3::diag({2.0e5, kIy, 7.0e5});
            return p;
        }());
        REQUIRE(sc.load(A.log, A.cps));
        CHECK(sc.log_size() == std::size_t(kTotal));

        for (const tick_t t :
             std::vector<tick_t>{-1, 0, 3599, 3600, 7198, 8999, 17999, 20999, 24599, 35999}) {
            REQUIRE(sc.jump_to(t));
            CHECK(sc.cursor() == t);
            if (t >= 0)
                CHECK(sc.hash().value() == A.sim_hash[std::size_t(t)]);
        }
        // Checkpoint locality: exact cp hit costs nothing, gaps cost the gap.
        sc.reset_replay_ops();
        REQUIRE(sc.jump_to(7199));  // cp tick
        CHECK(sc.replay_ops() == 0);
        REQUIRE(sc.jump_to(7200));
        CHECK(sc.replay_ops() == 1);
        REQUIRE(sc.jump_to(3600));  // 1 past cp 3599
        CHECK(sc.replay_ops() == 2);
        // step_frame chains continue the live run exactly.
        REQUIRE(sc.jump_to(35999));
        CHECK_FALSE(sc.step_frame());  // end of log, fail closed
        REQUIRE(sc.jump_to(20998));
        REQUIRE(sc.step_frame());
        CHECK(sc.hash().value() == A.sim_hash[kDryTick]);
    }
}
