// Phase 4 task 4.11 — the done-when harness (PLAN Phase 4 (a)/(b)/(c)):
// a scripted figure-8 across the 2-planet + 1-moon hand-authored testbed,
// flown with assists ON (autopilot legs) and a ballistic pulse variant with
// assists OFF, with 4.10 ghost previews covering the WHOLE figure. Pins:
//   (a) assist-on figure-8 ends settled on the final waypoint and every
//       leg parks within tolerance; the assist-OFF ballistic variant is
//       bit-deterministic run-to-run.
//   (b) HUD-style windowed ghost previews (each restarted from the LIVE
//       state, as a real HUD recomputes every frame) stay near live at
//       EVERY tick of the entire figure.
//   (c) replay hashes match run-to-run (both flights).
// Measured via probes (4.11 notes): a 10 km autopilot transit + settle
// needs ~45 s, so legs are 55 s; per-leg park lands at 0.0-2.7 m with
// final rel-velocity 0.6 m/s. Windowed D=4 previews: max gap 23 m.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "ve/core/sim/assist.h"
#include "ve/core/sim/preview.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/replay.h"
#include "ve/core/sim/rigid.h"

namespace {
using ve::core::math::dvec3;
namespace math = ve::core::math;
using namespace ve::core::sim;

constexpr int kLeg_ticks = 3300;  // 55 s per waypoint leg
constexpr std::int32_t kWaypoints[4] = {1, 2, 0, 1};  // p1 -> moon -> p0 -> p1

rail_world make_figure8_world() {
    rail_world w;
    body_def p0;  // homeworld: static root
    p0.surface_radius = 1000.0;
    p0.well_radius = 12000.0;
    p0.well_taper_start = 3000.0;
    p0.surface_g = 9.8;
    if (!w.add_body(p0).has_value()) std::abort();
    body_def p1;  // sister planet on a slow rail
    p1.parent = 0;
    p1.orbit_period_s = 36000.0;
    p1.orbit_radius_m = 10000.0;
    p1.orbit_phase_rad = 0.0;
    p1.surface_radius = 900.0;
    p1.well_radius = 6000.0;
    p1.well_taper_start = 2000.0;
    p1.surface_g = 8.0;
    if (!w.add_body(p1).has_value()) std::abort();
    body_def moon;  // moon of p1: a fast moving waypoint
    moon.parent = 1;
    moon.orbit_period_s = 1800.0;
    moon.orbit_radius_m = 3000.0;
    moon.orbit_phase_rad = 1.5707963267948966;  // pi/2: the p1->moon leg is not radial
    moon.surface_radius = 300.0;
    moon.well_radius = 1500.0;
    moon.well_taper_start = 700.0;
    moon.surface_g = 1.5;
    if (!w.add_body(moon).has_value()) std::abort();
    return w;
}

ReplayLog make_figure8_log() {
    ReplayLog log;
    for (std::size_t leg = 0; leg < 4; ++leg) {
        tick_command c;
        c.assist_mask = kAssist_autopilot;
        c.autopilot_body = kWaypoints[leg];
        for (int i = 0; i < kLeg_ticks; ++i) log.append(c);
    }
    return log;
}

rigid_params ship_params() { return rigid_params{20000.0, dvec3(2.0e5, 2.0e5, 2.0e5)}; }
rigid_state ship_start() {
    rigid_state s;
    s.pos = dvec3(0.0, 1500.0, 0.0);  // inside p0's core band; autopilot lifts off
    return s;
}

std::vector<std::uint64_t> run_hashes(const rail_world& w, const ReplayLog& log,
                                      const rigid_params& p) {
    std::vector<std::uint64_t> hashes;
    rigid_state s = ship_start();
    for (std::size_t i = 0; i < log.size(); ++i) {
        replay_step(w, log, i, static_cast<tick_t>(i), p, s);
        hashes.push_back(state_hash(static_cast<tick_t>(i + 1), s));
    }
    return hashes;
}
} // namespace

TEST_CASE("sim_flight_figure8") {
    const rail_world w = make_figure8_world();
    const ReplayLog log = make_figure8_log();
    const rigid_params p = ship_params();

    SUBCASE("(a) assists on: every leg parks; figure ends settled on the moving waypoint") {
        rigid_state s = ship_start();
        for (std::size_t leg = 0; leg < 4; ++leg) {
            for (int i = 0; i < kLeg_ticks; ++i)
                replay_step(w, log, leg * kLeg_ticks + i,
                            static_cast<tick_t>(leg * kLeg_ticks + i), p, s);
            const auto t = static_cast<tick_t>((leg + 1) * kLeg_ticks);
            // Measured 0.0-2.7 m per leg end (25 km probes timed the law,
            // 10 km probes timed the legs); pins carry ~4x margin.
            CHECK(length(s.pos - *w.position(kWaypoints[leg], t)) < 10.0);
        }
        // Measured 0.60 m/s terminal rel-velocity on a 1.7 m/s moving target.
        const auto t_end = static_cast<tick_t>(log.size());
        const std::int32_t wp = kWaypoints[3];
        CHECK(length(s.vel - *w.velocity(wp, t_end)) < 2.0);
    }

    SUBCASE("(a2) assists-off ballistic pulse flight is bit-deterministic") {
        // Open-loop pilot: timed thrust bursts + torque kicks through the
        // nested wells (thrust under the assist budget's 40 m/s^2:
        // 20 kN / 20 t = 10 m/s^2). Same log twice -> identical hash streams.
        ReplayLog pulses;
        auto append_window = [&](int ticks, dvec3 thrust, dvec3 torque) {
            tick_command c;
            c.thrust_body = thrust;
            c.torque_body = torque;
            for (int i = 0; i < ticks; ++i) pulses.append(c);
        };
        append_window(200, dvec3(2.0e4, 0.0, 0.0), dvec3());   // +X burn
        append_window(100, dvec3(), dvec3(0.0, 1.0e5, 0.0));   // yaw kick
        append_window(700, dvec3(0.0, 0.0, -2.0e4), dvec3());  // -Z burn (attitude turned)
        append_window(200, dvec3(), dvec3(0.0, -1.0e5, 0.0));  // yaw restore
        append_window(1000, dvec3(0.0, 1.5e4, 0.0), dvec3());  // climb out
        while (pulses.size() < 6000) pulses.append(tick_command{});

        rigid_state s = ship_start();
        for (std::size_t i = 0; i < pulses.size(); ++i)
            replay_step(w, pulses, i, static_cast<tick_t>(i), p, s);
        // The flight actually went somewhere (log is not silently no-op).
        CHECK(length(s.pos - ship_start().pos) > 1000.0);
        const auto h1 = run_hashes(w, pulses, p);
        const auto h2 = run_hashes(w, pulses, p);
        CHECK(h1.size() == pulses.size());
        CHECK(h1 == h2);
    }

    SUBCASE("(b) windowed ghost previews track live across the WHOLE figure") {
        // A HUD re-previews from live state every frame; cover the figure
        // with back-to-back 1024-step windows (horizon 1024*D ticks).
        // D=4 is the divisor cap: coarser aliases the autopilot attitude
        // loop (kGhost_preview_max_step_divisor, preview.h).
        std::vector<rigid_state> live;
        {
            rigid_state lv = ship_start();
            for (std::size_t i = 0; i < log.size(); ++i) {
                replay_step(w, log, i, static_cast<tick_t>(i), p, lv);
                live.push_back(lv);
            }
        }
        const std::size_t D = 4;
        double max_gap = 0.0;
        std::size_t sampled = 0;
        for (std::size_t t0 = 0; t0 < log.size(); t0 += kGhost_preview_max_steps * D) {
            const auto g = ghost_preview(w, log, t0, static_cast<tick_t>(t0), live[t0], p,
                                         kGhost_preview_max_steps * D, D);
            CHECK(g.size() <= kGhost_preview_max_steps);
            for (std::size_t k = 0; k < g.size(); ++k) {
                const std::size_t i = t0 + (k + 1) * D - 1;
                if (i >= live.size()) break;
                max_gap = std::max(max_gap, length(g[k].pos - live[i].pos));
                ++sampled;
            }
        }
        // Measured 23.3 m worst gap over 13200 ticks (autopilot guidance
        // re-aims from the ghost's OWN state, so coarse-rate error
        // self-corrects instead of ratcheting). Pin ~2x margin.
        CHECK(max_gap < 50.0);
        CHECK(sampled * D + D >= live.size());  // windows x stride cover every tick
    }

    SUBCASE("(b2) preview REFUSES coarse rates: divisor > cap clamps to cap") {
        // The alias refusal must be structural, not documented-only: an
        // over-coarse divisor returns the capped preview BITWISE.
        const auto g4 = ghost_preview(w, log, 0, 0, ship_start(), p, log.size(), 4);
        const auto g16 = ghost_preview(w, log, 0, 0, ship_start(), p, log.size(), 16);
        REQUIRE(g16.size() == g4.size());
        for (std::size_t k = 0; k < g4.size(); ++k)
            CHECK(state_hash(static_cast<tick_t>(k + 1), g16[k]) ==
                  state_hash(static_cast<tick_t>(k + 1), g4[k]));
    }

    SUBCASE("(c) replay hashes match run-to-run on the assist-on figure") {
        const auto h1 = run_hashes(w, log, p);
        const auto h2 = run_hashes(w, log, p);
        CHECK(h1.size() == log.size());
        CHECK(h1 == h2);
    }
}
