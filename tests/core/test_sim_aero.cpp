// test: sim_aero — 7.6 arcade atmosphere inside wells only (#17). Density
// curve rho0*exp(-h/H) pinned from an independent port (#40); #17 hard gates
// (no atmosphere authored / no well / outside well => ZERO force); pure-drag
// force pin bit-exact (rho=1, round numbers); forgiving stall (CL peaks at
// exactly lift_coeff at authored a_max, sin rolloff pinned at 1.5*a_max,
// exactly dead past 2*a_max); spin co-air wind pin; and the done-when
// hand-computed numbers: TERMINAL VELOCITY sqrt(2mg/(rho*A*CD)) reached as a
// monotone attractor through the Phase-4 integrator, and an equilibrium
// glide (tan(gamma)=CD/CL) that holds its descent angle tick after tick.

#include <cmath>
#include <numbers>

#include <doctest/doctest.h>
#include "ve/core/sim/aero.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"

namespace {

using namespace ve::core;

// Hand-authored atmosphere body (#45d): clean SI numbers so every pin is
// hand-computable. Big scale height for the integration blocks makes the
// fall air effectively uniform, so terminal velocity is an exact attractor.
sim::body_def world_body() {
    sim::body_def b;
    b.surface_radius = 1'000'000.0;
    b.well_radius = 1'200'000.0;
    b.surface_g = 10.0;
    b.well_taper_start = 1'100'000.0;  // existing add_body well-shape gate
    b.atm_rho0_kg_m3 = 1.0;
    b.atm_scale_height_m = 1'000'000'000'000.0;  // 1e12 m: near-uniform air for the
    // force/energy pins; the DENSITY CURVE itself is pinned separately
    // by direct calls at authored H=1000 below.
    return b;
}

sim::aero_params hull() {
    sim::aero_params a;
    a.ref_area_m2 = 2.0;
    a.drag_coeff = 0.5;
    a.lift_coeff = 1.0;
    a.max_aoa_rad = 0.2;
    return a;
}

} // namespace

TEST_CASE("sim_aero") {
    const math::dvec3 origin{};

    // ---- density curve: rho0=1, H=1000 (independent exp pins, #40) --------
    CHECK(sim::atmosphere_density(1.0, 1000.0, 0.0) == 1.0);  // exp(0)=1 exact
    CHECK(sim::atmosphere_density(1.0, 1000.0, 1000.0) == doctest::Approx(0.36787944117144233).epsilon(1e-15));
    CHECK(sim::atmosphere_density(1.0, 1000.0, 2000.0) == doctest::Approx(0.1353352832366127).epsilon(1e-15));
    CHECK(sim::atmosphere_density(1.0, 1000.0, -500.0) == doctest::Approx(1.6487212707001282).epsilon(1e-15));
    CHECK(sim::atmosphere_density(0.0, 1000.0, 0.0) == 0.0);    // airless authors zero
    CHECK(sim::atmosphere_density(1.0, 0.0, 0.0) == 0.0);       // degenerate scale
    CHECK(sim::atmosphere_density(-1.0, 1000.0, 0.0) == 0.0);  // garbage in => 0

    // ---- #17 hard gates ----------------------------------------------------
    {   // airless moon authors no atmosphere: zero force even inside the well
        sim::body_def moon = world_body();
        moon.atm_rho0_kg_m3 = 0.0;
        moon.atm_scale_height_m = 0.0;
        sim::rigid_state s;
        s.pos = {0.0, 1'000'000.0, 0.0};
        s.vel = {100.0, 0.0, 0.0};
        const auto r = sim::aero_forces(moon, origin, {}, s, hull());
        CHECK(r.force == math::dvec3{});
        CHECK(r.dyn_q == 0.0);
    }
    {   // outside the well: zero force (the gate, not the exp curve, bounds aero)
        sim::body_def b = world_body();
        b.atm_scale_height_m = 1000.0;  // would be ~0 by exp anyway; gate proves #17
        sim::rigid_state s;
        s.pos = {0.0, 1'300'000.0, 0.0};  // dist 1.3e6 > well 1.2e6
        s.vel = {100.0, 0.0, 0.0};
        const auto r = sim::aero_forces(b, origin, {}, s, hull());
        CHECK(r.force == math::dvec3{});
        // exactly ON the well radius is inside (strict > gate): nonzero.
        s.pos = {0.0, 1'200'000.0, 0.0};
        CHECK(sim::aero_forces(b, origin, {}, s, hull()).force.x != 0.0);
    }
    {   // authored atmosphere without a well is an add_body authoring error
        sim::body_def b = world_body();
        b.well_radius = 0.0;
        sim::rail_world rw;
        CHECK_FALSE(rw.add_body(b).has_value());
        b = world_body();  // with well: fine
        b.atm_rho0_kg_m3 = 0.0;
        b.atm_scale_height_m = 0.0;  // moon w/o well: legal
        CHECK(rw.add_body(b).has_value());
    }

    // ---- pure drag pin: fwd -Z (#34) straight along flow, aoa = 0 ----------
    // rho=1 at surface (alt 0): q = 1/2*1*100^2 = 5000; F = -q*A*CD*vhat =
    // (0,0,-5000) EXACT (round numbers, no transcendentals on the force path).
    {
        sim::rigid_state s;
        s.pos = {0.0, 0.0, 1'000'000.0};
        s.vel = {0.0, 0.0, -100.0};  // att identity: fwd = -Z = flow dir
        const auto r = sim::aero_forces(world_body(), origin, {}, s, hull());
        CHECK(r.aoa_rad == 0.0);
        CHECK(r.cl_eff == 0.0);      // sin(0) = 0 exact
        CHECK(r.dyn_q == 5000.0);
        CHECK(r.force.z == 5000.0);   // drag DECELERATES: body moves -Z => +Z
        CHECK(r.force.x == 0.0);
        CHECK(r.force.y == 0.0);
    }

    // ---- forgiving stall: peak CL == lift_coeff at authored a_max ----------
    // vel at angle 0.2 from -Z in the YZ plane; lift = q*A*CL along the nose-
    // side normal; expected F hand-derived: -5000*vhat + 10000*uhat.
    {
        const double a = 0.2;
        sim::rigid_state s;
        s.pos = {0.0, 0.0, 1'000'000.0};
        s.vel = {0.0, 100.0 * std::sin(a), -100.0 * std::cos(a)};
        const auto r = sim::aero_forces(world_body(), origin, {}, s, hull());
        CHECK(r.aoa_rad == doctest::Approx(0.2).epsilon(1e-12));
        CHECK(r.cl_eff == doctest::Approx(1.0).epsilon(1e-12));  // sin(pi/2) peak
        CHECK(r.force.y == doctest::Approx(-10794.012432387723).epsilon(1e-12));
        CHECK(r.force.z == doctest::Approx(2913.639581255596).epsilon(1e-12));
        CHECK(r.force.x == 0.0);
    }
    {   // 1.5*a_max: sin rolloff to sqrt(2)/2 exactly (pin from port)
        const double a = 0.3;  // 1.5 * a_max
        sim::rigid_state s;
        s.pos = {0.0, 0.0, 1'000'000.0};
        s.vel = {0.0, 100.0 * std::sin(a), -100.0 * std::cos(a)};
        const auto r = sim::aero_forces(world_body(), origin, {}, s, hull());
        CHECK(r.cl_eff == doctest::Approx(0.7071067811865476).epsilon(1e-12));
    }
    {   // dead past 2*a_max: CL EXACTLY zero (guard, no sin call), drag lives
        sim::rigid_state s;
        s.pos = {0.0, 0.0, 1'000'000.0};
        s.vel = {0.0, 0.0, 100.0};  // flow straight up the nose: aoa = pi
        const auto r = sim::aero_forces(world_body(), origin, {}, s, hull());
        CHECK(r.aoa_rad == doctest::Approx(std::numbers::pi).epsilon(1e-15));
        CHECK(r.cl_eff == 0.0);
        CHECK(r.force.z == -5000.0);  // pure drag, opposing v_rel (+Z) => -Z
    }

    // ---- spin co-air (#41): 1e-6 rad/s about +Y, r = 1e6 m (+Z) ------------
    // omega x r = (1,0,0) m/s EXACT; body at rest feels a +X headwind =>
    // drag along -X: F = -q*A*CD*xhat, q = 1/2*1*1^2 = 0.5 => F = (-0.5,0,0).
    {
        const auto wind = sim::coair_velocity(1e-6, {0.0, 1.0, 0.0}, origin, {0.0, 0.0, 1'000'000.0});
        CHECK(wind.x == 1.0);
        CHECK(wind.y == 0.0);
        CHECK(wind.z == 0.0);
        sim::rigid_state s;
        s.pos = {0.0, 0.0, 1'000'000.0};
        const auto r = sim::aero_forces(world_body(), origin, wind, s, hull());
        CHECK(r.speed_rel == 1.0);
        CHECK(r.dyn_q == 0.5);
        CHECK(r.force.x == 0.5);  // +X wind pushes the resting body downstream
        CHECK(r.force.y == 0.0);
        CHECK(r.force.z == 0.0);
        // zero rate => bit-exact still air
        CHECK(sim::coair_velocity(0.0, {0.0, 1.0, 0.0}, origin, {5.0, 7.0, 11.0}) == math::dvec3{});
    }

    // ---- terminal velocity THROUGH the integrator (done-when number) -------
    // m=1000, g=10, rho=1, A=2, CD=0.5 => v_t = sqrt(2*10000/(1*2*0.5)) =
    // 141.4213562373095. Fall straight down (-Y); att identity puts aoa at
    // pi/2 (>= 2*a_max) => pure drag, no lift. Hand-checked force balance at
    // v_t: q*A*CD = 1/2*20000*2*0.5 = 10000 N = m*g EXACT (vt^2 = 20000.0).
    {
        const double vt = 141.4213562373095;
        CHECK(vt * vt == 20000.0);  // the attractor closes by hand, not luck
        sim::rigid_params p;
        p.mass = 1000.0;
        sim::rigid_state s;
        s.pos = {0.0, 1'000'000.0, 0.0};
        const sim::body_def b = world_body();
        const auto h = hull();
        double prev_vy = 0.0;
        for (int i = 0; i < 10800; ++i) {  // 180 s at 1/60 (tau = v_t/2g ~ 7 s)
            const auto aero = sim::aero_forces(b, origin, {}, s, h);
            const math::dvec3 accel{0.0, -10.0 + aero.force.y / p.mass, 0.0};
            sim::integrate_step(s, p, accel, {});
            // Monotone speed-up (fp sink jitter at the attractor +-ulp) and
            // NEVER an overshoot of the analytic attractor.
            CHECK(std::fabs(s.vel.y) >= std::fabs(prev_vy) - 1e-9);
            CHECK(s.vel.y > -vt);
            prev_vy = s.vel.y;
        }
        // Converged: residual vs the hand vt = 1.6e-6, exactly the vt_eff
        // lag of sinking 24 km in H=1e12 air (port-verified), not a trend.
        CHECK(std::fabs(s.vel.y + vt) < 1e-5);
    }

    // ---- equilibrium glide (done-when number) ------------------------------
    // aoa = a_max (CL=1): tan(gamma) = CD/CL = 0.5 => gamma = 0.4636476090008061;
    // L = m*g*cos(gamma) => v_g = sqrt(2*m*g*cos(gamma)/(rho*A*CL)) =
    // 94.57416090031758. Attitude = Rz(b), b = atan2(-fwd.x, -fwd.y), pins the
    // nose 0.2 rad ABOVE the flow. Net aero+weight at that state is ~3e-12 N
    // (port-computed); the sim holds the descent angle for 600 ticks.
    {
        // tan(gamma) = CD/CL = 0.5: gamma = 0.4636476090008061 (comment only;
        // pins below are the port-computed doubles at that angle).
        const double vg = 94.57416090031758;
        sim::rigid_params p;
        p.mass = 1000.0;
        sim::rigid_state s;
        s.pos = {0.0, 1'000'000.0, 0.0};
        s.vel = {84.58970107524513, -42.294850537622565, 0.0};
        // Axis-angle taking body -Z to the nose heading (cos fa, sin fa, 0),
        // fa = -gamma + a_max: axis = normalize(sin fa, -cos fa, 0), angle
        // pi/2 (port-verified to reproduce fwd to fp64, #40).
        s.att = math::quat_from_axis_angle({-0.26060384662486136, -0.965445821951872, 0.0},
                                           1.5707963267948966);
        const sim::body_def b = world_body();
        const auto h = hull();
        const auto first = sim::aero_forces(b, origin, {}, s, h);
        CHECK(first.aoa_rad == doctest::Approx(0.2).epsilon(1e-12));
        const math::dvec3 net = first.force + math::dvec3{0.0, -10000.0, 0.0};
        CHECK(std::fabs(net.x) < 1e-8);  // balanced glide, by construction
        CHECK(std::fabs(net.y) < 1e-8);
        for (int i = 0; i < 600; ++i) {
            const auto aero = sim::aero_forces(b, origin, {}, s, h);
            const math::dvec3 accel{aero.force.x / p.mass, -10.0 + aero.force.y / p.mass, 0.0};
            sim::integrate_step(s, p, accel, {});
        }
        const double spd = math::length(s.vel);
        CHECK(std::fabs(spd - vg) < 1e-4);                    // phugoid tail ~1e-5
        CHECK(std::fabs(s.vel.y / spd + 0.4472135954999579) < 1e-6);  // descent angle holds
    }

    // ---- degenerate input => zero force, never NaN, never a throw ----------
    {
        sim::rigid_state s;
        s.pos = {0.0, 0.0, 1'000'000.0};
        s.vel = {100.0, 0.0, 0.0};
        CHECK(sim::aero_forces(world_body(), origin, {}, s, {0.0, 0.5, 1.0, 0.0}).force == math::dvec3{});   // a_max = 0
        CHECK(sim::aero_forces(world_body(), origin, {}, s, {0.0, 0.5, 1.0, 2.0}).force == math::dvec3{});   // a_max >= pi/2
        CHECK(sim::aero_forces(world_body(), origin, {}, s, {-1.0, 0.5, 1.0, 0.2}).force == math::dvec3{});  // area <= 0
        s.vel = {std::nan(""), 0.0, 0.0};
        const auto r = sim::aero_forces(world_body(), origin, {}, s, hull());
        CHECK(r.force == math::dvec3{});
        s.vel = {100.0, 0.0, 0.0};
        s.pos = {std::nan(""), 0.0, 0.0};
        CHECK(sim::aero_forces(world_body(), origin, {}, s, hull()).force == math::dvec3{});
        s.pos = {0.0, 0.0, 1'000'000.0};
        s.vel = {};  // no relative flow at all
        const auto q = sim::aero_forces(world_body(), origin, {}, s, hull());
        CHECK(q.force == math::dvec3{});
        CHECK(q.dyn_q == 0.0);
        CHECK(q.speed_rel == 0.0);
    }

    // ---- purity: bit-identical repeated evaluation --------------------------
    {
        sim::rigid_state s;
        s.pos = {123.0, 999'111.0, -456.0};
        s.vel = {-70.0, 5.5, 33.0};
        s.att = math::quat_from_axis_angle({0.3, 0.6, -0.5}, 0.77);
        const auto a = sim::aero_forces(world_body(), {10.0, 0.0, 0.0}, {1.0, 2.0, 3.0}, s, hull());
        const auto b2 = sim::aero_forces(world_body(), {10.0, 0.0, 0.0}, {1.0, 2.0, 3.0}, s, hull());
        CHECK(a.force.x == b2.force.x);
        CHECK(a.force.y == b2.force.y);
        CHECK(a.force.z == b2.force.z);
        CHECK(a.dyn_q == b2.dyn_q);
        CHECK(a.aoa_rad == b2.aoa_rad);
        CHECK(a.cl_eff == b2.cl_eff);
    }
}
