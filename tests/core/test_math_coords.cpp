#include <cmath>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/math/coords.h"

using namespace ve::core::math;

TEST_CASE("math_coord_conversions") {
    // az frame pins: az=0 -> +X, az=+pi/2 -> +Z (around +Y, #34 Y-up)
    CHECK(approximately_equal(spherical_to_cart(spherical<double>{1.0, 0.0, 0.0}), dvec3(1, 0, 0), 1e-12));
    CHECK(approximately_equal(spherical_to_cart(spherical<double>{1.0, kHalfPi_d, 0.0}), dvec3(0, 0, 1), 1e-12));
    CHECK(approximately_equal(spherical_to_cart(spherical<double>{2.0, 0.0, kHalfPi_d}), dvec3(0, 2, 0), 1e-12));

    // round-trips
    for (dvec3 v : {dvec3(3, 4, 12), dvec3(-1e9, -0.5, 7e6), dvec3(0, -5, 0)}) {
        const auto s = cart_to_spherical(v);
        CHECK(approximately_equal(spherical_to_cart(s), v, 1e-9 * length(v)));
        CHECK(s.range >= 0.0);
        CHECK(s.el_rad >= -kHalfPi_d - 1e-12);
        CHECK(s.el_rad <= kHalfPi_d + 1e-12);
    }

    // polar singularity: poles -> az PINNED to 0 (deterministic, no NaN)
    const auto pole = cart_to_spherical(dvec3(0, 1000, 0));
    CHECK(approximately_equal(pole.el_rad, kHalfPi_d, 1e-12));
    CHECK(pole.az_rad == 0.0);
    const auto spole = cart_to_spherical(dvec3(0, -1000, 0));
    CHECK(approximately_equal(spole.el_rad, -kHalfPi_d, 1e-12));
    CHECK(spole.az_rad == 0.0);

    // origin -> all-zero spherical, no NaN
    const auto o = cart_to_spherical(dvec3(0, 0, 0));
    CHECK(o.range == 0.0); CHECK(o.az_rad == 0.0); CHECK(o.el_rad == 0.0);

    // azimuth stays in [-pi, pi): just past the -X side wraps
    const auto w = cart_to_spherical(dvec3(-1, 0, -1e-9));
    CHECK(w.az_rad >= -kPi_d - 1e-12);
    CHECK(w.az_rad < kPi_d);
}

TEST_CASE("math_origin_shift") {
    // docking formation at 1 AU: flagship + escort holding mm-level offsets
    // (exactly the formation case decision #22 cares about)
    constexpr double kAU = 1.496e11;
    const dvec3 flagship{kAU, 0.0, 0.0};
    const dvec3 escort{kAU + 0.001, 0.002, -0.0005};   // mm offsets, world-absolute

    origin_frame of;   // origin at world 0
    const dvec3 huge = of.to_local(flagship);
    // fp32 quantum at 1 AU is ~16 km: local coord is NOT render-ready
    CHECK(std::abs(static_cast<float>(huge.x) - huge.x) > 1.0);

    // recenter onto the flagship; WORLD POSITIONS NEVER CHANGE
    CHECK(of.needs_shift(flagship, 1e6));
    const dvec3 delta = of.shift_to(flagship);
    CHECK_FALSE(of.needs_shift(flagship, 1e6));
    CHECK(delta == dvec3{-kAU, 0.0, 0.0});

    // pairwise deltas preserved: at 1 AU fp64 spacing is 3.05e-5 m, so the
    // mm formation offsets survive with micron slack (Sterbenz-exact nearby
    // subtractions; compare against the TRUE offset 0.001 -> only the
    // original ADD at kAU scale rounded, by <= half an ulp)
    const dvec3 esc_local = of.to_local(escort);
    const dvec3 fl_local = of.to_local(flagship);
    CHECK(length((esc_local - fl_local) - dvec3(0.001, 0.002, -0.0005)) < 1e-4);

    // flagship local is ~0; escort local is mm-scale and fp32-safe to
    // micron — THIS is why the floating origin exists (fp32 model matrices)
    CHECK(length(fl_local) < 1e-9);
    CHECK(std::abs(static_cast<float>(esc_local.x) - esc_local.x) < 1e-6);

    // cached-locals contract: old local + delta == new local (bit-exact here,
    // the shift is the same number for every entity)
    const dvec3 old_esc_local = huge + dvec3(0.001, 0.002, -0.0005);  // pre-shift (approx)
    const dvec3 new_esc_local = of.to_local(escort);
    CHECK(std::abs(length((old_esc_local + delta) - new_esc_local)) < 1e-4);

    // determinism: same inputs -> same delta, in independent frames
    origin_frame a, b;
    CHECK(a.shift_to(flagship) == b.shift_to(flagship));

    // to_world inverts to_local exactly (nearby fp64 sub/add pair, Sterbenz)
    CHECK(of.to_world(new_esc_local) == escort);
}
