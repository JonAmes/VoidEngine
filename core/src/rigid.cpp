#include "ve/core/sim/rigid.h"

#include <cmath>

namespace ve::core::sim {

using ve::core::math::cross;
using ve::core::math::dvec3;
using ve::core::math::is_finite;

bool params_ok(const rigid_params& p) {
    return is_finite(p.mass) && p.mass > 0.0 && is_finite(p.inertia_diag.x) && p.inertia_diag.x > 0.0 &&
           is_finite(p.inertia_diag.y) && p.inertia_diag.y > 0.0 &&
           is_finite(p.inertia_diag.z) && p.inertia_diag.z > 0.0;
}

bool state_ok(const rigid_state& s) {
    return is_finite(s.pos.x) && is_finite(s.pos.y) && is_finite(s.pos.z) &&
           is_finite(s.vel.x) && is_finite(s.vel.y) && is_finite(s.vel.z) &&
           is_finite(s.omega_body.x) && is_finite(s.omega_body.y) && is_finite(s.omega_body.z) &&
           is_finite(s.att.w) && is_finite(s.att.x) && is_finite(s.att.y) && is_finite(s.att.z);
}

void integrate_step(rigid_state& s, const rigid_params& p, const dvec3& accel_world,
                    const dvec3& torque_body) {
    constexpr double dt = kSim_tick_dt;
    // Linear: semi-implicit Euler — vel first, then pos with the NEW vel.
    s.vel = s.vel + accel_world * dt;
    s.pos = s.pos + s.vel * dt;
    // Angular: Euler's equations in the body frame with diagonal I (see
    // header), advanced by EXPLICIT MIDPOINT (RK2), NOT plain Euler: the
    // linearized top has imaginary eigenvalues (|1+i*lam*dt| > 1) so a
    // forward-Euler omega update pumps ~exp((lam*dt)^2/2 per tick) and the
    // free top spirals to NaN over minutes (found by test). Midpoint's
    // amplitude error is O((lam*dt)^4) per tick — bounded for arcade spin
    // rates — while staying one eval deeper, branch-free, deterministic.
    const dvec3& I = p.inertia_diag;
    const auto alpha = [&](const dvec3& w) {
        const dvec3 Iw(w.x * I.x, w.y * I.y, w.z * I.z);
        const dvec3 gyro = cross(w, Iw);
        return dvec3((torque_body.x - gyro.x) / I.x, (torque_body.y - gyro.y) / I.y,
                     (torque_body.z - gyro.z) / I.z);
    };
    const dvec3 w_mid = s.omega_body + alpha(s.omega_body) * (0.5 * dt);
    s.omega_body = s.omega_body + alpha(w_mid) * dt;
    // normalize-every-step keeps |q| drift bounded (pinned in 4.2).
    s.att = integrate_angular(s.att, s.omega_body, dt);
}

} // namespace ve::core::sim
