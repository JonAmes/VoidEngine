// test: sim_spin — body spin (decision #41) as an analytic pose in core/sim.
// Hand-pinned orientation at fixed ticks against closed-form yaw/pitch/roll,
// composition up the parent chain (a moon inherits its planet's rotation),
// purity/bit-stability, and the load-bearing regression pin: spin NEVER
// perturbs rail position()/velocity() — wells and safe bubbles stay
// position-only ("centers move, normals don't").

#include <cmath>
#include <limits>

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
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
} // namespace

TEST_CASE("sim_spin") {
    // ---- no spin authored: orientation is EXACTLY identity at any tick ----
    {
        rail_world w;
        const auto root = w.add_body(planet(1000.0));
        REQUIRE(root.has_value());
        for (const tick_t t : {tick_t(0), tick_t(-1), tick_t(1'000'000)})
            CHECK(*w.orientation(*root, t) == math::dquat{});  // bit-exact
    }

    // ---- one spinning root body, rate = 2*pi rad/s (1 rev / 60 ticks) ----
    // Hand pins (#45a tick = 1/60 s): tick 15 = 0.25 s -> angle pi/2 about
    // +Y = quat_yaw(pi/2) = (0, sin(pi/4), 0, cos(pi/4)); sin == cos == sqrt(2)/2.
    {
        rail_world w;
        body_def b = planet(1000.0);
        b.spin_rate_rad_s = math::kTau_d;
        const auto root = w.add_body(b);
        REQUIRE(root.has_value());

        const auto q = *w.orientation(*root, tick_t(15));
        constexpr double kHalfSqrt2 = 0.7071067811865476;  // sqrt(2)/2 nearest double
        CHECK(q.x == doctest::Approx(0.0).epsilon(1e-15));
        CHECK(q.y == doctest::Approx(kHalfSqrt2).epsilon(1e-15));
        CHECK(q.z == doctest::Approx(0.0).epsilon(1e-15));
        CHECK(q.w == doctest::Approx(kHalfSqrt2).epsilon(1e-15));

        // Convention pin: default-axis spin IS quat_yaw(rate * elapsed s),
        // bit-identical (axis {0,1,0} normalizes exactly).
        CHECK(q == math::quat_yaw(math::kTau_d * (15.0 / 60.0)));

        // Negative time: spin(-t) == conjugate(spin(+t)) exactly (sin is odd).
        CHECK(*w.orientation(*root, tick_t(-15)) == math::conjugate(q));

        // Purity: same tick => bit-identical, call after call.
        CHECK(*w.orientation(*root, tick_t(15)) == q);

        // Huge tick (10 days): still finite and unit-length (no pre-wrapping).
        const auto far = *w.orientation(*root, tick_t(864'000));
        CHECK(math::approximately_equal(math::length2(far), 1.0, 1e-12));
    }

    // ---- composition up the chain: child inherits parent spin ----
    // Planet spins at a, moon at b (both default +Y). authored in their
    // parent's frame. World attitude of the moon = yaw(a*t) * yaw(b*t).
    {
        rail_world w;
        body_def p = planet(1000.0);
        p.spin_rate_rad_s = 0.1;
        const auto planet_id = w.add_body(p);
        REQUIRE(planet_id.has_value());
        body_def m = planet(100.0);
        m.parent = *planet_id;
        m.orbit_period_s = 120.0;
        m.orbit_radius_m = 4000.0;
        m.spin_rate_rad_s = 0.3;
        const auto moon_id = w.add_body(m);
        REQUIRE(moon_id.has_value());

        const tick_t t(1000);
        const double tau = 1000.0 / 60.0;
        CHECK(*w.orientation(*moon_id, t) ==
              math::quat_yaw(0.1 * tau) * math::quat_yaw(0.3 * tau));
        CHECK(*w.orientation(*planet_id, t) == math::quat_yaw(0.1 * tau));

        // A non-spinning child inherits the parent's attitude exactly.
        body_def m2 = planet(50.0);
        m2.parent = *planet_id;
        m2.orbit_period_s = 60.0;
        m2.orbit_radius_m = 2000.0;
        const auto moon2 = w.add_body(m2);
        REQUIRE(moon2.has_value());
        CHECK(*w.orientation(*moon2, t) == *w.orientation(*planet_id, t));
    }

    // ---- authored non-default axis ----
    {
        rail_world w;
        body_def b = planet(1000.0);
        b.spin_axis = math::dvec3(0, 0, 1);  // +Z: closed form = quat_roll
        b.spin_rate_rad_s = 0.5;
        const auto z = w.add_body(b);
        REQUIRE(z.has_value());
        // Non-unit axis normalizes EXACTLY (3.0 scales the +Y unit), so the
        // eval must bit-equal the unit-axis yaw.
        body_def c = planet(500.0);
        c.spin_axis = math::dvec3(0, 3.0, 0);
        c.spin_rate_rad_s = 0.5;
        const auto y = w.add_body(c);
        REQUIRE(y.has_value());
        const tick_t t(1234);
        const double ang = 0.5 * (1234.0 / 60.0);
        CHECK(*w.orientation(*z, t) == math::quat_roll(ang));
        CHECK(*w.orientation(*y, t) == math::quat_yaw(ang));
    }

    // ---- add_body rejects broken spin defs (core is exception-free) ----
    {
        rail_world w;
        body_def b = planet(1000.0);
        b.spin_axis = math::dvec3(0, 0, 0);
        b.spin_rate_rad_s = 1.0;  // nonzero rate, zero axis: nonsense
        CHECK_FALSE(w.add_body(b).has_value());
        body_def c = planet(1000.0);
        c.spin_rate_rad_s = kNaN;
        CHECK_FALSE(w.add_body(c).has_value());
        body_def d = planet(1000.0);
        d.spin_axis = math::dvec3(kNaN, 1.0, 0.0);
        d.spin_rate_rad_s = 1.0;
        CHECK_FALSE(w.add_body(d).has_value());
        CHECK(w.body_count() == 0);  // rejected adds changed nothing
    }

    // ---- REGRESSION PIN (#41): spin never touches rail kinematics ----
    // Two worlds identical except spin; position() and velocity() must be
    // bit-identical at pinned ticks — the well/bubbles read position only.
    {
        auto build = [](double spin_planet, double spin_moon) {
            rail_world w;
            body_def p = planet(1000.0);
            p.well_radius = 20'000.0;
            p.well_taper_start = 6000.0;
            p.spin_rate_rad_s = spin_planet;
            const auto pid = *w.add_body(p);
            body_def m = planet(100.0);
            m.parent = pid;
            m.orbit_period_s = 90.0;
            m.orbit_radius_m = 5000.0;
            m.orbit_phase_rad = 0.7;
            m.spin_axis = math::dvec3(0, 1.0, 1.0);
            m.spin_rate_rad_s = spin_moon;
            const auto mid = *w.add_body(m);
            return std::pair{w, std::pair{pid, mid}};
        };
        const auto [wa, ids] = build(0.0, 0.0);
        const auto [wb, ids2] = build(1e-3, 3.5);
        CHECK(ids == ids2);
        for (const tick_t t : {tick_t(-7), tick_t(0), tick_t(1), tick_t(3600), tick_t(36'000)}) {
            CHECK(*wa.position(ids.first, t) == *wb.position(ids2.first, t));
            CHECK(*wa.position(ids.second, t) == *wb.position(ids2.second, t));
            CHECK(*wa.velocity(ids.first, t) == *wb.velocity(ids2.first, t));
            CHECK(*wa.velocity(ids.second, t) == *wb.velocity(ids2.second, t));
        }
        // ...but the attitudes DO differ where spin was added.
        CHECK(*wa.orientation(ids.second, tick_t(100)) !=
              *wb.orientation(ids2.second, tick_t(100)));
    }
}
