// Phase 5 task 5.3 — full-tensor rigid path (sim_tensor_dynamics). The
// diagonal path's bitwise identity is guarded by the UNTOUCHED Phase-4
// replay-hash pins (same suite, unchanged); here we validate the tensor
// branch against closed forms:
//  1. torque-free axisymmetric top, symmetry axis TILTED in body axes
//     (real off-diagonal coupling): body-frame omega precesses about the
//     symmetry axis at Omega = omega3*(I3-I1)/I1 — Rodrigues closed form.
//  2. tensor == diag -> same dynamics as the diagonal path (tight Approx).
//  3. xy-coupled tensor + pure-z torque -> omega_z = tau_z*t/Izz exactly,
//     omega_xy stays exactly zero (gyro term vanishes, I^-1 tau has no xy).
//  4. params_ok positive-definiteness gates (Sylvester leading minors).
#include <doctest/doctest.h>

#include <cmath>

#include "ve/core/sim/rigid.h"

namespace {

using namespace ve::core;
using math::cross;
using math::dmat3;
using math::dvec3;
using math::transpose;
using sim::integrate_step;
using sim::rigid_params;
using sim::rigid_state;

// right-handed rotation of `a` about unit axis `k` (Rodrigues matrix).
dmat3 rot_axis(dvec3 k, double a) {
    const double c = std::cos(a), s = std::sin(a);
    const double C = 1.0 - c;
    return dmat3{{c + k.x * k.x * C, k.y * k.x * C + k.z * s, k.z * k.x * C - k.y * s,
                  k.x * k.y * C - k.z * s, c + k.y * k.y * C, k.z * k.y * C + k.x * s,
                  k.x * k.z * C + k.y * s, k.y * k.z * C - k.x * s, c + k.z * k.z * C}};
}

constexpr double kDt = 1.0 / 60.0;  // canonical tick (kSim_tick_dt)

}  // namespace

TEST_CASE("sim_tensor_dynamics") {
    // ---- 1. torque-free axisymmetric top, tilted symmetry axis ----
    // principal (10, 10, 12); body axes = principal tilted by alpha=30 deg
    // about body x -> genuine off-diagonal Iyz coupling in the tensor.
    const double alpha = math::kPi_d / 6.0;
    const dmat3 Rx{{1, 0, 0,
                    0, std::cos(alpha), std::sin(alpha),
                    0, -std::sin(alpha), std::cos(alpha)}};  // rot about x by -alpha (columns are body axes of principal frame)
    const dmat3 I = Rx * dmat3::diag(dvec3(10, 10, 12)) * transpose(Rx);
    // off-diagonal coupling present (that's the whole point of this case):
    CHECK(std::fabs(I.m[1 * 3 + 2]) > 0.4);  // I_zy (col1,row2) nonzero

    const double I1 = 10.0, I3 = 12.0;
    const dvec3 e3 = Rx.col(2);  // symmetry axis in BODY coords
    CHECK(std::fabs(length(e3) - 1.0) < 1e-12);

    rigid_params p;
    p.mass = 100.0;
    p.inertia_tensor = I;
    REQUIRE(params_ok(p));

    rigid_state s;
    s.omega_body = dvec3(0.6, 0.8, 1.0);
    const double w3 = dot(s.omega_body, e3);           // component along symmetry axis (const)
    const double Omega = w3 * (I3 - I1) / I1;          // body-frame precession rate
    const int ticks = 600;                             // 10 s
    for (int i = 0; i < ticks; ++i)
        integrate_step(s, p, dvec3(0, 0, 0), dvec3(0, 0, 0));

    const dvec3 want = rot_axis(e3, Omega * ticks * kDt) * dvec3(0.6, 0.8, 1.0);
    CHECK(std::fabs(s.omega_body.x - want.x) < 1e-6);
    CHECK(std::fabs(s.omega_body.y - want.y) < 1e-6);
    CHECK(std::fabs(s.omega_body.z - want.z) < 1e-6);

    // conserved quantities: energy and |omega| (angle to e3 constant).
    const double e0 = 0.5 * dot(dvec3(0.6, 0.8, 1.0), I * dvec3(0.6, 0.8, 1.0));
    const double e1 = 0.5 * dot(s.omega_body, I * s.omega_body);
    CHECK(std::fabs(e1 - e0) < 1e-6);
    CHECK(std::fabs(length(s.omega_body) - length(dvec3(0.6, 0.8, 1.0))) < 1e-6);

    // ---- 2. diagonal tensor == diagonal params (dynamics match closely) ----
    rigid_params pd, pt;
    pd.mass = pt.mass = 50.0;
    pd.inertia_diag = dvec3(2, 3, 4);
    pt.inertia_tensor = dmat3::diag(dvec3(2, 3, 4));
    rigid_state sd, st;
    sd.omega_body = st.omega_body = dvec3(1.0, -0.5, 0.3);
    for (int i = 0; i < 300; ++i) {
        integrate_step(sd, pd, dvec3(0, 0, 0), dvec3(0, 0, 0));
        integrate_step(st, pt, dvec3(0, 0, 0), dvec3(0, 0, 0));
    }
    CHECK(std::fabs(sd.omega_body.x - st.omega_body.x) < 1e-10);
    CHECK(std::fabs(sd.omega_body.y - st.omega_body.y) < 1e-10);
    CHECK(std::fabs(sd.omega_body.z - st.omega_body.z) < 1e-10);

    // ---- 3. xy-coupled tensor, pure-z torque: closed form exact ----
    rigid_params pc;
    pc.mass = 10.0;
    pc.inertia_tensor = dmat3{{4, 1, 0,
                               1, 3, 0,
                               0, 0, 5}};  // column-major: Ixy=Iyx=1, Izz=5
    REQUIRE(params_ok(pc));
    rigid_state sc;
    for (int i = 0; i < 600; ++i)
        integrate_step(sc, pc, dvec3(0, 0, 0), dvec3(0, 0, 2.0));
    // gyro vanishes while omega rides z; I^-1 tau has no xy component:
    CHECK(sc.omega_body.z == doctest::Approx(2.0 / 5.0 * 10.0));  // tau/Izz * 10 s
    CHECK(sc.omega_body.x == 0.0);
    CHECK(sc.omega_body.y == 0.0);

    // ---- 4. params_ok gates (Sylvester leading principal minors) ----
    rigid_params bad;
    bad.inertia_tensor = dmat3::diag(dvec3(1, 1, -1));
    CHECK_FALSE(params_ok(bad));                      // det < 0
    bad.inertia_tensor = dmat3{{0, 0, 0, 0, 5, 0, 0, 0, 5}};
    CHECK_FALSE(params_ok(bad));                      // m00 == 0
    bad.inertia_tensor = dmat3{{1, 2, 0, 2, 1, 0, 0, 0, 5}};
    CHECK_FALSE(params_ok(bad));                      // 2x2 minor negative
    bad.inertia_tensor = dmat3{{1, 0, 0, 0, 1, 0, 0, 0, std::nan("")}};
    CHECK_FALSE(params_ok(bad));                      // non-finite entry
    // positive definite off-diagonal passes:
    bad.inertia_tensor = dmat3{{4, 1, 0, 1, 3, 0, 0, 0, 5}};
    CHECK(params_ok(bad));
}
