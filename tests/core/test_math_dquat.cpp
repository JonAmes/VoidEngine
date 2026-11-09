// test: math_dquat_ops — fp64 sim-attitude surface (decision #45b, Phase 4
// task 4.2): the Phase 1 templates exercised at double width for the first
// time. Convention is identical to the fp32 pins (decision #34: RH, Y-up,
// -Z fwd; Hamilton (a*b) applies b first). Every expected value is
// hand-derived, derivation in the comment above each pin (AGENTS rule).

#include <cmath>

#include <doctest/doctest.h>

#include "ve/core/math/quat.h"

using namespace ve::core::math;

TEST_CASE("math_dquat_ops") {
    // ---- axis-angle sense at double: +90 deg about +Z turns +X into +Y
    // (thumb = +Z, fingers curl X->Y). rotate(q, v) = q v q*.
    const dquat qz = quat_roll(kHalfPi_d);
    CHECK(approximately_equal(rotate(qz, dvec3(1, 0, 0)), dvec3(0, 1, 0), 1e-15));

    // yaw +90 about +Y turns forward (-Z) to -X (hand-derived: Ry(90)*(0,0,-1)).
    const dquat qy = quat_yaw(kHalfPi_d);
    CHECK(approximately_equal(rotate(qy, dvec3(0, 0, -1)), dvec3(-1, 0, 0), 1e-15));

    // ---- composition order: (a*b) applies b first — non-commutative pin,
    // same two paths as the fp32 suite. yaw90*roll90 on +X: roll90: X->Y;
    // yaw90: Y->Y. reversed: yaw90: X->-Z; roll90 leaves -Z.
    const dquat combo = qy * qz;
    CHECK(approximately_equal(rotate(combo, dvec3(1, 0, 0)), dvec3(0, 1, 0), 1e-15));
    CHECK(approximately_equal(rotate(qz * qy, dvec3(1, 0, 0)), dvec3(0, 0, -1), 1e-15));

    // ---- inverse round-trip on a unit quat: q * invert_unit(q) = identity
    const dquat u = normalize(combo);
    const auto inv = invert_unit(u);
    REQUIRE(inv.has_value());
    const dquat id = u * *inv;
    CHECK(approximately_equal(id.x, 0.0, 1e-15));
    CHECK(approximately_equal(id.y, 0.0, 1e-15));
    CHECK(approximately_equal(id.z, 0.0, 1e-15));
    CHECK(approximately_equal(id.w, 1.0, 1e-15));

    // ---- invert_unit rejects a drifted quat (length2 off by 1e-2 >> the
    // 1e-3 gate): integration bugs must fail loud, not rotate at wrong scale.
    dquat drifted = combo;
    drifted.w += 1e-2;
    CHECK_FALSE(invert_unit(drifted).has_value());

    // ---- normalize zero-safe: zero quat -> identity (never NaN; the sim
    // feeds this from every fixed step).
    const dquat safe = normalize(dquat{0, 0, 0, 0});
    CHECK(approximately_equal(safe.w, 1.0, 1e-15));

    // ---- euler (deg) <-> quat round-trip at double, away from gimbal lock.
    // quat_to_euler is the exact inverse of quat_from_euler per Phase 1.
    const dquat qe = quat_from_euler(37.5, -22.25, 11.125);
    const auto e = quat_to_euler(qe);
    CHECK(approximately_equal(e.yaw_deg, 37.5, 1e-9));
    CHECK(approximately_equal(e.pitch_deg, -22.25, 1e-9));
    CHECK(approximately_equal(e.roll_deg, 11.125, 1e-9));

    // gimbal lock (pitch -> 90): yaw/roll split is degenerate; the fold rule
    // puts the residual in yaw with roll=0, but the *rotation* must survive
    // exactly — compare the composed rotation, not the folded angles.
    const dquat lock = quat_from_euler(50.0, 90.0, 30.0);
    const auto el = quat_to_euler(lock);
    CHECK(approximately_equal(el.roll_deg, 0.0, 1e-9));
    const dquat lock2 = quat_from_euler(el.yaw_deg, el.pitch_deg, el.roll_deg);
    for (const dvec3 v : {dvec3(1, 0, 0), dvec3(0, 1, 0), dvec3(0, 0, -1)})
        CHECK(approximately_equal(rotate(lock, v), rotate(lock2, v), 1e-12));

    // ---- quat <-> dmat4 both ways (Shepperd extraction at double):
    // re-extracted quat may be q or -q (same rotation) — pin by |dot|.
    const dmat4 m = to_mat4(qe);
    const auto back = quat_from_mat4(m);
    REQUIRE(back.has_value());
    CHECK(approximately_equal(std::fabs(dot(qe, *back)), 1.0, 1e-12));
    // non-rotational matrix rejected: scaled block is a data error.
    dmat4 scaled = m;
    for (int i = 0; i < 12; ++i) scaled.m[i] *= 2.0;
    CHECK_FALSE(quat_from_mat4(scaled).has_value());

    // ---- integrate_angular over a LONG fp64 horizon (the replay-hash basis).
    // omega_body = (0, 1, 0) rad/s constant, dt = 1/60 s, N = 10'000 steps.
    // Derivation: q_{n+1} = q_n * exp(omega*dt) right-multiplies the same
    // +Y axis every step (q_0 = identity keeps the axis world-aligned), and
    // same-axis rotations compose additively: q_N = quat_yaw(N*dt mod 2pi).
    // Pin A: normalize-every-step keeps |q| == 1 with drift bounded far below
    // fp32 (10k steps at double).
    constexpr double dt = 1.0 / 60.0;  // #45a canonical tick
    dquat q{};
    for (int i = 0; i < 10000; ++i)
        q = integrate_angular(q, dvec3(0, 1, 0), dt);
    CHECK(approximately_equal(length(q), 1.0, 1e-12));
    // Pin B: accumulated attitude equals the closed form (wrap only changes
    // the quaternion's parameterization, not the rotation — compare via v').
    const dquat expected = quat_yaw(wrap_angle(10000 * dt));
    for (const dvec3 v : {dvec3(1, 0, 0), dvec3(0, 1, 0), dvec3(0, 0, -1)})
        CHECK(approximately_equal(rotate(q, v), rotate(expected, v), 1e-11));
    // Pin C: omega = 0 is a no-op step (exact identity, bit-stable).
    CHECK(integrate_angular(q, dvec3(0, 0, 0), dt) == q);

    // ---- bitwise determinism: same inputs -> bit-identical doubles, twice,
    // including the fp64 integrate loop (the replay-hash assumption, #5).
    dquat qa{};
    dquat qb{};
    for (int i = 0; i < 1000; ++i) {
        qa = integrate_angular(qa, dvec3(0.1, -2.0, 0.7), dt);
        qb = integrate_angular(qb, dvec3(0.1, -2.0, 0.7), dt);
    }
    CHECK(qa == qb);
}
