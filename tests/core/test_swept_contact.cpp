// test: swept_contact — #45e continuous point/capsule vs oriented component
// bounds (OBB). Core assertions are the done-when (d) guarantees: continuous
// TOI resolved inside one fixed tick (1/60), NO TUNNELING at any speed step
// (10 orders of magnitude pinned), swept-vs-moving-target cases (the Phase 9
// ballistics + Phase 8 warp dependency). Slab-clipped TOIs pin bit-exact for
// axis-aligned identity-orient cases; rotated cases ride 1e-12 (quat libm
// ulp, #40). Capsule corner behavior pins the documented EARLY-hit bound:
// L-inf slab expansion hits at most r*(sqrt(2)-1) of path early — never late.

#include <cmath>
#include <numbers>

#include <doctest/doctest.h>

#include "ve/core/math/quat.h"
#include "ve/core/sim/swept.h"

using namespace ve::core;
using sim::obb;
using sim::sweep_capsule_vs_obb;
using sim::sweep_point_vs_obb;
using sim::swept_hit;

namespace {
constexpr double kDt = 1.0 / 60.0;  // one fixed tick (#45)

obb unit_box() {
    obb b{};
    b.half_extents = math::dvec3(1.0, 1.0, 1.0);
    return b;
}
} // namespace

TEST_CASE("swept_contact") {
    const obb box = unit_box();

    // ---- head-on: TOI bit-exact, normal faces the incoming sweep ----
    {
        const auto hit = *sweep_point_vs_obb(math::dvec3(-5, 0, 0), math::dvec3(600, 0, 0), kDt, box);
        CHECK(hit.t == (-1.0 - (-5.0)) / 600.0);  // slab division, same bits
        CHECK(hit.normal == math::dvec3(-1, 0, 0));
        CHECK(math::approximately_equal(hit.pos, math::dvec3(-1, 0, 0), 1e-12));
        CHECK(hit.t <= kDt);
    }

    // ---- no tunneling at any speed step (done-when (d)) ----
    for (const double scale : {1.0, 1e3, 1e6, 1e9, 1e12}) {
        const auto hit = sweep_point_vs_obb(math::dvec3(-5, 0, 0), math::dvec3(600 * scale, 0, 0), kDt, box);
        REQUIRE(hit.has_value());  // one tick crosses the box 1e10+ times over
        CHECK(math::approximately_equal(hit->t, 4.0 / (600.0 * scale), 1e-15));
        CHECK(math::approximately_equal(hit->pos.x, -1.0, 1e-12));  // entry point, not pass-through
        CHECK(hit->t < kDt);
    }
    // hit lands next tick: nullopt (resolved then, not tunneled silently)
    CHECK(!sweep_point_vs_obb(math::dvec3(-5, 0, 0), math::dvec3(60, 0, 0), kDt, box).has_value());

    // ---- miss / graze ----
    CHECK(!sweep_point_vs_obb(math::dvec3(-5, 2, 0), math::dvec3(600, 0, 0), kDt, box).has_value());
    {  // grazing exactly along the slab boundary still reports the entry
        const auto hit = sweep_point_vs_obb(math::dvec3(-5, 1, 0), math::dvec3(600, 0, 0), kDt, box);
        REQUIRE(hit.has_value());
        CHECK(hit->t == 4.0 / 600.0);
    }

    // ---- start inside: t = 0, exit-plane normal negated (push-out) ----
    {
        const auto hit = *sweep_point_vs_obb(math::dvec3(0.5, 0, 0), math::dvec3(600, 0, 0), kDt, box);
        CHECK(hit.t == 0.0);
        CHECK(hit.normal == math::dvec3(-1, 0, 0));
    }
    // inside with zero relative velocity: no sweep event
    CHECK(!sweep_point_vs_obb(math::dvec3(0, 0, 0), math::dvec3(0, 0, 0), kDt, box).has_value());

    // ---- swept vs MOVING target (Phase 9 ballistics / Phase 8 warp) ----
    {
        // receding target: TOI from the RELATIVE closing speed
        const auto hit = *sweep_point_vs_obb(
            math::dvec3(-5, 0, 0), math::dvec3(600, 0, 0), kDt, box, math::dvec3(100, 0, 0));
        CHECK(hit.t == 4.0 / 500.0);
        CHECK(hit.normal == math::dvec3(-1, 0, 0));
        // target faster than the pursuer: no contact this tick
        CHECK(!sweep_point_vs_obb(math::dvec3(-5, 0, 0), math::dvec3(600, 0, 0), kDt, box,
                                  math::dvec3(700, 0, 0)).has_value());
        // head-on approach of the target: closing speed adds
        const auto head = *sweep_point_vs_obb(
            math::dvec3(-5, 0, 0), math::dvec3(600, 0, 0), kDt, box, math::dvec3(-400, 0, 0));
        CHECK(head.t == 4.0 / 1000.0);
    }

    // ---- capsule: face TOI shifts by exactly radius/speed ----
    {
        const auto hit = *sweep_capsule_vs_obb(math::dvec3(-5, 0, 0), math::dvec3(600, 0, 0), kDt, 0.5, box);
        CHECK(hit.t == (-1.5 - (-5.0)) / 600.0);
        CHECK(hit.normal == math::dvec3(-1, 0, 0));
    }

    // ---- corner: L-inf expansion hits EARLY by at most r*(sqrt2-1), never late ----
    {
        const double r = 0.5;
        const double speed = 600.0;
        const double c = speed / std::sqrt(2.0);  // per-axis component
        const auto hit = *sweep_capsule_vs_obb(math::dvec3(-7, -7, 0), math::dvec3(c, c, 0), kDt, r, box);
        CHECK(hit.t == (-1.5 - (-7.0)) / c);  // slab entry (shared sqrt2 bits)
        // true sphere-vs-edge corner touch: center at -(1 + r/sqrt2) per axis
        const double s_true = -(1.0 + r / std::sqrt(2.0));
        const double t_true = (s_true - (-7.0)) / c;
        CHECK(hit.t <= t_true);  // NEVER late (tunneling impossible)
        const double lead = (t_true - hit.t) * speed;
        CHECK(lead <= r * (std::sqrt(2.0) - 1.0) + 1e-9);  // documented bound
        CHECK(lead > 0.0);  // corners really are conservative
    }

    // ---- oriented bounds: 90 deg about +Y maps local x-half 2 to world z ----
    {
        obb b{};
        b.half_extents = math::dvec3(2.0, 1.0, 1.0);
        b.orient = math::quat_from_axis_angle(math::dvec3(0, 1, 0), std::numbers::pi_v<double> / 2.0);
        const auto hit = *sweep_point_vs_obb(math::dvec3(0, 0, -5), math::dvec3(0, 0, 600), kDt, b);
        CHECK(hit.t == doctest::Approx(3.0 / 600.0).epsilon(1e-12));  // |z| = 2 face
        CHECK(math::approximately_equal(hit.normal, math::dvec3(0, 0, -1), 1e-12));
    }

    // ---- degenerate input: nullopt, never a throw, never a NaN hit ----
    {
        obb bad = unit_box();
        bad.half_extents = math::dvec3(-1, 1, 1);
        CHECK(!sweep_point_vs_obb(math::dvec3(-5, 0, 0), math::dvec3(600, 0, 0), kDt, bad).has_value());
        CHECK(!sweep_point_vs_obb(math::dvec3(-5, 0, 0), math::dvec3(std::nan(""), 0, 0), kDt, box).has_value());
        CHECK(!sweep_point_vs_obb(math::dvec3(0, 0, 0), math::dvec3(600, 0, 0), -1.0, box).has_value());
        CHECK(!sweep_point_vs_obb(math::dvec3(0, 0, 0), math::dvec3(600, 0, 0), std::nan(""), box).has_value());
        CHECK(!sweep_capsule_vs_obb(math::dvec3(-5, 0, 0), math::dvec3(600, 0, 0), kDt, std::nan(""), box).has_value());
        obb scaled = unit_box();
        scaled.orient.w = 2.0;  // grossly non-unit attitude is rejected
        CHECK(!sweep_point_vs_obb(math::dvec3(-5, 0, 0), math::dvec3(600, 0, 0), kDt, scaled).has_value());
    }

    // ---- purity: identical calls, bit-identical hits ----
    {
        const auto a = sweep_capsule_vs_obb(math::dvec3(-5, -3, 2.5), math::dvec3(431, 250, -300), kDt, 0.4, box);
        const auto b = sweep_capsule_vs_obb(math::dvec3(-5, -3, 2.5), math::dvec3(431, 250, -300), kDt, 0.4, box);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(a->t == b->t);
        CHECK(a->pos == b->pos);
        CHECK(a->normal == b->normal);
    }
}
