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

} // namespace ve::core::sim
