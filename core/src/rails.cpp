#include "ve/core/sim/rails.h"

#include <cmath>

#include "ve/core/math/base.h"

namespace ve::core::sim {

using ve::core::math::dvec3;

std::optional<std::int32_t> rail_world::add_body(body_def def) {
    const bool finite_all =
        std::isfinite(def.surface_radius) && std::isfinite(def.well_radius) &&
        std::isfinite(def.surface_g) && std::isfinite(def.well_taper_start) &&
        std::isfinite(def.orbit_period_s) && std::isfinite(def.orbit_radius_m) &&
        std::isfinite(def.orbit_phase_rad);
    if (!finite_all || def.surface_radius <= 0.0 || def.surface_g < 0.0)
        return std::nullopt;
    // Well shape sanity (field math lands 4.4; reject nonsense now, before
    // anything can reference this body by id).
    if (def.well_radius != 0.0 && def.well_radius <= def.surface_radius)
        return std::nullopt;
    if (def.well_radius != 0.0 &&
        (def.well_taper_start < def.surface_radius || def.well_taper_start >= def.well_radius))
        return std::nullopt;
    // Parent must be an EARLIER id (parent < size): cycles impossible,
    // chain order fixed by construction (#5 determinism discipline).
    if (def.parent != -1 && (def.parent < 0 || def.parent >= static_cast<std::int32_t>(m_bodies.size())))
        return std::nullopt;
    if (def.parent >= 0 && (def.orbit_period_s <= 0.0 || def.orbit_radius_m < 0.0))
        return std::nullopt;
    // Spin (#41): a nonzero rate needs a usable axis; normalize ONCE here
    // (never per eval) so orientation() is bit-stable across calls. A
    // zero-length axis with zero rate stays as-authored: inert either way.
    if (!std::isfinite(def.spin_rate_rad_s) || !std::isfinite(def.spin_axis.x) ||
        !std::isfinite(def.spin_axis.y) || !std::isfinite(def.spin_axis.z))
        return std::nullopt;
    const double axis_len2 = math::length2(def.spin_axis);
    if (def.spin_rate_rad_s != 0.0 && axis_len2 == 0.0)
        return std::nullopt;
    if (axis_len2 > 0.0) def.spin_axis = def.spin_axis * (1.0 / std::sqrt(axis_len2));
    m_bodies.push_back(std::move(def));
    return static_cast<std::int32_t>(m_bodies.size() - 1);
}

const body_def* rail_world::body(std::int32_t id) const {
    if (id < 0 || id >= static_cast<std::int32_t>(m_bodies.size())) return nullptr;
    return &m_bodies[static_cast<std::size_t>(id)];
}

dvec3 rail_world::local_rail_offset(const body_def& body, tick_t t) {
    if (body.parent == -1) return dvec3(0, 0, 0);  // static root anchor
    // Analytic angle: phase + tau * elapsed_periods. No pre-wrapping: C++
    // libm argument reduction is exact for finite doubles, and skipping the
    // wrap keeps the value bit-identical across calls (determinism #5).
    const double theta = body.orbit_phase_rad +
                         math::kTau_d * (tick_to_seconds(t) / body.orbit_period_s);
    const double c = std::cos(theta), s = std::sin(theta);
    // Mathematically positive spin about +Y (#34): +X turns toward -Z —
    // identical to rotate(quat_yaw(theta), (R,0,0)), pinned against the
    // rotation convention in test sim_rails.
    return dvec3(body.orbit_radius_m * c, 0.0, -body.orbit_radius_m * s);
}

std::optional<dvec3> rail_world::position(std::int32_t id, tick_t t) const {
    const body_def* b = body(id);
    if (!b) return std::nullopt;
    dvec3 p(0, 0, 0);
    std::int32_t cur = id;
    while (true) {
        const body_def* cb = body(cur);  // parent < id invariant: always valid
        p = p + local_rail_offset(*cb, t);
        if (cb->parent == -1) break;
        cur = cb->parent;
    }
    return p;
}

std::optional<math::dvec3> rail_world::velocity(std::int32_t id, tick_t t) const {
    if (!body(id)) return std::nullopt;
    // Central difference of the analytic position: pure function of the
    // integer tick, no stored state, bit-deterministic. Exact for static
    // roots; chord-vs-tangent relative error ~ (2*pi*dt/P)^2/24 (<= ~1e-9
    // rel for authored periods >= 60 s).
    const auto p1 = position(id, t + 1);
    const auto p0 = position(id, t - 1);
    return (*p1 - *p0) * (0.5 * kSim_tick_hz);
}

math::dquat rail_world::local_spin_orient(const body_def& body, tick_t t) {
    if (body.spin_rate_rad_s == 0.0) return math::dquat{};
    // No pre-wrapping of the angle: libm argument reduction is exact for
    // finite doubles and the unwrapped value stays bit-identical across
    // calls (determinism #5, same discipline as local_rail_offset). Axis
    // is already unit-normalized by add_body.
    const double angle = body.spin_rate_rad_s * tick_to_seconds(t);
    return math::quat_from_axis_angle(body.spin_axis, angle);
}

std::optional<math::dquat> rail_world::orientation(std::int32_t id, tick_t t) const {
    const body_def* b = body(id);
    if (!b) return std::nullopt;
    // Chain is short (parent < id invariant); compose root-down. The axis
    // is authored in the parent rail frame, so the local spin RIGHT-
    // multiplies the parent's world attitude: world = parent_world * local.
    std::vector<const body_def*> chain;
    std::int32_t cur = id;
    while (true) {
        const body_def* cb = body(cur);  // parent < id invariant: always valid
        chain.push_back(cb);
        if (cb->parent == -1) break;
        cur = cb->parent;
    }
    math::dquat q{};  // static root frame is identity (#45c)
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        q = q * local_spin_orient(**it, t);
    return q;
}

} // namespace ve::core::sim
