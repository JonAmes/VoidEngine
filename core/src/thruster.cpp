// 5.5 implementation. Everything in BODY axes until the last rotate;
// fixed gimbal order (pitch about mount +X, then yaw about mount +Y) and
// input-order iteration keep this deterministic (#5).
#include "ve/core/sim/thruster.h"

#include <algorithm>
#include <cmath>

#include "ve/core/math/base.h"

namespace ve::core::sim {

namespace {
using math::dquat;
using math::dvec2;
using math::dvec3;
using math::quat_pitch;
using math::quat_yaw;
using math::rotate;
}  // namespace

math::dvec3 gimbal_deflected_axis(const thruster_static& t, dvec2 gimbal) {
    // Defensive normalize: authors must pass a unit axis, but a denormal
    // axis must not silently scale force.
    const double alen = length(t.axis_local);
    if (alen == 0.0) return dvec3(0.0, 0.0, 0.0);
    const dvec3 axis = t.axis_local / alen;

    // Fixed mount (limit 0): the gimbal input is IGNORED entirely.
    if (t.gimbal_limit_rad <= 0.0) return rotate(t.orientation_local, axis);
    // Cone clamp: scale the (pitch,yaw) angle pair into the cone first —
    // direction of deflection preserved, magnitude capped (composed angle
    // matches the cone half-angle to O(theta^3) — small-angle composition).
    if (t.gimbal_limit_rad > 0.0) {
        const double mag = std::sqrt(gimbal.x * gimbal.x + gimbal.y * gimbal.y);
        if (mag > t.gimbal_limit_rad) {
            const double s = t.gimbal_limit_rad / mag;
            gimbal = dvec2(gimbal.x * s, gimbal.y * s);
        }
    }
    // Fixed order: pitch (mount +X) then yaw (mount +Y). Composition in
    // mount frame: axis_deflected = orient * pitch * yaw applied to axis.
    const dquat deflect = quat_pitch(gimbal.x) * quat_yaw(gimbal.y);
    return rotate(t.orientation_local * deflect, axis);
}

thruster_forces apply_thrusters(const dquat& att, dvec3 com_local,
                                const std::vector<thruster_static>& thrusters,
                                const std::vector<thruster_input>& inputs) {
    thruster_forces out;
    const std::size_t n = std::min(thrusters.size(), inputs.size());
    dvec3 force_local(0.0, 0.0, 0.0);
    dvec3 torque(0.0, 0.0, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const double throttle = std::clamp(inputs[i].throttle, 0.0, 1.0);
        if (throttle == 0.0 || thrusters[i].max_force == 0.0) continue;
        const dvec3 axis = gimbal_deflected_axis(thrusters[i], inputs[i].gimbal);
        if (length2(axis) == 0.0) continue;  // degenerate axis: no force, no record
        const dvec3 f = axis * (throttle * thrusters[i].max_force);
        force_local = force_local + f;
        // r x F about the CoM, both in body axes — off-CoM engines produce
        // real torque with NO special-casing (the 5.13 misaligned-engine pin).
        torque = torque + cross(thrusters[i].position_local - com_local, f);
        out.applied.push_back(applied_force{i, f});
    }
    out.force_world = rotate(att, force_local);
    out.torque_body = torque;
    return out;
}

}  // namespace ve::core::sim
