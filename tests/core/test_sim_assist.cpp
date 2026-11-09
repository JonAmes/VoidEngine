// Phase 4 task 4.8 — toggleable attitude assists (sim_assist_attitude).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "ve/core/sim/assist.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/replay.h"
#include "ve/core/sim/well_blend.h"
#include "ve/core/math/quat.h"

namespace {

using namespace ve::core;
using ve::core::math::dvec3;  // nested-namespace names don't leak via using namespace
using sim::ReplayLog;
using sim::assist_gains;
using sim::auto_level_torque;
using sim::body_def;
using sim::rail_world;
using sim::rigid_params;
using sim::rigid_state;
using sim::stabilizer_torque;
using sim::tick_command;
using sim::tick_t;

// Planet at origin via root anchor: stand at (0,1500,0), surface 1000,
// constant-g core down to r=2000 -> local up at the ship is exactly +Y.
rail_world level_world() {
    rail_world w;
    body_def p{};
    p.surface_radius = 1000.0;
    p.well_radius = 13000.0;
    p.well_taper_start = 2000.0;
    p.surface_g = 9.8;
    if (!w.add_body(p).has_value())
        std::abort();  // testbed construction is scripted; failure = bug
    return w;
}

double tilt_angle(const rigid_state& s) {  // vs world +Y (level here)
    const dvec3 up = math::rotate(s.att, dvec3(0.0, 1.0, 0.0));
    return std::acos(std::clamp(math::dot(up, dvec3(0.0, 1.0, 0.0)), -1.0, 1.0));
}

} // namespace

TEST_CASE("sim_assist_attitude") {
    const rigid_params p{20000.0, dvec3(2e5, 2e5, 2e5)};  // isotropic: simple settle math

    // ---- stabilizer: tumbled body converges within a pinned tick count.
    // stab_kd=1e5, I=2e5 -> time constant 2 s; |w0|=sqrt(50)~7.07 -> 0.01
    // rad/s at 2*ln(707)~13.1 s = 789 ticks; pin the envelope at 900.
    {
        rigid_state s{};
        s.omega_body = dvec3(3.0, -4.0, 5.0);
        int settle_tick = -1;
        for (int k = 1; k <= 2400; ++k) {   // 40 s tail
            sim::integrate_step(s, p, dvec3(0, 0, 0),
                                sim::stabilizer_torque(s, sim::kArcade_assist_gains));
            if (settle_tick < 0 && length(s.omega_body) < 0.01)
                settle_tick = k;
        }
        CHECK(settle_tick > 0);
        CHECK(settle_tick <= 900);
        CHECK(length(s.omega_body) < 1e-6);   // deep in the tail at 40 s
    }

    // ---- auto-level holds level against a constant disturbance torque.
    // Equilibrium tilt solves torque balance (disturbance + weight from the
    // tilted hover thrust): measured 0.037 rad; pins: max tilt during the
    // transient < 0.05 rad, final tilt < 0.05 rad (35% margin).
    {
        rigid_state s{};
        s.pos = dvec3(0.0, 1500.0, 0.0);
        const auto w = level_world();
        const dvec3 disturbance(5.0e4, 0.0, 0.0);
        double max_tilt = 0.0;
        for (int k = 1; k <= 1800; ++k) {
            const dvec3 g = sim::total_accel(w, s.pos, k);
            // Hover thrust in body +Y: without it the ship free-falls through
            // the core and local up legitimately flips at the center — the
            // level loop would FOLLOW that flip (correct, but not this pin).
            const dvec3 thrust_world = math::rotate(s.att, dvec3(0.0, 1.96e5, 0.0));
            sim::integrate_step(s, p, g + thrust_world * (1.0 / p.mass),
                                auto_level_torque(g, s, sim::kArcade_assist_gains) + disturbance);
            max_tilt = std::max(max_tilt, tilt_angle(s));
        }
        CHECK(max_tilt < 0.05);
        CHECK(tilt_angle(s) < 0.05);
    }

    // ---- auto-level has NO yaw authority: level ship spinning about its
    // own +Y keeps omega.y bit-identical forever (heading survives).
    {
        const auto w = level_world();
        rigid_state s{};
        s.pos = dvec3(0.0, 1500.0, 0.0);
        s.omega_body = dvec3(0.0, 0.3, 0.0);
        const double y0 = s.omega_body.y;
        for (int k = 1; k <= 600; ++k) {
            const dvec3 g = sim::total_accel(w, s.pos, k);
            sim::integrate_step(s, p, g, auto_level_torque(g, s, sim::kArcade_assist_gains));
        }
        CHECK(s.omega_body.y == y0);           // bitwise untouched
        CHECK(s.omega_body == dvec3(0.0, y0, 0.0));
    }

    // ---- void: gravity exactly zero -> auto-level is a bitwise no-op
    // (returns the exact zero vector; no invented reference down).
    {
        rigid_state s{};
        s.att = math::quat_from_axis_angle(dvec3(0.3, 0.5, 0.8), 1.1);  // arbitrary tumbled att
        s.omega_body = dvec3(0.4, -0.2, 0.6);
        const dvec3 t = auto_level_torque(dvec3(0.0, 0.0, 0.0), s, sim::kArcade_assist_gains);
        CHECK(t == dvec3(0.0, 0.0, 0.0));
    }

    // ---- level hold is bit-exact: cross(cur_up, want_up) of parallel
    // vectors is exactly zero -> a level ship never gets perturbed.
    {
        const auto w = level_world();
        rigid_state s{};
        s.pos = dvec3(0.0, 3000.0, 0.0);
        const dvec3 g = sim::total_accel(w, s.pos, 1);
        CHECK(math::dot(g, dvec3(0.0, 1.0, 0.0)) < 0.0);  // down is -Y here
        const dvec3 t = auto_level_torque(g, s, sim::kArcade_assist_gains);
        CHECK(t == dvec3(0.0, 0.0, 0.0));
    }

    // ---- toggles ride the replay command stream: assist OFF tumbles the
    // whole run (torque-free |w| conserved), assist ON from tick 301 settles
    // it. Same log shape, different mask — replay remains the record.
    {
        const auto w = level_world();
        ReplayLog log_off;
        ReplayLog log_on;
        for (std::size_t i = 0; i < 1200; ++i) {
            // Hover thrust keeps pos above the core: without it the ship
            // free-falls through the center and local up flips (correct
            // behavior, but this pin is about assist toggles).
            tick_command c{};
            c.thrust_body = dvec3(0.0, 1.96e5, 0.0);
            c.torque_body = dvec3(2.0e3, 1.0e3, 0.0);  // thruster imbalance
            log_off.append(c);
            if (i >= 300)
                c.assist_mask = sim::kAssist_stabilizer | sim::kAssist_auto_level;
            log_on.append(c);
        }
        rigid_state a{}, b{};
        a.omega_body = dvec3(1.0, 0.0, 0.5);
        b.omega_body = a.omega_body;
        for (std::size_t i = 0; i < 1200; ++i) {
            sim::replay_step(w, log_off, i, static_cast<tick_t>(i) + 1, p, a);
            sim::replay_step(w, log_on, i, static_cast<tick_t>(i) + 1, p, b);
        }
        CHECK(length(a.omega_body) > 1.0);   // never stabilized: still tumbling
        // Stabilized steady state is the damping equilibrium against the
        // constant imbalance: |w| -> |tau|/stab_kd = sqrt(5e6)/1e5 = 0.0224.
        CHECK(length(b.omega_body) < 0.05);  // on for 900 ticks: settled at equilibrium
    }

    // ---- determinism: assist runs replayed twice hash bitwise-equal.
    {
        const auto w = level_world();
        const ReplayLog log = [] {
            ReplayLog l;
            for (int i = 0; i < 600; ++i) {
                tick_command c{};
                c.assist_mask = sim::kAssist_stabilizer | sim::kAssist_auto_level;
                c.thrust_body = dvec3(1000.0, 0.0, 0.0);
                l.append(c);
            }
            return l;
        }();
        auto run = [&] {
            rigid_state s{};
            s.pos = dvec3(0.0, 3000.0, 0.0);
            s.vel = dvec3(50.0, 0.0, 0.0);
            s.omega_body = dvec3(0.7, -0.9, 0.2);
            for (std::size_t i = 0; i < log.size(); ++i)
                sim::replay_step(w, log, i, static_cast<tick_t>(i) + 1, p, s);
            return sim::state_hash(601, s);
        };
        CHECK(run() == run());
    }
}
