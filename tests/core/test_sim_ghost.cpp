// Phase 4 task 4.10 — ghost trajectory preview pins. The load-bearing
// claims: (1) divisor 1 is BITWISE the live sim (one codepath), (2) coarse
// preview error against the live sim is bounded by a hand-computed value
// (closed form in void; hand-derived bound in gravity), (3) the step budget
// is enforced, (4) assists ride along for free (autopilot ghost parks on
// the moving waypoint too).
#include <doctest/doctest.h>

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

TEST_CASE("sim_ghost_preview") {
    const rail_world w = make_world();
    const rigid_params p = ship_params();

    SUBCASE("void constant thrust: closed forms match exactly; divisor 1 is bitwise live") {
        ReplayLog log;
        tick_command c;
        c.thrust_body = dvec3(p.mass * 10.0, 0.0, 0.0);  // a = 10 m/s^2 (body==world, att id)
        for (int i = 0; i < 3600; ++i) log.append(c);

        rigid_state s0;
        s0.pos = dvec3(1.0e6, 0.0, 0.0);  // outside every well: gravity exactly 0

        // Live: 3600 canonical ticks.
        rigid_state live = s0;
        for (std::size_t i = 0; i < log.size(); ++i) replay_step(w, log, i, static_cast<tick_t>(i), p, live);

        // Semi-implicit Euler under CONSTANT accel has a closed form
        // (pinned in 4.6): pos = a*dt^2*N(N+1)/2, vel = a*dt*N exactly.
        // Hand values, a=10, dt=1/60, T=60 s:
        const double a = 10.0, dt = kSim_tick_dt;
        const std::size_t N = 3600;
        const double live_x = a * dt * dt * (static_cast<double>(N) * (N + 1) / 2.0);  // 18005
        CHECK(std::abs(live.pos.x - (1.0e6 + live_x)) < 1e-6);  // closed form = DISPLACEMENT
        CHECK(std::abs(live.vel.x - a * dt * N) < 1e-9);

        // Ghost at divisor 4: same closed form with dt'=4dt, M=900 steps:
        // x = a*(4dt)^2 * 900*901/2 = 18020 -> gap to live = 15 m, exactly
        // the hand formula 0.5*a*T*(D-1)*dt. Preview error is ANALYTIC here.
        const auto g4 = ghost_preview(w, log, 0, 0, s0, p, 3600, 4);
        CHECK(g4.size() == 900);
        const rigid_state& g4_end = g4.back();
        const double g4_x = a * (4 * dt) * (4 * dt) * (900.0 * 901.0 / 2.0);
        CHECK(std::abs(g4_end.pos.x - (1.0e6 + g4_x)) < 1e-6);
        CHECK(std::abs(g4_end.pos.x - live.pos.x - 0.5 * a * 60.0 * 3.0 * dt) < 1e-6);
        // Velocities agree EXACTLY (semi-implicit vel is dt-scheme-invariant
        // under constant accel): the coarse preview's only error is the
        // pinned position bias.
        CHECK(std::abs(g4_end.vel.x - live.vel.x) < 1e-9);

        // Divisor 1 == live, BITWISE, via per-tick state hashes: the ghost
        // IS the live integrator, not an approximation of it. Horizon is
        // budget-sized (1024 full-rate steps = the pinned budget).
        const auto g1 = ghost_preview(w, log, 0, 0, s0, p, kGhost_preview_max_steps, 1);
        CHECK(g1.size() == kGhost_preview_max_steps);
        std::vector<std::uint64_t> gh, lh;
        rigid_state walk = s0;
        for (std::size_t i = 0; i < kGhost_preview_max_steps; ++i) {
            replay_step(w, log, i, static_cast<tick_t>(i), p, walk);
            lh.push_back(state_hash(static_cast<tick_t>(i + 1), walk));
            gh.push_back(state_hash(static_cast<tick_t>(i + 1), g1[i]));
        }
        CHECK(gh == lh);
        // Caller's state untouched by the ghost.
        CHECK(s0.pos == dvec3(1.0e6, 0.0, 0.0));
    }

    SUBCASE("gravity-world fall: coarse preview inside hand-derived jerk bound") {
        // A TAPER-BAND fall that never crosses r=taper_start (no core
        // traversal: crossing events put step-phase error ~v*D*dt into the
        // comparison, which no constant-accel formula bounds). 25 s from
        // y=7000 drops ~1.7 km, stays in [5300, 7000] where g ~ 5.6 m/s^2.
        ReplayLog log;
        constexpr int kFall = 1500;  // 25 s
        for (int i = 0; i < kFall; ++i) log.append(tick_command{});  // pure fall
        rigid_state s0;
        s0.pos = dvec3(0.0, 7000.0, 0.0);

        rigid_state live = s0;
        for (std::size_t i = 0; i < log.size(); ++i) replay_step(w, log, i, static_cast<tick_t>(i), p, live);
        CHECK(live.pos.y > 2000.0);  // no core crossing: bound below is valid

        const auto g4 = ghost_preview(w, log, 0, 0, s0, p, kFall, 4);
        CHECK(g4.size() == 375);
        CHECK(g4.size() <= kGhost_preview_max_steps);
        const double err = length(g4.back().pos - live.pos);
        // Hand bound, D=4: constant-accel bias 0.5*g*T*(D-1)*dt ~ 3.5 m
        // plus taper jerk term 0.5*(dg/dr|max * v_max)*T^2*(D-1)*dt ~ 3 m
        // (dg/dr <= 1.5*g_span/taper = 1.34e-3 1/m; v_max ~ 139 m/s).
        // Allowance x3 -> 20 m; path ~1.7 km -> relative error ~1%.
        CHECK(err < 20.0);
        CHECK(err < 0.02 * length(live.pos - s0.pos));
    }

    SUBCASE("budget clamp + determinism") {
        ReplayLog log;
        tick_command c;
        c.assist_mask = kAssist_autopilot;
        c.autopilot_body = 1;
        for (int i = 0; i < 3600; ++i) log.append(c);
        rigid_state s0;
        s0.pos = dvec3(0.0, 1500.0, 0.0);

        // Oversized horizon with divisor 1 clamps at the pinned budget.
        const auto budgeted = ghost_preview(w, log, 0, 0, s0, p, 100000, 1);
        CHECK(budgeted.size() == kGhost_preview_max_steps);
        CHECK(budgeted.size() <= kGhost_preview_max_steps);

        // Same inputs -> bit-identical preview stream (hash the samples).
        const auto a = ghost_preview(w, log, 0, 0, s0, p, 3600, 4);
        const auto b = ghost_preview(w, log, 0, 0, s0, p, 3600, 4);
        std::vector<std::uint64_t> ha, hb;
        for (std::size_t k = 0; k < a.size(); ++k) {
            ha.push_back(state_hash(static_cast<tick_t>(4 * (k + 1)), a[k]));
            hb.push_back(state_hash(static_cast<tick_t>(4 * (k + 1)), b[k]));
        }
        CHECK(ha == hb);
    }

    SUBCASE("assists ride along: autopilot ghost reaches the moving waypoint") {
        ReplayLog log;
        tick_command c;
        c.assist_mask = kAssist_autopilot;
        c.autopilot_body = 1;
        for (int i = 0; i < 3600; ++i) log.append(c);
        rigid_state s0;
        s0.pos = dvec3(0.0, 1500.0, 0.0);

        const auto g = ghost_preview(w, log, 0, 0, s0, p, 3600, 4);
        CHECK(g.size() == 900);
        // Ghost inherits the FULL assist stack via assemble_tick_inputs:
        // 60 s of autopilot at coarse rate still closes on the MOVING
        // station to within hundreds of metres (live parks it ~0 m at 90 s;
        // at 60 s live is ~15 m — coarse integration error dominates).
        const dvec3 wp = *w.position(1, 3600);
        CHECK(length(g.back().pos - wp) < 500.0);
        CHECK(length(g.back().vel - *w.velocity(1, 3600)) < 100.0);
    }
}
