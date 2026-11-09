// test: sim_rails — kinematic rails in core/sim (decision #17a, #45a/c).
// Body positions are analytic functions of the integer tick: hand-computed
// positions at pinned ticks, nested-parent chains (moon = planet pos +
// local), and the purity contract (same tick => bit-identical dvec3 with
// zero stored state). Pins carry derivations (AGENTS rule).

#include <cmath>

#include <doctest/doctest.h>

#include "ve/core/math/quat.h"
#include "ve/core/sim/rails.h"

using namespace ve::core;
using sim::body_def;
using sim::rail_world;
using sim::tick_t;

namespace {
body_def planet(double radius) {
    body_def b{};
    b.surface_radius = radius;
    b.well_radius = 10.0 * radius;  // sane defaults so add_body accepts
    b.surface_g = 9.8;
    b.well_taper_start = 5.0 * radius;
    return b;
}
} // namespace

TEST_CASE("sim_rails") {
    // ---- root anchor: exactly origin at ANY tick, bit-exact (zero offset,
    // never even touches cos/sin).
    rail_world w;
    const auto root = w.add_body(planet(1000.0));  // parent -1
    REQUIRE(root.has_value());
    for (const tick_t t : {tick_t(0), tick_t(1), tick_t(-1), tick_t(1'000'000)})
        CHECK(*w.position(*root, t) == math::dvec3(0, 0, 0));

    // ---- one circular rail: period 20 s (1200 ticks #45a), R = 4000 m,
    // phase 0. Hand pins, positive-about-+Y sense (#34: +X turns toward -Z):
    body_def moon = planet(500.0);
    moon.parent = *root;
    moon.orbit_period_s = 20.0;
    moon.orbit_radius_m = 4000.0;
    const auto m = w.add_body(moon);
    REQUIRE(m.has_value());
    // t=0: theta=0 exactly -> (R,0,0) bit-exact (cos 0 = 1, sin 0 = 0).
    CHECK(*w.position(*m, 0) == math::dvec3(4000, 0, 0));
    // t=300 = quarter period: theta = tau/4 -> (0, 0, -R) (forward-ish -Z).
    CHECK(math::approximately_equal(*w.position(*m, 300), math::dvec3(0, 0, -4000), 1e-7));
    // t=-300: negative scrub (replay/ghost evaluate any tick): (0, 0, +R).
    CHECK(math::approximately_equal(*w.position(*m, -300), math::dvec3(0, 0, 4000), 1e-7));
    // t=1200 = full period: back to (R, 0, ~0).
    CHECK(math::approximately_equal(*w.position(*m, 1200), math::dvec3(4000, 0, 0), 1e-7));

    // ---- rail spin == pinned rotation convention: at t=150 (theta = tau *
    // 2.5/20 = pi/4), the cos/sin rail offset must equal rotate(quat_yaw,
    // (R,0,0)) — two independent code paths pinning the same #34 sense.
    const auto rail150 = rail_world::local_rail_offset(*w.body(*m), 150);
    const auto quat150 = math::rotate(math::quat_yaw(math::kPi_d / 4.0), math::dvec3(4000, 0, 0));
    CHECK(math::approximately_equal(rail150, quat150, 1e-9));

    // ---- phase_rad: phase = pi/2 puts the body at (0, 0, -R) at tick 0.
    body_def ph = planet(500.0);
    ph.parent = *root;
    ph.orbit_period_s = 20.0;
    ph.orbit_radius_m = 4000.0;
    ph.orbit_phase_rad = math::kPi_d / 2.0;
    const auto pm = w.add_body(ph);
    REQUIRE(pm.has_value());
    CHECK(math::approximately_equal(*w.position(*pm, 0), math::dvec3(0, 0, -4000), 1e-7));

    // ---- nested chain (#17b shape): planet P around root (period 60 s =
    // 3600 ticks, R = 100 km), moon around P (period 10 s = 600 ticks,
    // R = 1000 m). At t=900: P at theta=tau/4 -> (0,0,-1e5); moon at
    // theta = tau*15/10 = 1.5*tau -> cos=-1 -> local (-1000, 0, ~0);
    // world = (-1000, 0, -100000). Moon rides its planet's moving position.
    body_def P1 = planet(2000.0);
    P1.parent = *root;
    P1.orbit_period_s = 60.0;
    P1.orbit_radius_m = 100000.0;
    const auto p1 = w.add_body(P1);
    REQUIRE(p1.has_value());
    body_def M2 = planet(300.0);
    M2.parent = *p1;
    M2.orbit_period_s = 10.0;
    M2.orbit_radius_m = 1000.0;
    const auto m2 = w.add_body(M2);
    REQUIRE(m2.has_value());
    CHECK(math::approximately_equal(*w.position(*m2, 900), math::dvec3(-1000, 0, -100000), 1e-6));
    // Composition invariant: moon world == planet world + moon local, and
    // position() sums root-first, so this is BIT-exact, not approximate.
    const auto pw = *w.position(*p1, 1234);
    const auto mw = *w.position(*m2, 1234);
    const auto ml = rail_world::local_rail_offset(*w.body(*m2), 1234);
    CHECK(mw == pw + ml);

    // ---- 4-level chain, all phase 0, t=0: exact sum of radii along +X
    // (cos 0 = 1 exactly; integer sums exact).
    rail_world deep;
    auto d0 = deep.add_body(planet(10.0));
    REQUIRE(d0.has_value());
    std::int32_t prev = *d0;
    for (double R : {10.0, 20.0, 30.0}) {
        body_def b = planet(1.0);
        b.parent = prev;
        b.orbit_period_s = 100.0;
        b.orbit_radius_m = R;
        const auto id = deep.add_body(b);
        REQUIRE(id.has_value());
        prev = *id;
    }
    CHECK(*deep.position(prev, 0) == math::dvec3(60, 0, 0));

    // ---- purity/determinism: position() stores nothing. Same (id, tick)
    // twice is bit-identical, and evaluating OTHER ticks in between cannot
    // perturb it (no state to perturb).
    const auto a = *w.position(*m2, 500'000);
    (void)w.position(*p1, 7);
    (void)w.position(*m, 12'345'678);
    const auto b2 = *w.position(*m2, 500'000);
    CHECK(a == b2);

    // ---- add_body rejects nonsense (core is exception-free; every reject
    // leaves body_count unchanged so ids stay stable).
    const auto before = w.body_count();
    body_def bad = planet(1.0);
    bad.parent = static_cast<std::int32_t>(before) + 5;  // dangling parent
    CHECK_FALSE(w.add_body(bad).has_value());
    bad = planet(1.0);
    bad.parent = -2;                                     // parent < -1
    CHECK_FALSE(w.add_body(bad).has_value());
    bad = planet(1.0);
    bad.parent = 0;
    bad.orbit_period_s = 0.0;                            // parented, no period
    CHECK_FALSE(w.add_body(bad).has_value());
    bad = planet(1.0);
    bad.parent = 0;
    bad.orbit_period_s = std::nan("");                   // non-finite
    CHECK_FALSE(w.add_body(bad).has_value());
    bad = planet(-5.0);                                  // radius <= 0
    CHECK_FALSE(w.add_body(bad).has_value());
    bad = planet(10.0);
    bad.well_radius = 8.0;                               // well inside surface
    CHECK_FALSE(w.add_body(bad).has_value());
    bad = planet(10.0);
    bad.well_taper_start = 500.0;                        // taper past well edge
    CHECK_FALSE(w.add_body(bad).has_value());
    CHECK(w.body_count() == before);
    // lookup guards
    CHECK(w.position(-1, 0) == std::nullopt);
    CHECK(w.position(static_cast<std::int32_t>(before), 0) == std::nullopt);
    CHECK(w.body(999) == nullptr);
}
