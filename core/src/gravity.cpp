#include "ve/core/sim/gravity.h"

#include <cmath>

namespace ve::core::sim {

using ve::core::math::dvec3;

double well_g_magnitude(const body_def& body, double r) {
    if (body.well_radius <= 0.0 || r >= body.well_radius) return 0.0;
    if (r <= body.well_taper_start) return body.surface_g;  // constant core
    // C1 ramp (derivation in gravity.h): g = surface_g * (1 - s(u)).
    const double u = (r - body.well_taper_start) / (body.well_radius - body.well_taper_start);
    const double one_minus_u = 1.0 - u;
    // (1-u)^2 (1+2u) == 1 - (3u^2 - 2u^3) exactly in exact math; the factored
    // form is also better conditioned near the edge (no cancellation).
    return body.surface_g * one_minus_u * one_minus_u * (1.0 + 2.0 * u);
}

dvec3 well_accel_at(const body_def& body, const dvec3& center, const dvec3& pos) {
    const dvec3 to_center = center - pos;
    const double r = length(to_center);
    if (r == 0.0) return dvec3(0, 0, 0);  // at the center: no direction
    const double g = well_g_magnitude(body, r);
    if (g == 0.0) return dvec3(0, 0, 0);  // outside: EXACTLY zero, no -0 noise
    return to_center * (g / r);
}

dvec3 well_accel(const rail_world& world, std::int32_t body_id, const dvec3& pos, tick_t t) {
    const body_def* b = world.body(body_id);
    const auto center = world.position(body_id, t);
    if (!b || !center) return dvec3(0, 0, 0);
    return well_accel_at(*b, *center, pos);
}

} // namespace ve::core::sim
