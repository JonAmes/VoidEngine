#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>
#include <limits>

#include "ve/core/math/base.h"

using namespace ve::core::math;

TEST_CASE("math_scalars") {
    // tau == 2*pi exactly at both widths (hand check: definitions above).
    CHECK(kTau_d == doctest::Approx(2.0 * kPi_d).epsilon(1e-15));
    CHECK(kHalfPi_d == doctest::Approx(kPi_d / 2.0).epsilon(1e-15));
    CHECK(approximately_equal<double>(static_cast<double>(kPi_f), kPi_d, 1e-7));

    // deg<->rad exact anchors (hand-derived: 180deg = pi, 90 = pi/2, 360 = tau)
    CHECK(approximately_equal(deg_to_rad(180.0), kPi_d));
    CHECK(approximately_equal(deg_to_rad(90.0), kHalfPi_d));
    CHECK(approximately_equal(deg_to_rad(360.0), kTau_d));
    CHECK(approximately_equal(deg_to_rad(0.0), 0.0));
    CHECK(approximately_equal(deg_to_rad(-45.0), -kPi_d / 4.0));
    // round-trip identity
    for (double d : {-720.0, -33.3, 0.0, 45.0, 359.9, 1000.0})
        CHECK(approximately_equal(rad_to_deg(deg_to_rad(d)), d));

    // approximately_equal regime switch at |x|=1 (notes.md contract).
    // Absolute regime: near zero, eps is what it says.
    CHECK(approximately_equal(0.0, 1e-13));           // 1e-13 < 1e-12 abs
    CHECK_FALSE(approximately_equal(0.0, 1e-11));     // exceeds absolute eps
    // Relative regime: at 1 AU (1.496e11 m) two values 1 m apart must NOT be
    // equal (real-world meaning), but 1e-2 m apart at 1e11 must be
    // (below relative eps = 1e-12*1e11 = 0.1).
    CHECK_FALSE(approximately_equal(1.496e11, 1.496e11 + 1.0));
    CHECK(approximately_equal(1.496e11, 1.496e11 + 1e-3));
    // exact equality short-circuits diff==0 even for huge values
    CHECK(approximately_equal(1.496e11, 1.496e11));
    // Double default is kDefaultEpsD=1e-12 (not the float 1e-6): diff 1e-7 at
    // scale 1 is *outside* double default eps -> not equal; 1e-13 is inside.
    CHECK_FALSE(approximately_equal(-1.0, -1.0000001));
    CHECK(approximately_equal(-1.0, -1.0000000000001));
    CHECK(approximately_equal(-1.0f, -1.0000001f));   // float eps 1e-6 covers 1.2e-7

    CHECK(is_finite(1e300));
    CHECK_FALSE(is_finite(std::numeric_limits<double>::infinity()));
    CHECK_FALSE(is_finite(std::numeric_limits<double>::quiet_NaN()));

    CHECK(lerp(0.0, 10.0, 0.25) == 2.5);
    CHECK(lerp(2.0f, -2.0f, 0.5f) == 0.0f);
    CHECK(clamp(5.0, 0.0, 1.0) == 1.0);
    CHECK(clamp(-5.0, 0.0, 1.0) == 0.0);
    CHECK(clamp(0.5, 0.0, 1.0) == 0.5);
}

TEST_CASE("math_angles") {
    // Wrap band [-pi, pi): boundary rule +pi -> -pi (half-open, documented).
    CHECK(approximately_equal(wrap_angle(kPi_d), -kPi_d));
    CHECK(approximately_equal(wrap_angle(-kPi_d), -kPi_d));
    CHECK(approximately_equal(wrap_angle(0.0), 0.0));
    CHECK(approximately_equal(wrap_angle(kHalfPi_d), kHalfPi_d));
    CHECK(approximately_equal(wrap_angle(-kHalfPi_d), -kHalfPi_d));

    // Multi-turn winds (hand-derived): 3pi = 2pi + pi -> -pi boundary;
    // 5pi/2 = 2pi + pi/2 -> pi/2; -3pi/2 = -2pi + pi/2 -> pi/2;
    // -5pi/2: fmod keeps dividend sign -> -pi/2 + ... lands at -pi/2 (one
    // sub of tau from the +3pi/2 intermediate), NOT +pi/2.
    CHECK(approximately_equal(wrap_angle(3.0 * kPi_d), -kPi_d));
    CHECK(approximately_equal(wrap_angle(2.5 * kPi_d), kHalfPi_d));
    CHECK(approximately_equal(wrap_angle(-1.5 * kPi_d), kHalfPi_d));
    CHECK(approximately_equal(wrap_angle(-2.5 * kPi_d), -kHalfPi_d));

    // One full turn maps to 0 within fp error.
    CHECK(approximately_equal(wrap_angle(kTau_d), 0.0, 1e-12));

    // Many turns: fmod keeps precision where naive subtraction would drift.
    CHECK(approximately_equal(wrap_angle(1000.0), wrap_angle(1000.0 - 159.0 * kTau_d), 1e-12));

    // Float width agrees with double width inside float eps.
    CHECK(approximately_equal(static_cast<double>(wrap_angle(kPi_f)), wrap_angle(kPi_d), 1e-6));
    CHECK(approximately_equal(wrap_angle(270.0f * (kPi_f / 180.0f)), -kHalfPi_f, 1e-6f));

    // wrap is idempotent (used per-tick on accumulated heading)
    const double a = 12.34;
    CHECK(wrap_angle(wrap_angle(a)) == doctest::Approx(wrap_angle(a)).epsilon(1e-15));
}
