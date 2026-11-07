#include <doctest/doctest.h>

#include "ve/core/math/vec.h"

using namespace ve::core::math;

TEST_CASE("math_vec3_ops") {
    // hand-derived values throughout (comments carry derivations)
    vec3f a{1.f, 2.f, 3.f}, b{4.f, 5.f, 6.f};
    CHECK((a + b) == vec3f(5.f, 7.f, 9.f));
    CHECK((b - a) == vec3f(3.f, 3.f, 3.f));
    CHECK(-a == vec3f(-1.f, -2.f, -3.f));
    CHECK(a * 2.f == vec3f(2.f, 4.f, 6.f));
    CHECK(2.f * a == vec3f(2.f, 4.f, 6.f));
    CHECK(a / 2.f == vec3f(0.5f, 1.f, 1.5f));

    // dot: 1*4+2*5+3*6 = 32
    CHECK(dot(a, b) == doctest::Approx(32.f));
    // cross((1,2,3),(4,5,6)) = (2*6-3*5, 3*4-1*6, 1*5-2*4) = (-3, 6, -3)
    CHECK(cross(a, b) == vec3f(-3.f, 6.f, -3.f));

    CHECK(length2(b) == doctest::Approx(77.f));       // 16+25+36
    CHECK(length(vec3f(3.f, 4.f, 0.f)) == doctest::Approx(5.f)); // 3-4-5

    // normalize(3,4,0) = (0.6, 0.8, 0)
    CHECK(approximately_equal(normalize(vec3f(3.f, 4.f, 0.f)), vec3f(0.6f, 0.8f, 0.f), 1e-6f));
    // zero-safe contract (documented): zero in -> zero out, no NaN
    vec3f z = normalize(vec3f{});
    CHECK(is_finite(z.x)); CHECK(z == vec3f{});

    CHECK(approximately_equal(lerp(vec3f{0.f, 0.f, 0.f}, vec3f{10.f, -10.f, 1.f}, 0.25f),
                               vec3f(2.5f, -2.5f, 0.25f), 1e-6f));

    // project (2,0,0) onto (1,1,0): ((2,0)·(1,1))/2 = 1 -> (1,1,0)
    CHECK(approximately_equal(project(vec3f(2.f, 0.f, 0.f), vec3f(1.f, 1.f, 0.f)),
                               vec3f(1.f, 1.f, 0.f), 1e-6f));
    // reject = a - proj = (1,-1,0) — perpendicular: dot(rej, onto) == 0
    vec3f r = reject(vec3f(2.f, 0.f, 0.f), vec3f(1.f, 1.f, 0.f));
    CHECK(approximately_equal(r, vec3f(1.f, -1.f, 0.f), 1e-6f));
    CHECK(approximately_equal(dot(r, vec3f(1.f, 1.f, 0.f)), 0.f, 1e-6f));
    // project onto zero vector -> zero (contract)
    CHECK(project(vec3f(1.f, 2.f, 3.f), vec3f{}) == vec3f{});

    // vec2 + vec4 sanity: w-semantics constructors
    CHECK(vec4f::point(vec3f(1.f, 2.f, 3.f)) == vec4f(1.f, 2.f, 3.f, 1.f));
    CHECK(vec4f::direction(vec3f(1.f, 2.f, 3.f)) == vec4f(1.f, 2.f, 3.f, 0.f));
    CHECK(dot(vec2f(3.f, 4.f), vec2f(3.f, 4.f)) == doctest::Approx(25.f));
    CHECK(length(vec2f(3.f, 4.f)) == doctest::Approx(5.f));

    // fp64 alias works through the same templates (sim positions)
    dvec3 p{1.0e9, 2.0e9, 3.0e9}, q{1.0e9 + 1.0, 2.0e9, 3.0e9};
    CHECK(approximately_equal(p.x + 1.0, q.x, 1e-9));
}

TEST_CASE("math_convention_handedness") {
    // PLAN decision #34: right-handed, Y-up, -Z forward. Pinned here — if a
    // later refactor flips a sign anywhere in the basis, these fail.
    CHECK(cross(kAxisXf, kAxisYf) == kAxisZf);   // RH: X x Y = Z
    CHECK(cross(kAxisYf, kAxisZf) == kAxisXf);   // cyclic
    CHECK(cross(kAxisZf, kAxisXf) == kAxisYf);
    CHECK(cross(kAxisYf, kAxisXf) == -kAxisZf);  // anti-commutative
    CHECK(cross(kAxisXf, kAxisXf) == vec3f{});

    // -Z forward, +Y up -> +X right (the slice cockpit convention):
    CHECK(kForwardf == vec3f(0.f, 0.f, -1.f));
    CHECK(right_of(kForwardf, kAxisYf) == kAxisXf);
    // and the same identity holds at an authored attitude (hand-derived:
    // forward yawed 90 deg RH about +Y turns -Z toward -X):
    vec3f fwd_y90{-1.f, 0.f, 0.f};
    CHECK(approximately_equal(right_of(fwd_y90, kAxisYf), vec3f(0.f, 0.f, -1.f), 1e-6f));

    // up is perpendicular to forward (rig invariant)
    CHECK(approximately_equal(dot(kForwardf, kAxisYf), 0.f, 1e-6f));
}
