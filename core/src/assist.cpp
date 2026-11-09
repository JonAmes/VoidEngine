#include "ve/core/sim/assist.h"

#include <cmath>

#include "ve/core/math/quat.h"

namespace ve::core::sim {

using ve::core::math::dvec3;

dvec3 auto_level_torque(const dvec3& accel_world, const rigid_state& s, const assist_gains& g) {
    const double a2 = length2(accel_world);
    if (a2 == 0.0)
        return {};  // void: no reference down — bitwise no-op (header rule)

    // Tilt error as a world-frame rotation vector: cross(current_up, want_up)
    // = sin(theta) * axis — small-angle ~ theta*axis, and exactly 0 when
    // level (cross of parallel vectors), so the hold case is bit-exact.
    const dvec3 want_up = accel_world * (-1.0 / std::sqrt(a2));
    const dvec3 cur_up = math::rotate(s.att, dvec3(0.0, 1.0, 0.0));
    const dvec3 e_world = cross(cur_up, want_up);
    const dvec3 e_body = math::rotate(math::conjugate(s.att), e_world);

    // Damp only the tilt-plane ω: remove the body-ŷ (yaw) component so
    // auto-level never fights heading. Exact decoupling while level.
    const dvec3 w_tilt(s.omega_body.x, 0.0, s.omega_body.z);
    return dvec3(g.level_kp * e_body.x - g.level_kd * w_tilt.x,
                 g.level_kp * e_body.y - g.level_kd * w_tilt.y,
                 g.level_kp * e_body.z - g.level_kd * w_tilt.z);
}

dvec3 stabilizer_torque(const rigid_state& s, const assist_gains& g) {
    return s.omega_body * (-g.stab_kd);
}

namespace {
// Clamp accel magnitude to the assist's thrust budget, preserving direction.
dvec3 clamp_mag(const dvec3& a, double cap) {
    const double l2 = length2(a);
    if (l2 <= cap * cap || l2 == 0.0) return a;
    return a * (cap / std::sqrt(l2));
}
} // namespace

dvec3 air_brake_accel(const dvec3& vel, const dvec3& frame_vel, const assist_gains& g) {
    return clamp_mag((vel - frame_vel) * (-g.ab_k), g.max_accel);
}

dvec3 autopilot_accel(const dvec3& pos, const dvec3& vel, const dvec3& gravity,
                      const dvec3& target_pos, const dvec3& target_vel, const assist_gains& g) {
    const dvec3 rel = target_pos - pos;
    const double dist = length(rel);
    const dvec3 v_rel = vel - target_vel;
    dvec3 v_cmd(0.0, 0.0, 0.0);  // at the waypoint: hold (zero closing speed)
    if (dist != 0.0) {
        // Stopping-distance cap: v^2 = 2*a_stop*dist keeps the required
        // decel inside ap_stop_m < 1 of the clamp for ANY approach speed.
        const double v_allow = std::sqrt(2.0 * g.ap_stop_m * g.max_accel * dist);
        v_cmd = rel * (v_allow / dist);
    }
    // -gravity: cancelling the well's pull inside the servo is what holds
    // the line (arcade: the assist flies the line, the pilot trusts it).
    return clamp_mag((v_cmd - v_rel) * g.ap_k_v - gravity, g.max_accel);
}

dvec3 autopilot_torque(const rigid_state& s, const dvec3& accel_cmd, const assist_gains& g) {
    const double am2 = length2(accel_cmd);
    dvec3 e_world(0, 0, 0);
    if (am2 != 0.0) {
        const dvec3 dir = accel_cmd * (1.0 / std::sqrt(am2));
        const dvec3 fwd = math::rotate(s.att, dvec3(0.0, 0.0, -1.0));  // #34 forward
        e_world = cross(fwd, dir);
        // Near-antiparallel stall: cross ~ 0 gives no reversal authority —
        // deterministic spin-up kick about body +Y (both body X/Y are
        // perpendicular to fwd; Y chosen, branch on state only).
        if (math::dot(fwd, dir) < 0.0 && length2(e_world) < 1e-12)
            e_world = math::rotate(s.att, dvec3(0.0, 1.0, 0.0)) * 3.14159265358979;
    }
    const dvec3 e_body = math::rotate(math::conjugate(s.att), e_world);
    // Full-omega damping: unlike auto-level, the autopilot owns yaw too.
    return e_body * g.ap_att_kp + s.omega_body * (-g.ap_att_kd);
}

} // namespace ve::core::sim
