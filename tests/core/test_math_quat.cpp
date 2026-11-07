#include <cmath>

#include <doctest/doctest.h>

#include "ve/core/math/quat.h"

using namespace ve::core::math;

TEST_CASE("math_quat_ops") {
    // axis-angle sense (#34): +90 deg about +Z turns +X into +Y (thumb +Z,
    // fingers X->Y). rotate(q, v) = q v q*.
    const quatf qz = quat_roll(deg_to_rad(90.f));
    const vec3f r = rotate(qz, vec3f(1, 0, 0));
    CHECK(approximately_equal(r, vec3f(0, 1, 0), 1e-6f));

    // yaw +90 about +Y turns forward (-Z) to -X: turning "left" while
    // facing -Z. Hand-derived: Ry(90) * (0,0,-1) = (-1,0,0).
    const quatf qy = quat_yaw(deg_to_rad(90.f));
    CHECK(approximately_equal(rotate(qy, vec3f(0, 0, -1)), vec3f(-1, 0, 0), 1e-6f));

    // Hamilton composition order: (a*b) applies b first, then a —
    // same as mat4 M1*M2. yaw90 * roll90 on +X: roll90: X->Y; yaw90: Y->Y.
    const quatf combo = qy * qz;
    CHECK(approximately_equal(rotate(combo, vec3f(1, 0, 0)), vec3f(0, 1, 0), 1e-6f));
    // reversed order differs (non-commutative): roll90 after yaw90 on +X:
    // yaw90: X->-Z; roll90: -Z stays -Z.
    CHECK(approximately_equal(rotate(qz * qy, vec3f(1, 0, 0)), vec3f(0, 0, -1), 1e-6f));

    // inverse round-trip
    auto inv = invert_unit(combo);
    REQUIRE(inv.has_value());
    const quatf id = normalize(combo * *inv);
    CHECK(approximately_equal(std::abs(id.w), 1.f, 1e-6f));
    // non-unit input rejected (drifting integration must not pass silently)
    CHECK_FALSE(invert_unit(quatf{0.f, 0.f, 0.f, 2.f}).has_value());

    // zero-safe normalize: zero quat -> identity, never NaN
    const quatf z = normalize(quatf{0.f, 0.f, 0.f, 0.f});
    CHECK(z == quatf{});

    // to_mat4 agrees with the mat4 rotation ctor (same convention, two impls)
    CHECK(approximately_equal(to_mat4(qy), rotation_y(deg_to_rad(90.f)), 1e-6f));
    CHECK(approximately_equal(to_mat4(qz), rotation_z(deg_to_rad(90.f)), 1e-6f));

    // slerp endpoints + shortest arc (q == -q): identity -> yaw90 midpoint
    // must be yaw45.
    const quatf mid = slerp(quatf{}, qy, 0.5f);
    CHECK(approximately_equal(rotate(mid, vec3f(0, 0, -1)),
                              rotate(quat_yaw(deg_to_rad(45.f)), vec3f(0, 0, -1)), 1e-6f));
    const quatf neg_y{ -qy.x, -qy.y, -qy.z, -qy.w };
    CHECK(approximately_equal(rotate(slerp(quatf{}, neg_y, 0.75f), vec3f(1, 0, 0)),
                              rotate(slerp(quatf{}, qy,   0.75f), vec3f(1, 0, 0)), 1e-6f));
}

TEST_CASE("math_euler_roundtrip") {
    // notes.md convention: q = Yaw(Y) * Pitch(X) * Roll(Z), degrees.
    // Pin the order concretely: euler(90 yaw, 0, 0) must equal quat_yaw.
    CHECK(approximately_equal(rotate(quat_from_euler(90.f, 0.f, 0.f), vec3f(1, 0, 0)),
                              rotate(quat_yaw(deg_to_rad(90.f)), vec3f(1, 0, 0)), 1e-6f));

    // normal round-trip
    const quatf q = quat_from_euler(37.f, -22.f, 64.f);
    const auto e = quat_to_euler(q);
    CHECK(approximately_equal(e.yaw_deg, 37.f, 1e-4f));
    CHECK(approximately_equal(e.pitch_deg, -22.f, 1e-4f));
    CHECK(approximately_equal(e.roll_deg, 64.f, 1e-4f));

    // gimbal lock: pitch=90 has degenerate yaw/roll split; the fold
    // (roll=0) must still reproduce the same ROTATION.
    const quatf locked = quat_from_euler(45.f, 90.f, 20.f);
    const auto el = quat_to_euler(locked);
    CHECK(approximately_equal(el.pitch_deg, 90.f, 1e-3f));
    CHECK(approximately_equal(el.roll_deg, 0.f, 1e-3f));
    const quatf rebuilt = quat_from_euler(el.yaw_deg, el.pitch_deg, el.roll_deg);
    // same rotation <=> same action on basis vectors (q vs -q tolerated)
    for (vec3f v : {vec3f(1,0,0), vec3f(0,1,0), vec3f(0,0,1)}) {
        CHECK(approximately_equal(rotate(rebuilt, v), rotate(locked, v), 1e-4f));
    }

    // quat_from_mat4 (Shepperd) round-trips every quadrant, rejects scale
    const mat4f m = to_mat4(quat_from_euler(-140.f, 170.f, -80.f));
    auto q2 = quat_from_mat4(m);
    REQUIRE(q2.has_value());
    const mat4f m_src = to_mat4(quat_from_euler(-140.f, 170.f, -80.f));
    for (vec3f v : {vec3f(1,0,0), vec3f(0,1,0), vec3f(0,0,1)})
        CHECK(approximately_equal(rotate(*q2, v), transform_direction(m_src, v), 1e-4f));
    CHECK_FALSE(quat_from_mat4(scale(vec3f(2.f, 1.f, 1.f))).has_value());
}

TEST_CASE("math_rig_frames") {
    // yaw-90 rig: forward (-Z local) points -X world; up stays +Y
    drig rig;
    rig.position = {1.496e11, 0.0, 0.0};   // 1 AU: fp64 sim position
    rig.rotation = quat_yaw(deg_to_rad(90.f));
    CHECK(approximately_equal(rig.basis_forward(), vec3f(-1, 0, 0), 1e-6f));
    CHECK(approximately_equal(rig.basis_up(), vec3f(0, 1, 0), 1e-6f));
    // Ry(90) column 0 = (cos,0,-sin) = (0,0,-1): right swings to -Z
    CHECK(approximately_equal(rig.basis_right(), vec3f(0, 0, -1), 1e-6f));
    // to_model_matrix is the RENDER boundary: fp32, so it is only valid
    // camera-relative (origin utilities in 1.7 shift world->local first —
    // feeding it a raw AU-scale position would quantize at ~16 km, see
    // math_fp64_precision). Tested at local scale:
    rigf lrig;
    lrig.position = {1000.f, 200.f, -50.f};
    lrig.rotation = rig.rotation;
    const mat4f M = to_model_matrix(lrig);
    // local point (0,0,-2) = 2 m ahead -> world pos + forward*2 = (998,200,-50)
    const vec3f nose = transform_point(M, vec3f(0, 0, -2));
    CHECK(approximately_equal(nose, vec3f(998.f, 200.f, -50.f), 1e-3f));
    // translation slot pinned (m[12..14])
    CHECK(approximately_equal(M.m[12], 1000.f, 1e-3f));

    // body-omega integration: constant yaw rate PI/2 rad/s for 1s total ->
    // same as one-shot yaw90. Determinism: same fixed-step loop bit-matches
    // run to run (ordered ops, no wall clock).
    // total sweep = omega * T = (pi/2 rad/s) * 1.0 s = 90 deg yaw
    quatf q{};
    for (int i = 0; i < 100; ++i)
        q = integrate_angular(q, vec3f(0.f, static_cast<float>(kPi_d) / 2.0f, 0.f), 0.01f);
    const vec3f fwd_after = rotate(q, vec3f(0, 0, -1));
    CHECK(approximately_equal(fwd_after, vec3f(-1, 0, 0), 1e-3f));

    quatf q2{};
    for (int i = 0; i < 100; ++i)
        q2 = integrate_angular(q2, vec3f(0.f, static_cast<float>(kPi_d) / 2.0f, 0.f), 0.01f);
    CHECK(q == q2);   // identical fixed-step recipe -> bit-identical attitude

    // renormalize bounds drift: 10k steps stay unit within tight eps
    quatf qd{};
    for (int i = 0; i < 10000; ++i)
        qd = integrate_angular(qd, vec3f(0.1f, 0.2f, 0.05f), 0.016f);
    CHECK(approximately_equal(length2(qd), 1.f, 1e-5f));
}

TEST_CASE("math_frame_change") {
    // look_at_rh: camera at (5,0,0) looking at origin, up +Y.
    // view-space pins: eye -> origin; target -> straight ahead (0,0,-5);
    // a point above the target is +y in view; +Z world is to the LEFT
    // (camera faces -X world => right is -Z... hand-check: f=(-1,0,0),
    // s=cross(f,up)=cross((-1,0,0),(0,1,0))=(0,0,-1) => view +x = world -Z).
    const mat4f V = look_at_rh(vec3f(5, 0, 0), vec3f(0, 0, 0), vec3f(0, 1, 0));
    CHECK(approximately_equal(transform_point(V, vec3f(5, 0, 0)), vec3f(0, 0, 0), 1e-5f));
    CHECK(approximately_equal(transform_point(V, vec3f(0, 0, 0)), vec3f(0, 0, -5), 1e-5f));
    // world (0,3,0): 5 in front, 3 up -> view (0, 3, -5)
    CHECK(approximately_equal(transform_point(V, vec3f(0, 3, 0)), vec3f(0, 3, -5), 1e-5f));
    // world (0,0,-1), rel eye (-5,0,-1): x=dot(s,.)=+1 (world -Z is camera right),
    // z=dot(-f,.)=-5 (5 m ahead) -> (1, 0, -5)
    CHECK(approximately_equal(transform_point(V, vec3f(0, 0, -1)), vec3f(1, 0, -5), 1e-5f));

    // polar degenerate: looking straight up along +Y must not NaN
    const mat4f Vp = look_at_rh(vec3f(0, 0, 0), vec3f(0, 10, 0), vec3f(0, 1, 0));
    const vec3f ahead = transform_point(Vp, vec3f(0, 10, 0));
    CHECK(std::isfinite(ahead.x)); CHECK(std::isfinite(ahead.y)); CHECK(std::isfinite(ahead.z));
    CHECK(approximately_equal(ahead, vec3f(0, 0, -10), 1e-4f));

    // TRS round-trip: compose -> decompose -> compose is identity
    trsf t;
    t.position = {4.f, -2.f, 9.f};
    t.rotation = quat_from_euler(25.f, -60.f, 100.f);
    t.scale = {2.f, 0.5f, 7.f};
    const mat4f M = trs_compose(t);
    auto back = trs_decompose(M);
    REQUIRE(back.has_value());
    CHECK(approximately_equal(back->position, t.position, 1e-4f));
    CHECK(approximately_equal(back->scale, t.scale, 1e-4f));
    for (vec3f v : {vec3f(1,0,0), vec3f(0,1,0), vec3f(0,0,1)})
        CHECK(approximately_equal(rotate(back->rotation, v), rotate(t.rotation, v), 1e-4f));

    // shear is NOT TRS-decomposable: report, never silently approximate
    mat4f shear = mat4f{};          // identity
    shear.m[4] = 2.f;               // col1 += 2*row0... shear block
    CHECK_FALSE(trs_decompose(shear).has_value());

    // nlerp agrees with slerp for small angles and stays unit
    const quatf a = quat_yaw(0.01f), b = quat_yaw(0.02f);
    const quatf n = nlerp(a, b, 0.5f), s = slerp(a, b, 0.5f);
    for (vec3f v : {vec3f(1,0,0), vec3f(0,0,-1)})
        CHECK(approximately_equal(rotate(n, v), rotate(s, v), 1e-5f));
    CHECK(approximately_equal(length2(n), 1.f, 1e-6f));
}
