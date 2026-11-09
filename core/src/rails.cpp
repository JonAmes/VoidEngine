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

} // namespace ve::core::sim
