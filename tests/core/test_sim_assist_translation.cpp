// Phase 4 task 4.9 — translation assist pins (air brake + waypoint
// autopilot). Everything runs through the SHARED replay_step so the pins
// cover the assist math AND its command-stream wiring (4.10 ghost inherits).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "ve/core/sim/assist.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/replay.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/well_blend.h"

namespace {
using ve::core::math::dvec3;
namespace math = ve::core::math;
using namespace ve::core::sim;

// Planet (root) + a rail-bodied well "station". Same shape as the 4.5
// testbed: nested wells, moon rides its planet.
rail_world make_world() {
    rail_world w;
    body_def planet;
    planet.surface_radius = 1000.0;
    planet.well_radius = 13000.0;
    planet.well_taper_start = 2000.0;
    planet.surface_g = 9.8;
    if (!w.add_body(planet).has_value()) std::abort();
    body_def station;
    station.parent = 0;
    station.orbit_period_s = 3600.0;
    station.orbit_radius_m = 20000.0;
    station.orbit_phase_rad = 0.0;
    station.surface_radius = 300.0;
    station.well_radius = 5000.0;
    station.well_taper_start = 1000.0;
    station.surface_g = 2.0;
    if (!w.add_body(station).has_value()) std::abort();
    return w;
}

rigid_params ship_params() { return rigid_params{20000.0, dvec3(2.0e5, 2.0e5, 2.0e5)}; }
} // namespace

TEST_CASE("sim_assist_translation") {
    const rail_world w = make_world();
    const rigid_params p = ship_params();

    SUBCASE("air brake collapses to the MOVING home-well frame") {
        const dvec3 st_pos = *w.position(1, 0);
        const dvec3 st_vel = *w.velocity(1, 0);
        // 2*pi*20000/3600 = 34.9 m/s rail speed.
        CHECK(length(st_vel) > 34.0);
        CHECK(length(st_vel) < 36.0);
        CHECK(dominant_well_at(w, st_pos + dvec3(0.0, 1500.0, 0.0), 0) == 1);

        rigid_state s;
        s.pos = st_pos + dvec3(0.0, 1500.0, 0.0);
        s.vel = st_vel + dvec3(300.0, 0.0, 150.0);  // |v_rel| ~ 335
        ReplayLog log;
        tick_command c;
        c.assist_mask = kAssist_air_brake;
        for (int i = 0; i < 1500; ++i) log.append(c);

        int settle_tick = -1;
        double worst_rel = 0.0;
        for (std::size_t i = 0; i < log.size(); ++i) {
            replay_step(w, log, i, static_cast<tick_t>(i), p, s);
            const double rel = length(s.vel - *w.velocity(1, static_cast<tick_t>(i)));
            worst_rel = std::max(worst_rel, rel);
            if (settle_tick < 0 && rel < 10.0) settle_tick = static_cast<int>(i);
        }
        // Braking only: |v_rel| never exceeds its initial ~335.4.
        CHECK(worst_rel < 336.0);
        // Pin: settled (within the arcade equilibrium floor below) <= 1200
        // ticks (~20 s: ~5 s at the 40 m/s^2 clamp, then ab_k exponential).
        CHECK(settle_tick >= 0);
        CHECK(settle_tick <= 1200);
        // Arcade equilibrium pinned, not hidden: the brake balances the
        // station's well pull, so the residual drift is ~g_home/ab_k (~6 m/s),
        // NOT exactly zero.
        CHECK(length(s.vel - *w.velocity(1, 1499)) < 10.0);
    }

    SUBCASE("air brake with no well -> static root frame, exact decay law") {
        rigid_state s;
        s.pos = dvec3(1.0e6, 0.0, 0.0);  // outside every well
        s.vel = dvec3(100.0, 0.0, 0.0);
        CHECK(!dominant_well_at(w, s.pos, 0).has_value());

        ReplayLog log;
        tick_command c;
        c.assist_mask = kAssist_air_brake;
        for (int i = 0; i < 1200; ++i) log.append(c);
        for (std::size_t i = 0; i < log.size(); ++i)
            replay_step(w, log, i, static_cast<tick_t>(i), p, s);
        // Unclamped regime (100*ab_k = 30 < max_accel): semi-implicit
        // exponential vel *= (1 - ab_k*dt)^n — pin the analytic value.
        const double expect = 100.0 * std::pow(1.0 - kArcade_assist_gains.ab_k * kSim_tick_dt, 1200.0);
        CHECK(length(s.vel) < 1.0);
        CHECK_FALSE(s.vel == dvec3(0.0, 0.0, 0.0));  // asymptotic, never exactly 0
        CHECK(length(s.vel) > 0.5 * expect);
        CHECK(length(s.vel) < 2.0 * expect);
    }

    SUBCASE("autopilot reaches a rail-riding waypoint and settles") {
        const dvec3 st0 = *w.position(1, 0);
        CHECK(length(st0 - dvec3(20000.0, 0.0, 0.0)) < 1e-9);  // phase 0 pin

        rigid_state s;
        s.pos = dvec3(0.0, 1500.0, 0.0);  // near planet, ~20 km from station
        ReplayLog log;
        tick_command c;
        c.assist_mask = kAssist_autopilot;
        c.autopilot_body = 1;
        constexpr int kTicks = 5400;  // 90 s
        for (int i = 0; i < kTicks; ++i) log.append(c);

        auto gap_at = [&](tick_t t) { return length(s.pos - *w.position(1, t)); };
        auto relv_at = [&](tick_t t) { return length(s.vel - *w.velocity(1, t)); };
        auto aim_at = [&](tick_t t) {
            const dvec3 fwd = math::rotate(s.att, dvec3(0.0, 0.0, -1.0));
            return math::dot(fwd, math::normalize(*w.position(1, t) - s.pos));
        };
        double vel_at_60 = 0.0, aim_mid = 0.0, aim_flip = 0.0, max_relv = 0.0;
        double min_gap_after_arrival = 1.0e30;
        double gap_at_70s = 0.0;
        for (std::size_t i = 0; i < log.size(); ++i) {
            replay_step(w, log, i, static_cast<tick_t>(i), p, s);
            const tick_t t = static_cast<tick_t>(i);
            if (i == 59) vel_at_60 = length(s.vel);
            if (i == 1199) aim_mid = aim_at(t);
            if (i == 1799) aim_flip = aim_at(t);
            if (i == 4199) gap_at_70s = gap_at(t);
            if (i >= 120) max_relv = std::max(max_relv, relv_at(t));
            if (i >= 2700) min_gap_after_arrival = std::min(min_gap_after_arrival, gap_at(t));
        }
        // Thrust budget observable: unclamped guidance would be ~1000 m/s^2;
        // after 1 s |v| ~ max_accel (40) + gravity residue, NOT ~1000.
        CHECK(vel_at_60 > 38.0);
        CHECK(vel_at_60 < 50.0);
        // Attitude loop owns yaw: nose flies the line (~20 s in)...
        CHECK(aim_mid > 0.97);
        // ...and FLIPS for the retro burn (flip-and-burn, the stopping law's
        // signature): nose points AWAY from the waypoint at ~30 s.
        CHECK(aim_flip < -0.99);
        // Stopping law envelope: closing speed never cracks the braking
        // envelope sqrt(2*a_max*gap) (peak measured ~790 m/s; an unsaturated
        // position PD would reach ~1050 and overshoot by kilometres).
        CHECK(max_relv < 850.0);
        // No crash-through: after arrival the gap never dips below ~0.4 m
        // (asymptotic servo, not a flyby).
        CHECK(min_gap_after_arrival > 0.4);
        // Parked: at the MOVING rail waypoint within centimetres, matching
        // its velocity, by 90 s.
        CHECK(gap_at_70s < 5.0);
        CHECK(gap_at(kTicks - 1) < 5.0);
        CHECK(relv_at(kTicks - 1) < 1.0);
    }

    SUBCASE("assist determinism: identical logs -> identical hash streams") {
        ReplayLog log;
        tick_command c;
        c.assist_mask = kAssist_autopilot;
        c.autopilot_body = 1;
        for (int i = 0; i < 600; ++i) log.append(c);

        rigid_state a, b;
        a.pos = b.pos = dvec3(0.0, 1500.0, 0.0);
        std::vector<std::uint64_t> ha, hb;
        for (std::size_t i = 0; i < log.size(); ++i) {
            replay_step(w, log, i, static_cast<tick_t>(i), p, a);
            replay_step(w, log, i, static_cast<tick_t>(i), p, b);
            ha.push_back(state_hash(static_cast<tick_t>(i), a));
            hb.push_back(state_hash(static_cast<tick_t>(i), b));
        }
        CHECK(ha == hb);
        CHECK(replay_interval_hash(ha, 0, ha.size() - 1) == replay_interval_hash(hb, 0, hb.size() - 1));
        // Assist must actually change the trajectory: same log WITHOUT the
        // autopilot bit diverges within the interval.
        ReplayLog plain;
        for (int i = 0; i < 600; ++i) plain.append(tick_command{});
        rigid_state n;
        n.pos = dvec3(0.0, 1500.0, 0.0);
        std::vector<std::uint64_t> hn;
        for (std::size_t i = 0; i < plain.size(); ++i) {
            replay_step(w, plain, i, static_cast<tick_t>(i), p, n);
            hn.push_back(state_hash(static_cast<tick_t>(i), n));
        }
        CHECK(replay_interval_hash(hn, 0, hn.size() - 1) != replay_interval_hash(ha, 0, ha.size() - 1));
    }
}
