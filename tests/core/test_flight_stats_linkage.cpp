// test: flight_stats_linkage — 6.8 done-when (b): a pilot-stat change
// MEASURABLY changes the Phase-4 flight model's numbers through the seat
// path. Wiring: the pilot seat affordance carries the assist-authority
// attrs (grant x2 while seated; x0 zeroing while unmanned — #18b), the
// game-side glue reads the SHIP pipeline every tick into assist_gains, and
// the same scripted command stream flies wildly different trajectories
// whose decay pins are hand-computed discrete first-order law:
//     omega_{n+1} = omega_n * (1 - stab_kd*dt/I_y)     (tau = -kd*omega_pre)
// Mounting MID-FLIGHT bends the curve at exactly the hand-picked tick.
#include <bit>
#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/assist.h>
#include <ve/core/sim/effect_bus.h>
#include <ve/core/sim/loadout.h>
#include <ve/core/sim/occupancy.h>
#include <ve/core/sim/replay.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/seat_link.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_pipeline.h>
#include <ve/game/role_tags.h>

namespace {
using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;

constexpr double kIy = 8.0e5;         // body-Y inertia (analytic ship)
constexpr double kMass = 50000.0;
constexpr double kBase_kd = 1.0e5;    // baseline stabilizer gain (attr base)
constexpr double kSpinT = 1.6e6;      // spin-phase torque, body +Y
constexpr int kSpin_ticks = 60;       // domega/tick = T*dt/I = 1/30 -> w=2.0
constexpr int kStab_ticks = 240;

std::vector<std::string> role_registry() {
    return {ve::game::kPhase5_role_tags.begin(), ve::game::kPhase5_role_tags.end()};
}

Loadout ship_loadout() {
    hull_info h;
    h.class_tag = "class.poc";
    h.grid = grid_block{2.0, 4, 2.5, 0.05};
    h.hull_mass = kMass;
    std::vector<socket_desc> socks = {
        {"hp_seat_pilot", socket_category::utility_, {}, {}},
    };
    std::vector<component_type> cat = {{"seat.station", socket_category::utility_}};
    std::vector<mount_request> mounts = {
        {"seat_pilot", "seat.station", "hp_seat_pilot", {}, {"station.pilot"}},
    };
    auto r = Loadout::assemble(std::move(h), std::move(socks), std::move(cat),
                               std::move(mounts));
    REQUIRE(r.has_value());
    return std::move(*r);
}

// GAME-SIDE glue (this test IS the game side): ship attrs -> Phase-4 gains.
assist_gains derive_gains(StatsPipeline& sp) {
    assist_gains g{};
    g.stab_kd = *sp.derived("ship.assist.stab_kd");
    return g;
}

// Scripted flight. `mount_at` = tick to sit the pilot; -1 = never; the
// pilot sits from tick 0 when `seated_from_start`. Stand at `disembark_at`
// (-1 = never). Same torque schedule for every variant.
struct flight_result {
    std::vector<std::uint64_t> hashes;
    std::vector<double> w_y;  // omega_body.y AFTER each tick
};

flight_result fly(bool seated_from_start, int mount_at, int disembark_at,
                  std::size_t* rail_events = nullptr) {
    const Loadout lo = ship_loadout();
    const std::vector<std::string> reg = role_registry();
    Occupancy occ(lo, reg);
    ModifierStack ship;      // crew-owned ship systems ride the ship stack
    StatsSet sst;
    sst.set_base("ship.assist.stab_kd", kBase_kd);
    StatsPipeline sp(sst, ship);
    EffectEventBus bus;
    SeatLink link(occ, ship, &bus);

    seat_affordance pilot_aff;
    pilot_aff.role_tag = "station.pilot";
    pilot_aff.occupant_grant = {[] {
        auto m = modifier_desc{};
        m.target_attr = "ship.assist.stab_kd";
        m.stack_id = "seat.pilot_skill";
        m.op = mod_op::multiplier;
        m.value = 2.0f;
        m.policy = stack_policy::exclusive;
        return m;
    }()};
    pilot_aff.when_unmanned = {[] {
        auto m = modifier_desc{};
        m.target_attr = "ship.assist.stab_kd";
        m.stack_id = "seat.pilot_zero";
        m.op = mod_op::multiplier;
        m.value = 0.0f;
        m.policy = stack_policy::exclusive;
        return m;
    }()};
    REQUIRE(link.add_affordance(pilot_aff));

    // Wire the UNMANNED state as live before any flight (seats start empty;
    // scripted sit+stand arms the zeroing through the unit machinery).
    REQUIRE(link.sit(1, "seat_pilot", ship).has_value());
    REQUIRE(link.stand(1, "seat_pilot", ship).has_value());
    if (seated_from_start)
        REQUIRE(link.sit(1, "seat_pilot", ship).has_value());
    REQUIRE(*sp.derived("ship.assist.stab_kd") ==
            (seated_from_start ? 2.0 * kBase_kd : 0.0));

    rigid_params p{};
    p.mass = kMass;
    p.inertia_tensor = dmat3::diag({2.0e5, kIy, 7.0e5});

    ReplayLog log;
    rail_world w;
    rigid_state s{};
    flight_result res{};
    const std::int64_t total = kSpin_ticks + kStab_ticks;
    for (std::int64_t t = 0; t < total; ++t) {
        if (t == mount_at)
            REQUIRE(link.sit(1, "seat_pilot", ship).has_value());
        if (t == disembark_at)
            REQUIRE(link.stand(1, "seat_pilot", ship).has_value());
        tick_command cmd{};
        if (t < kSpin_ticks) {
            cmd.torque_body = {0.0, kSpinT, 0.0};
        } else {
            // Control law sampled from the pipeline AT THIS TICK: the
            // derived gain re-resolves iff stats changed since last read.
            cmd.torque_body = stabilizer_torque(s, derive_gains(sp));
        }
        log.append(cmd);
        replay_step(w, log, static_cast<std::size_t>(t), static_cast<tick_t>(t), p, s);
        res.hashes.push_back(state_hash(static_cast<tick_t>(t), s));
        res.w_y.push_back(s.omega_body.y);
    }
    if (rail_events)
        *rail_events = bus.drain().size();
    return res;
}
}  // namespace

TEST_CASE("flight_stats_linkage") {
    // The spin phase is identical for every variant: omega reaches exactly
    // 60 * (T*dt/I) = 60/30 = 2.0 rad/s.
    // Closed-form spin end = 2.0 rad/s, but the accumulated sim value is
    // the source of truth for the decay pins below (geometric law, doubles).

    SUBCASE("done-when (b): seated vs unseated measurably changes flight numbers") {
        const auto seated = fly(true, -1, -1);
        const auto unman = fly(false, -1, -1);

        // Unmanned: gain ZEROED -> no damping at all, spin persists exactly.
        CHECK(unman.w_y.back() == unman.w_y[kSpin_ticks - 1]);

        // Seated: hand-computed discrete law with kd = 2*base.
        const double r = 1.0 - (2.0 * kBase_kd) / (60.0 * kIy);  // 1 - 1/240
        const double expect = unman.w_y[kSpin_ticks - 1] * std::pow(r, kStab_ticks);
        CHECK(seated.w_y.back() == doctest::Approx(expect).epsilon(1e-12));

        // MEASURABLY different (the done-when wording), and hashes diverge.
        CHECK(seated.w_y.back() < 0.4 * unman.w_y.back());
        std::size_t first_diff = 0;
        while (first_diff < seated.hashes.size() &&
               seated.hashes[first_diff] == unman.hashes[first_diff])
            ++first_diff;
        CHECK(first_diff >= static_cast<std::size_t>(kSpin_ticks));  // spin leg identical
    }

    SUBCASE("mid-flight mount bends the decay at exactly the hand-picked tick") {
        // Unmanned spin (kd=0 all spin), pilot MOUNTS at the stab phase's
        // halfway point (tick 180): first 120 stab ticks undamped, last
        // 120 damped with kd = 2*base.
        const int mount = kSpin_ticks + kStab_ticks / 2;
        const auto mid = fly(false, mount, -1);
        const double r = 1.0 - (2.0 * kBase_kd) / (60.0 * kIy);
        const double expect = mid.w_y[mount - 1] * std::pow(r, kStab_ticks / 2);
        CHECK(mid.w_y.back() == doctest::Approx(expect).epsilon(1e-12));
        // Undamped leg held constant before the mount:
        CHECK(mid.w_y[mount - 1] == mid.w_y[kSpin_ticks]);
        CHECK(mid.w_y[mount] < mid.w_y[mount - 1]);  // damped from that tick
    }

    SUBCASE("mid-flight dismount re-arms the zeroing; omega freeze-frames") {
        const int leave = kSpin_ticks + 60;
        const auto out = fly(true, -1, leave);
        // Damped for 60 stab ticks, then FLAT (zeroing live again).
        const double r = 1.0 - (2.0 * kBase_kd) / (60.0 * kIy);
        CHECK(out.w_y[leave - 1] ==
              doctest::Approx(out.w_y[kSpin_ticks - 1] * std::pow(r, 60)).epsilon(1e-12));
        CHECK(out.w_y.back() == out.w_y[leave]);  // frozen from the stand tick
    }

    SUBCASE("each flight is replay-hash deterministic; variants differ") {
        const auto a1 = fly(true, -1, -1);
        const auto a2 = fly(true, -1, -1);
        CHECK(a1.hashes == a2.hashes);  // same script -> bit-identical hash stream
        const auto b = fly(false, -1, -1);
        CHECK(a1.hashes != b.hashes);
        // The seat rail events ride the same determinism: sit+stand wiring
        // (2) + scripted sit (1) + scripted stand (1) + scripted sit again (1).
        std::size_t ev = 0;
        const auto c = fly(true, -1, -1, &ev);
        CHECK(ev == 3);
        CHECK(c.hashes == a1.hashes);
    }
}
