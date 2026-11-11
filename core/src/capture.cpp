#include "ve/core/sim/capture.h"
#include "ve/core/sim/dock.h"

#include <cmath>

namespace ve::core::sim {
namespace {

bool finite(math::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Minimal rotation taking unit `from` to unit `to`. The 180-degenerate
// axis choice is deterministic (first stable perpendicular axis) — pinned
// by test, not hidden.
math::dquat minimal_rotation(math::dvec3 from, math::dvec3 to) {
    const double d = math::dot(from, to);
    if (d > 1.0 - 1e-12) return {};  // identity
    if (d < -1.0 + 1e-12) {
        math::dvec3 axis = math::cross({1.0, 0.0, 0.0}, from);
        if (math::length2(axis) < 1e-12) axis = math::cross({0.0, 1.0, 0.0}, from);
        axis = math::normalize(axis);
        return math::dquat{axis.x, axis.y, axis.z, 0.0};  // 180 deg
    }
    const math::dvec3 axis = math::cross(from, to);
    const double s = std::sqrt((1.0 + d) * 2.0);
    const double inv = 1.0 / s;
    return math::dquat{axis.x * inv, axis.y * inv, axis.z * inv, s * 0.5};
}

} // namespace

std::optional<pad_world> pad_at(const rail_world& world, const pad_def& pad,
                                tick_t t) {
    if (pad.body < 0 || !finite(pad.local_dir) || !(pad.capture_radius_m >= 0.0) ||
        !(pad.safe_speed_m_s >= 0.0) || !(pad.rest_depth_m >= 0.0))
        return std::nullopt;
    if (std::fabs(math::length(pad.local_dir) - 1.0) > 1e-9) return std::nullopt;
    const auto center = world.position(pad.body, t);
    const auto orient = world.orientation(pad.body, t);
    const auto body_vel = world.velocity(pad.body, t);
    if (!center || !orient || !body_vel) return std::nullopt;
    const auto* def = world.body(pad.body);
    if (def == nullptr) return std::nullopt;

    // Spin angular velocity in world. The axis is authored in the PARENT
    // rail frame (rails.h #41); orientation() = parent_att ⊗ local_spin,
    // so the plain spin axis carries parent_att = orient ⊗ conjugate(spin).
    // With one body in the chain, parent attitude is identity anyway —
    // nested rails (Phase 8+) revisit this.
    math::dvec3 omega_world{0.0, 0.0, 0.0};
    if (def->spin_rate_rad_s != 0.0 && finite(def->spin_axis)) {
        const math::dvec3 axis = math::normalize(def->spin_axis);
        omega_world = axis * def->spin_rate_rad_s;
    }

    pad_world out;
    out.normal = math::rotate(*orient, pad.local_dir);
    const math::dvec3 r_local = pad.local_dir * def->surface_radius;
    out.pos = *center + math::rotate(*orient, r_local);
    const math::dvec3 r_world = out.pos - *center;
    out.vel = *body_vel + math::cross(omega_world, r_world);
    return out;
}

template <typename Zone, typename World>
std::optional<swept_hit> capture_engage(const Zone& zone,
                                        const World& pad,
                                        math::dvec3 ship_pos,
                                        math::dvec3 ship_vel, tick_t t) {
    if (!finite(pad.pos) || !finite(pad.vel) || !finite(pad.normal) ||
        !finite(ship_pos) || !finite(ship_vel))
        return std::nullopt;
    if (!(zone.capture_radius_m > 0.0) || !(zone.safe_speed_m_s >= 0.0))
        return std::nullopt;


    const math::dvec3 rel_p = ship_pos - pad.pos;
    const math::dvec3 rel_v = ship_vel - pad.vel;
    // #29d gate: relative speed, constant over the tick. A hull screaming
    // past above safe_speed never engages however close it passes (#29e:
    // it simply doesn't — nothing "fails").
    if (!(math::length(rel_v) <= zone.safe_speed_m_s)) return std::nullopt;

    // Closest approach of the two linear paths over tau in [0, dt]
    // (the #45e swept-at-a-fixed-tick shape; parallel motion uses the
    // constant start distance).
    const double dt = kSim_tick_dt;
    double tau = 0.0;
    const double vv = math::dot(rel_v, rel_v);
    if (vv > 0.0) {
        tau = -math::dot(rel_p, rel_v) / vv;
        tau = tau < 0.0 ? 0.0 : (tau > dt ? dt : tau);
    }
    const math::dvec3 at = rel_p + rel_v * tau;
    const double dist = math::length(at);
    if (!(dist <= zone.capture_radius_m)) return std::nullopt;

    swept_hit hit;
    hit.t = tau;
    hit.pos = ship_pos + ship_vel * tau;
    hit.normal = dist > 0.0 ? at / dist : pad.normal;  // zone surface, pad->hull
    (void)t;  // tick kept for the API's tick-pinned call shape (#45e)
    return hit;
}

rigid_state snap_to_rest(const rigid_state& ship, const pad_world& pad,
                         const pad_def& def) {
    rigid_state out;
    // Gear contact point (hull local -Y in the rest pose) pinned
    // rest_depth under the pad surface, on the pad normal.
    out.pos = pad.pos - pad.normal * def.rest_depth_m;
    out.vel = pad.vel;  // ride the moving pad (#41)
    // Arcade alignment assist: one algebraic re-aim — hull local -Y onto
    // the pad normal by the minimal rotation from the current attitude.
    const math::dvec3 down_world =
        math::rotate(ship.att, {0.0, -1.0, 0.0});
    out.att = minimal_rotation(down_world, pad.normal) * ship.att;
    out.omega_body = {};  // at rest relative to the PAD; the hull does not
                          // inherit body spin (#41) — spin rides pad.vel
    return out;
}

// Template definition lives in this TU; explicit instantiations for every
// authored zone shape in the engine (landing pad #29g, dock port #23a).
template std::optional<swept_hit> capture_engage<pad_def, pad_world>(
    const pad_def&, const pad_world&, math::dvec3, math::dvec3, tick_t);
template std::optional<swept_hit> capture_engage<dock_port_def, port_world>(
    const dock_port_def&, const port_world&, math::dvec3, math::dvec3, tick_t);

} // namespace ve::core::sim
