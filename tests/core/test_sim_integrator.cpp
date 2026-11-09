// Phase 4 task 4.6 — rigid body + flight integrator pins (sim_integrator).
#include <doctest/doctest.h>

#include <cmath>

#include "ve/core/math/quat.h"
#include "ve/core/sim/gravity.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/well_blend.h"

namespace {

using namespace ve::core;
using ve::core::math::dquat;  // nested-namespace names don't leak via using namespace
using ve::core::math::dvec3;
using sim::body_def;
using sim::rail_world;
using sim::rigid_params;
using sim::rigid_state;
using sim::tick_t;

// Hand-computed semi-implicit Euler displacement after N ticks from rest
// under constant accel a: dt^2 * N(N+1)/2 (vel += a*dt then pos += vel*dt).
constexpr double semi_implicit_drop(double a, int n) {
    return a / 3600.0 * (static_cast<double>(n) * (n + 1) / 2.0);
}

rigid_params hover_params() { return rigid_params{1.0, dvec3(1.0, 1.0, 1.0)}; }

// #17b-style testbed for the moving-well fall: root anchor + one planet on
// a slow 100 km rail (period 360000 s => 1.75 m/s drift at the ship).
void add_fall_world(rail_world& w) {
    body_def root{};
    root.surface_radius = 10.0;
    root.well_radius = 0.0;  // anchor: no well
    REQUIRE(w.add_body(root).has_value());
    body_def p{};
    p.surface_radius = 1000.0;
    p.well_radius = 13000.0;
    p.well_taper_start = 2000.0;
    p.surface_g = 9.8;
    p.parent = 0;
    p.orbit_period_s = 360000.0;
    p.orbit_radius_m = 100000.0;
    p.orbit_phase_rad = 0.0;
    REQUIRE(w.add_body(p).has_value());
}

} // namespace

TEST_CASE("sim_integrator") {
    // ---- coast: pick vel so vel*dt is EXACTLY dyadic: 15 m/s * (1/60) =
    // 0.25 m/tick exactly, and every 0.25-sum stays exact => the 240-tick
    // (4 s) run lands on pos.x exactly 60.0 with bitwise-unchanged vel.
    {
        rigid_state s{};
        s.vel = dvec3(15.0, 0.0, 0.0);
        const dquat att0 = s.att;
        for (int k = 0; k < 240; ++k)
            sim::integrate_step(s, hover_params(), dvec3(0, 0, 0), dvec3(0, 0, 0));
        CHECK(s.vel == dvec3(15.0, 0.0, 0.0));  // zero accel: vel untouched bitwise
        CHECK(s.pos == dvec3(60.0, 0.0, 0.0));  // straight line, exact dyadic sum
        CHECK(s.att == att0);                        // omega=0: attitude untouched bitwise
        CHECK(s.omega_body == dvec3(0, 0, 0));
        CHECK(sim::state_ok(s));
    }

    // ---- constant thrust vs hand-derived semi-implicit sums (a=-1 Y, 10 s):
    // vel = -N*dt = -10 ; pos = -dt^2*N(N+1)/2 = -50.08333...
    // The +1/2*a*t*dt = 1/12 bias vs the continuous 1/2*a*t^2 = -50 is the
    // documented semi-implicit curve, pinned not hidden.
    {
        rigid_state s{};
        for (int k = 0; k < 600; ++k)
            sim::integrate_step(s, hover_params(), dvec3(0, -1.0, 0), dvec3(0, 0, 0));
        CHECK(math::approximately_equal(s.vel.y, -10.0, 1e-9));
        CHECK(math::approximately_equal(s.pos.y, -semi_implicit_drop(1.0, 600), 1e-9));
        CHECK(math::approximately_equal(-s.pos.y - 50.0, 1.0 / 12.0, 1e-9));
    }

    // ---- free fall into a MOVING rail well, 600 ticks, envelope pins:
    // ship starts at rest 1500 m from the planet center (planet at (1e5,0,0),
    // ship (98500,0,0)); the core keeps g=9.8 the whole fall (r stays <2000)
    // while the planet drifts -Z at 1.75 m/s. Hand math: fall depth ~490.8 m
    // along +X, drift-induced tilt <1 deg => lateral vel ~0.5 m/s toward -Z.
    {
        rail_world w;
        add_fall_world(w);
        rigid_state s{};
        s.pos = dvec3(98500.0, 0.0, 0.0);
        const dvec3 zero(0, 0, 0);
        for (tick_t k = 1; k <= 600; ++k) {
            const dvec3 g = sim::total_accel(w, s.pos, k);
            sim::integrate_step(s, hover_params(), g, zero);
        }
        CHECK(s.pos.x > 98990.0);
        CHECK(s.pos.x < 98990.9);                        // depth ~490.8 (tilt-trimmed)
        CHECK(s.vel.x > 97.9);
        CHECK(s.vel.x < 98.01);                          // semi-implicit 9.8*10, cos<=1
        CHECK(s.vel.z < -0.05);
        CHECK(s.vel.z > -1.5);                           // pulled toward where the planet went
        const double r = math::length(s.pos - *w.position(1, 600));
        CHECK(r < 2000.0);                               // stayed inside the constant-g core
    }

    // ---- principal-axis spin is exact: omega rides an eigenvector of diag I,
    // gyro term omega x (I . omega) = 0 bitwise, so omega never changes and
    // attitude matches the closed-form yaw 5 rad/s * 10 s = 50 rad.
    {
        const rigid_params p{1.0, dvec3(10.0, 20.0, 30.0)};
        rigid_state s{};
        s.omega_body = dvec3(0, 5.0, 0);
        for (int k = 0; k < 600; ++k)
            sim::integrate_step(s, p, dvec3(0, 0, 0), dvec3(0, 0, 0));
        CHECK(s.omega_body == dvec3(0, 5.0, 0));
        const dquat e = math::quat_from_axis_angle(dvec3(0, 1, 0), 50.0);
        CHECK(std::fabs(s.att.w - e.w) < 1e-9);
        CHECK(std::fabs(s.att.x - e.x) < 1e-9);
        CHECK(std::fabs(s.att.y - e.y) < 1e-9);
        CHECK(std::fabs(s.att.z - e.z) < 1e-9);
    }

    // ---- torque-free spin near the MAJOR axis (largest I), modest rate:
    // |I.omega| conserved to within 0.1% over 10 s. This pin is also the
    // stability canary: forward-Euler omega blows the free top up (NaN in
    // minutes); midpoint holds the envelope. Slow-ish spin (lam~2 rad/s)
    // keeps the O(dt^4) amplitude pump under the band; high-rate tumbling
    // lives under assist authority (4.8).
    {
        const rigid_params p{1.0, dvec3(10.0, 20.0, 30.0)};
        rigid_state s{};
        s.omega_body = dvec3(0.2, 0.4, 2.0);
        const double L0_sq = 2.0 * 2.0 + 8.0 * 8.0 + 60.0 * 60.0;  // I.w0 = (2,8,60)
        for (int k = 0; k < 600; ++k)
            sim::integrate_step(s, p, dvec3(0, 0, 0), dvec3(0, 0, 0));
        const dvec3 L(s.omega_body.x * p.inertia_diag.x, s.omega_body.y * p.inertia_diag.y,
                      s.omega_body.z * p.inertia_diag.z);
        CHECK(std::fabs(L.x * L.x + L.y * L.y + L.z * L.z - L0_sq) < L0_sq * 1e-3);
        CHECK(s.omega_body.z > 1.5);   // still near-major (nutation happened, no flip)
        CHECK(s.omega_body.x != 0.2);  // gyroscopic coupling actually moved omega
    }

    // ---- bitwise determinism: identical inputs + tick count => identical
    // bits in every field (the 4.7 replay-hash contract rides this).
    {
        rail_world w;
        add_fall_world(w);
        rigid_state a{}, b{};
        a.pos = b.pos = dvec3(98500.0, 0.0, 0.0);
        const dvec3 zero(0, 0, 0);
        for (tick_t k = 1; k <= 300; ++k)
            sim::integrate_step(a, hover_params(), sim::total_accel(w, a.pos, k), zero);
        for (tick_t k = 1; k <= 300; ++k)
            sim::integrate_step(b, hover_params(), sim::total_accel(w, b.pos, k), zero);
        CHECK(a.pos == b.pos);
        CHECK(a.vel == b.vel);
        CHECK(a.att == b.att);
        CHECK(a.omega_body == b.omega_body);
    }

    // ---- validators gate bad authored input (integrator stays branch-light).
    CHECK_FALSE(sim::params_ok(rigid_params{0.0, dvec3(1, 1, 1)}));
    CHECK_FALSE(sim::params_ok(rigid_params{1.0, dvec3(1, -2, 1)}));
    CHECK(sim::params_ok(hover_params()));
    {
        rigid_state bad{};
        bad.pos.x = std::nan("");
        CHECK_FALSE(sim::state_ok(bad));
    }
}
