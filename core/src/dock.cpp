#include "ve/core/sim/dock.h"

#include <cmath>
#include <limits>

#include "ve/core/math/quat.h"

namespace ve::core::sim {
namespace {

// Same guards as capture.cpp: garbage in, nullopt out (core is exception-free).
bool finite3(const math::dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Minimal rotation taking unit `from` onto unit `to` (both verified unit).
// Degenerate anti-parallel case: rotate 180 deg about a pinned axis
// (cross with xhat unless xhat is parallel, then yhat) — deterministic.
math::dquat minimal_rotation(math::dvec3 from, math::dvec3 to) {
    const double d = math::dot(from, to);
    if (d > 1.0 - 1e-12)
        return math::dquat{};  // identity
    if (d < -1.0 + 1e-12) {
        math::dvec3 axis = math::cross({1.0, 0.0, 0.0}, from);
        if (math::length2(axis) < 1e-12)
            axis = math::cross({0.0, 1.0, 0.0}, from);
        axis = math::normalize(axis);
        return math::quat_from_axis_angle(axis, math::kPi_d);
    }
    // axis = from x to, angle = acos(d); quat (1+d, axis) is the same
    // rotation up to scale — normalize once, bit-stable.
    math::dvec3 axis = math::cross(from, to);
    const double w = 1.0 + d;
    const double len = std::sqrt(w * w + math::length2(axis));
    return math::dquat{w / len, axis.x / len, axis.y / len, axis.z / len};
}

} // namespace

std::optional<port_world> port_at(const dock_port_def& port,
                                  const rigid_state& host) {
    if (port.body < 0 || !finite3(port.local_pos) || !finite3(port.local_normal))
        return std::nullopt;
    if (port.capture_radius_m < 0.0 || port.safe_speed_m_s < 0.0 ||
        port.rest_depth_m < 0.0)
        return std::nullopt;
    const double nl2 = math::length2(port.local_normal);
    if (!(nl2 > 0.0))  // zero OR NaN normal (NaN fails every comparison)
        return std::nullopt;
    const math::dvec3 n_local = port.local_normal / std::sqrt(nl2);
    const math::dquat q = host.att;
    const math::dvec3 r = math::rotate(q, port.local_pos);
    port_world out;
    out.pos = host.pos + r;
    // Point velocity of a spinning host: v + omega x r (omega in WORLD).
    const math::dvec3 omega_world = math::rotate(q, host.omega_body);
    out.vel = host.vel + math::cross(omega_world, r);
    out.normal = math::rotate(q, n_local);
    if (!finite3(out.pos) || !finite3(out.vel) || !finite3(out.normal))
        return std::nullopt;
    return out;
}

std::optional<port_world> port_on_rail(const rail_world& world,
                                       const dock_port_def& port, tick_t t) {
    if (port.body < 0 || !finite3(port.local_pos) || !finite3(port.local_normal))
        return std::nullopt;
    if (port.capture_radius_m < 0.0 || port.safe_speed_m_s < 0.0 ||
        port.rest_depth_m < 0.0)
        return std::nullopt;
    const double nl2 = math::length2(port.local_normal);
    if (!(nl2 > 0.0))
        return std::nullopt;
    const math::dvec3 n_local = port.local_normal / std::sqrt(nl2);
    const auto center = world.position(port.body, t);
    const auto vel = world.velocity(port.body, t);
    const auto orient = world.orientation(port.body, t);
    if (!center || !vel || !orient)
        return std::nullopt;
    const math::dvec3 r_local = port.local_pos;
    port_world out;
    out.normal = math::rotate(*orient, n_local);
    out.pos = *center + math::rotate(*orient, r_local);
    // Spin-riding point velocity, exactly the pad_at (#41) recipe: the
    // rail velocity plus omega x r about the body center.
    const auto* def = world.body(port.body);
    if (!def)
        return std::nullopt;
    const math::dvec3 omega = def->spin_axis * def->spin_rate_rad_s;
    out.vel = *vel + math::cross(omega, out.pos - *center);
    if (!finite3(out.pos) || !finite3(out.vel) || !finite3(out.normal))
        return std::nullopt;
    return out;
}

rigid_state snap_to_dock(const rigid_state& ship, const port_world& port,
                         const dock_port_def& port_def,
                         const dock_gear_def& gear) {
    // Gear point currently at ship.pos + R(ship.att)·gear.local_pos. The
    // clamp pins the gear ON the port axis at rest depth inside the plane:
    const math::dvec3 target =
        port.pos - port.normal * port_def.rest_depth_m;

    // Attitude: hull local +Z (the dock gear axis — the keel/airlock
    // faces the port) onto the port normal, via the same minimal-rotation
    // alignment assist as snap_to_rest (7.9 pins -Y because a LANDER
    // stands on its belly; a DOCKED ship seats its gear: the hull's +Z
    // must mate the normal or the gear can never sit on the port axis).
    rigid_state out = ship;
    out.att = math::normalize(minimal_rotation(math::rotate(ship.att,
                                              {0.0, 0.0, 1.0}),
                                              port.normal) *
                              ship.att);
    out.pos = target - math::rotate(out.att, gear.local_pos);
    out.vel = port.vel;  // port-relative rest (#41)
    out.omega_body = {0.0, 0.0, 0.0};
    return out;
}

rigid_state docked_pose(const rigid_state& ship_hull_hint,
                        const port_world& port, const dock_port_def& port_def,
                        const dock_gear_def& gear, math::dquat clamp_att) {
    // The rigid bind as an EXACT pose projection, no solver.
    //
    // clamp_att is captured ONCE at engage (the ship attitude relative to
    // the host hull: ship = host * clamp_att — snap_to_dock's result
    // satisfies exactly this with the host's attitude at engage). While
    // clamped, the SHIP keeps the same world attitude: a docked pair is one
    // rigid body — a re-spin of the host alone would break that identity,
    // so att is taken from the hint (the ship's own pose), NOT rebuilt.
    // Position is then re-projected so the gear point is pinned at rest
    // depth on the port axis: any host drift/orbit/spin since the last
    // tick shows up as a pure translation of the ship — the pair rides the
    // port. For a host that moves as a rigid body (the physics case: the
    // host is the ship's own frame partner), this projection is exact, not
    // an approximation.
    const math::dvec3 target =
        port.pos - port.normal * port_def.rest_depth_m;
    rigid_state out = ship_hull_hint;
    out.pos = target - math::rotate(out.att, gear.local_pos);
    out.vel = port.vel;
    (void)clamp_att;  // consumed at engage (its effect rides att/pos);
                      // kept in the signature so tools can rebuild the
                      // bind from authored data without stored state
    return out;
}

rigid_state release_impulse(const rigid_state& docked,
                            const port_world& port,
                            const dock_gear_def& gear,
                            const rigid_params& params,
                            double normal_impulse_N_s,
                            math::dvec3 tangent_impulse_N_s) {
    // J at the gear point: linear + angular, exactly the thrust-impulse
    // arithmetic of rigid integration (dv = J/m; dw = I^-1 (r x J)).
    const math::dvec3 n = port.normal;
    // Tangent component: strip any normal part from the authored nudge so
    // it can never pull the ship back through the clamp plane.
    const math::dvec3 t =
        tangent_impulse_N_s - n * math::dot(tangent_impulse_N_s, n);
    const math::dvec3 j = n * normal_impulse_N_s + t;

    const double mass = params.mass > 0.0 ? params.mass
                                          : std::numeric_limits<double>::max();
    rigid_state out = docked;
    out.vel = out.vel + j / mass;
    const math::dvec3 r = math::rotate(docked.att, gear.local_pos);
    const math::dvec3 torque_impulse = math::cross(r, j);
    // Body-frame inertia (same shape as integrate_step's angle path):
    // I^-1 is diag(1/I_ii); torque impulse expressed in body axes.
    const math::dvec3 torque_body =
        math::rotate(math::conjugate(docked.att), torque_impulse);
    const auto inv = [](double i) {
        return i > 0.0 ? 1.0 / i : 0.0;
    };
    const math::dvec3 dw_body = {torque_body.x * inv(params.inertia_diag.x),
                                 torque_body.y * inv(params.inertia_diag.y),
                                 torque_body.z * inv(params.inertia_diag.z)};
    out.omega_body = out.omega_body + dw_body;
    return out;
}

} // namespace ve::core::sim
