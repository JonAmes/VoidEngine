#include <doctest/doctest.h>

#include "ve/core/math/mat4.h"

using namespace ve::core::math;

// exact matrix compare for hand-exact cases (mult by I / add of zeros are exact)
static bool mat_equals(const mat4f& a, const mat4f& b) {
    for (int i = 0; i < 16; ++i) if (a.m[i] != b.m[i]) return false;
    return true;
}

TEST_CASE("math_mat4_ops") {
    // Column-major layout pin: translation lands in m[12..14] (slot order,
    // NOT row-major m[4,9,14]) — the memory contract gfx pushes on.
    mat4f T = translation(vec3f(1.f, 2.f, 3.f));
    CHECK(T.m[12] == 1.f); CHECK(T.m[13] == 2.f); CHECK(T.m[14] == 3.f);
    CHECK(T.m[0] == 1.f);  CHECK(T.m[5] == 1.f);  CHECK(T.m[10] == 1.f);
    CHECK(T.m[15] == 1.f); CHECK(T.m[1] == 0.f);

    // Identity acts as unit for * and for transforms.
    mat4f I;  // DEFAULT-CONTRACT PIN: default ctor is identity, never zero
    for (int i = 0; i < 16; ++i) CHECK(I.m[i] == (i % 5 == 0 ? 1.f : 0.f));
    CHECK(mat_equals(T * I, T));
    CHECK(mat_equals(I * T, T));

    // Translation composes by addition (T(a)*T(b) = T(a+b)) — hand check.
    CHECK(mat_equals(T * translation(vec3f(4.f, 5.f, 6.f)), translation(vec3f(5.f, 7.f, 9.f))));

    // w semantics: POINT (w=1) picks up translation, DIRECTION (w=0) does not.
    vec3f p = transform_point(T, vec3f(10.f, 10.f, 10.f));
    CHECK(approximately_equal(p, vec3f(11.f, 12.f, 13.f), 1e-6f));
    vec3f d = transform_direction(T, vec3f(10.f, 10.f, 10.f));
    CHECK(approximately_equal(d, vec3f(10.f, 10.f, 10.f), 1e-6f));

    // RH rotation about +Z by +90 deg maps +X -> +Y (basis pin at matrix level):
    mat4f rz = rotation_z(kHalfPi_f);
    CHECK(approximately_equal(transform_direction(rz, kAxisXf), kAxisYf, 1e-6f));
    // RH rotation about +Y by +90: -Z forward -> -X (matches 1.2's yawed case)
    mat4f ry = rotation_y(kHalfPi_f);
    CHECK(approximately_equal(transform_direction(ry, kForwardf), vec3f(-1.f, 0.f, 0.f), 1e-6f));

    // (A*B)^T == B^T*A^T, numerically (hand-cited identity, verified here).
    mat4f A = T * rz;
    mat4f B = scale(vec3f(2.f, 3.f, 4.f));
    CHECK(approximately_equal(transpose(A * B), transpose(B) * transpose(A), 1e-6f));

    // scale applies per-axis to directions:
    CHECK(approximately_equal(transform_direction(B, vec3f(1.f, 1.f, 1.f)),
                               vec3f(2.f, 3.f, 4.f), 1e-6f));

    // generic column-vector mult: M * (x,y,z,w) picks the w column for pure w.
    vec4f v = T * vec4f(0.f, 0.f, 0.f, 1.f);
    CHECK(approximately_equal(v, vec4f(1.f, 2.f, 3.f, 1.f), 1e-6f));

    // fp64 width works through the same templates (sim transforms).
    dmat4 Td = translation(dvec3(1.0e11, 0.0, 0.0));
    dvec3 pd = transform_point(Td, dvec3(1.0, 0.0, 0.0));
    CHECK(approximately_equal(pd.x, 1.0e11 + 1.0, 1e-9));   // fp32 would eat this
}

TEST_CASE("math_mat4_inverse") {
    // General inverse: M * inv(M) == I for a full non-singular TRS chain.
    mat4f M = translation(vec3f(2.f, -1.f, 3.f)) * rotation_y(deg_to_rad(30.f))
            * scale(vec3f(2.f, 3.f, 4.f));
    auto inv = invert(M);
    REQUIRE(inv.has_value());
    CHECK(approximately_equal(M * *inv, mat4f{}, 1e-5f));
    CHECK(approximately_equal(*inv * M, mat4f{}, 1e-5f));
    // round-trip transform: inv(M) * M * p == p (hand-cited contract)
    vec3f p(7.f, -2.f, 0.5f);
    CHECK(approximately_equal(transform_point(*inv, transform_point(M, p)), p, 1e-5f));

    // Singular matrix -> nullopt (core is exception-free; no throw on bad det).
    mat4f singular = scale(vec3f(1.f, 0.f, 1.f));
    CHECK_FALSE(invert(singular).has_value());
    mat4f zero;  // explicit zero matrix (default ctor is identity; pin it)
    for (auto& v : zero.m) v = 0.f;
    CHECK_FALSE(invert(zero).has_value());

    // Rigid fast inverse == general inverse within eps, for rot+translate only.
    mat4f rigid = translation(vec3f(-4.f, 2.f, 9.f)) * rotation_z(deg_to_rad(-67.f));
    auto fast = invert_rigid(rigid);
    auto gen = invert(rigid);
    REQUIRE(fast.has_value()); REQUIRE(gen.has_value());
    CHECK(approximately_equal(*fast, *gen, 1e-5f));
    // rigid inverse inverts the transform exactly (contract test):
    vec3f q(1.f, 2.f, 3.f);
    CHECK(approximately_equal(transform_point(*fast, transform_point(rigid, q)), q, 1e-5f));
    // a SCALED matrix is not rigid — fast inverse must report nullopt, not wrong math
    CHECK_FALSE(invert_rigid(M).has_value());

    // transpose of rotation = inverse for pure rotation (orthonormal identity):
    mat4f R = rotation_x(deg_to_rad(41.f));
    CHECK(approximately_equal(transpose(R), *invert(R), 1e-5f));
}
