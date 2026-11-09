// test: sim_gravity_field — arcade well profile (decision #22, Q4): constant
// surface_g core, C1 smoothstep ramp (g = surface_g * (1-u)^2 (1+2u)), exactly
// 0 outside; rail-sampled center. Pins are hand-derived (AGENTS rule).

#include <cmath>

#include <doctest/doctest.h>

#include "ve/core/sim/gravity.h"

using namespace ve::core;
using sim::body_def;
using sim::rail_world;
using sim::tick_t;

namespace {
// Hand-authored testbed body (no world cooking in Phase 4, #45d):
// surface 1000 m, well edge 4000 m, ramp from 2000 m, 9.8 m/s^2.
body_def well_body() {
    body_def b{};
    b.surface_radius = 1000.0;
    b.well_radius = 4000.0;
    b.well_taper_start = 2000.0;
    b.surface_g = 9.8;
    return b;
}
} // namespace

TEST_CASE("sim_gravity_field") {
    const body_def b = well_body();

    // ---- 1D profile, hand-derived values (bit-exact where the arithmetic
    // is exact: constant-core copies and the u=0.5 midpoint).
    CHECK(sim::well_g_magnitude(b, 0.0) == 9.8);        // center: constant core
    CHECK(sim::well_g_magnitude(b, 1000.0) == 9.8);     // surface
    CHECK(sim::well_g_magnitude(b, 2000.0) == 9.8);     // taper start (inclusive)
    // r=3000: u = 1000/2000 = 0.5 exactly -> (1-u)^2 (1+2u) = 0.25*2 = 0.5
    // -> g = 9.8 * 0.5 = 4.9 (exactly representable path).
    CHECK(sim::well_g_magnitude(b, 3000.0) == 4.9);
    // Outside: exactly zero, including ON the edge.
    CHECK(sim::well_g_magnitude(b, 4000.0) == 0.0);
    CHECK(sim::well_g_magnitude(b, 4001.0) == 0.0);

    // ---- monotone non-increasing, never negative, across the whole span.
    double prev = 1e9;
    for (double r = 0.0; r <= 4100.0; r += 100.0) {
        const double g = sim::well_g_magnitude(b, r);
        CHECK(g >= 0.0);
        CHECK(g <= prev);
        prev = g;
    }

    // ---- C1 at the well edge: quadratic falloff 1-s = (1-u)^2 (1+2u) makes
    // the finite-difference slope -> 0. edge slope must be ~0 and dwarfed by
    // the mid-ramp slope (analytic 9.8 * 6u(1-u)/2000 at u=0.5 = 0.00735).
    const double edge_slope = std::fabs((sim::well_g_magnitude(b, 3999.999) - 0.0) / 0.001);
    CHECK(edge_slope < 1e-6);
    const double mid_slope = (sim::well_g_magnitude(b, 2999.0) - sim::well_g_magnitude(b, 3001.0)) / 2.0;
    CHECK(math::approximately_equal(mid_slope, 0.00735, 1e-7)); // central diff of cubic: exact
    CHECK(edge_slope * 100.0 < mid_slope);
    // ---- C1 at the taper join (slope continuous with the constant core):
    // 1-s ~= 3u^2 near u=0 -> central-diff slope at delta=1e-3 m is ~1.5e-8.
    const double taper_slope = std::fabs((sim::well_g_magnitude(b, 2001.001) - sim::well_g_magnitude(b, 2000.999)) / 2.0);
    CHECK(taper_slope < 1e-6);

    // ---- vector form toward the center (root body at origin):
    // above the surface at +Z: pull is exactly -Z surface_g.
    const dvec3 c(0, 0, 0);
    CHECK(math::approximately_equal(sim::well_accel_at(b, c, dvec3(0, 0, 1000)), dvec3(0, 0, -9.8), 1e-12));
    // mid-ramp on three axes: same magnitude 4.9, direction inward (spherical
    // symmetry — magnitude depends on r only).
    CHECK(math::approximately_equal(sim::well_accel_at(b, c, dvec3(3000, 0, 0)), dvec3(-4.9, 0, 0), 1e-12));
    CHECK(math::approximately_equal(sim::well_accel_at(b, c, dvec3(0, 0, -3000)), dvec3(0, 0, 4.9), 1e-12));
    const dvec3 diag = sim::well_accel_at(b, c, dvec3(0, 2121.320343559643, 2121.320343559643)); // 3000/sqrt2
    CHECK(math::approximately_equal(length(diag), 4.9, 1e-9));
    // outside the well: EXACTLY the zero vector (no -0/NaN noise leaking into
    // integrator state), and at the center there is no direction to pull.
    CHECK(sim::well_accel_at(b, c, dvec3(0, 0, 4500)) == dvec3(0, 0, 0));
    CHECK(sim::well_accel_at(b, c, c) == dvec3(0, 0, 0));
    // no-well body: zero everywhere.
    body_def rocky = well_body();
    rocky.well_radius = 0.0;
    CHECK(sim::well_accel_at(rocky, c, dvec3(0, 0, 10)) == dvec3(0, 0, 0));

    // ---- rail-sampled center (#17a): orbiting planet, period 60 s = 3600
    // ticks, R = 100 km. At t=900 the center is (0, 0, -1e5); a ship at
    // (0, 0, -99000) sits 1000 m from the center -> 9.8 m/s^2 toward it.
    rail_world w;
    const auto root = w.add_body([](body_def d) { d.well_radius = 0.0; return d; }(well_body()));
    REQUIRE(root.has_value());
    body_def P = well_body();
    P.parent = *root;
    P.orbit_period_s = 60.0;
    P.orbit_radius_m = 100000.0;
    const auto p = w.add_body(P);
    REQUIRE(p.has_value());
    CHECK(math::approximately_equal(sim::well_accel(w, *p, dvec3(0, 0, -99000), 900), dvec3(0, 0, -9.8), 1e-9));
    // Sampling identity: the rail-sampled form is bit-identical to feeding
    // position(id, t) into the pure form (one code path, no drift).
    const auto a1 = sim::well_accel(w, *p, dvec3(12345, 678, -90000), 31'415);
    const auto a2 = sim::well_accel_at(*w.body(*p), *w.position(*p, 31'415), dvec3(12345, 678, -90000));
    CHECK(a1 == a2);
    // Determinism: same inputs twice -> bit-identical.
    CHECK(sim::well_accel(w, *p, dvec3(0, 0, -99000), 900) == sim::well_accel(w, *p, dvec3(0, 0, -99000), 900));
}
