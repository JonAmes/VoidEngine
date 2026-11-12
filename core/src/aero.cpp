#include "ve/core/sim/aero.h"

#include <cmath>

namespace ve::core::sim {

namespace {

bool all_finite(math::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

bool aero_params_ok(const aero_params& a) {
    return std::isfinite(a.ref_area_m2) && a.ref_area_m2 > 0.0 &&
           std::isfinite(a.drag_coeff) && a.drag_coeff >= 0.0 &&
           std::isfinite(a.lift_coeff) && a.lift_coeff >= 0.0 &&
           std::isfinite(a.max_aoa_rad) && a.max_aoa_rad > 0.0 &&
           a.max_aoa_rad < math::kPi_d / 2.0;  // forgiving shape needs a clear dead zone
}

double atmosphere_density(double rho0_kg_m3, double scale_height_m, double alt_m) {
    if (!(rho0_kg_m3 > 0.0) || !(scale_height_m > 0.0)) return 0.0;  // no-atmo / degenerate
    if (!std::isfinite(rho0_kg_m3) || !std::isfinite(scale_height_m) || !std::isfinite(alt_m))
        return 0.0;
    return rho0_kg_m3 * std::exp(-alt_m / scale_height_m);
}

math::dvec3 coair_velocity(double spin_rate_rad_s, math::dvec3 spin_axis_world,
                           math::dvec3 center, math::dvec3 pos) {
    if (spin_rate_rad_s == 0.0) return {};  // bit-exact still air
    if (!std::isfinite(spin_rate_rad_s) || !all_finite(spin_axis_world) || !all_finite(center) ||
        !all_finite(pos))
        return {};
    return math::cross(spin_axis_world * spin_rate_rad_s, pos - center);
}

aero_result aero_forces(const body_def& body, math::dvec3 center, math::dvec3 air_vel,
                        const rigid_state& s, const aero_params& a) {
    aero_result r;
    // #17 gates: atmosphere authored, well exists, body inside the well.
    if (body.atm_rho0_kg_m3 <= 0.0 || body.well_radius <= 0.0) return r;
    if (!state_ok(s) || !aero_params_ok(a) || !all_finite(center) || !all_finite(air_vel) ||
        !all_finite(body.spin_axis))
        return r;
    const math::dvec3 rel = s.pos - center;
    const double dist = math::length(rel);
    if (dist > body.well_radius) return r;  // aero only inside wells (#17)

    const double alt = dist - body.surface_radius;
    const double rho = atmosphere_density(body.atm_rho0_kg_m3, body.atm_scale_height_m, alt);
    if (rho <= 0.0) return r;

    const math::dvec3 vrel = s.vel - air_vel;
    r.speed_rel = math::length(vrel);
    if (r.speed_rel == 0.0) return r;  // no flow, no force
    r.dyn_q = 0.5 * rho * r.speed_rel * r.speed_rel;

    const math::dvec3 vhat = vrel * (1.0 / r.speed_rel);
    const math::dvec3 fwd = math::rotate(s.att, math::dvec3{0.0, 0.0, -1.0});  // #34 -Z fwd
    // Clamp before acos: fp dot of unit-ish vectors can pass +/-1 by an ulp.
    const double c = math::clamp(math::dot(fwd, vhat), -1.0, 1.0);
    r.aoa_rad = std::acos(c);

    // Forgiving stall (#47-era arcade rule): peak CL at authored a_max,
    // smooth sin rolloff to zero at 2*a_max, dead beyond (never negative).
    if (r.aoa_rad < 2.0 * a.max_aoa_rad)
        r.cl_eff = a.lift_coeff * std::sin(math::kPi_d * r.aoa_rad / (2.0 * a.max_aoa_rad));

    // Drag opposes relative flow.
    r.force = vhat * (-r.dyn_q * a.ref_area_m2 * a.drag_coeff);
    // Lift in the flow/nose plane, perpendicular to flow, on the nose side.
    // u degenerates exactly when fwd || vhat (aoa 0 or pi) — where CL is 0
    // for a_max < pi/2 — so skipping there is exact, not a loss.
    if (r.cl_eff != 0.0) {
        const math::dvec3 u = fwd - vhat * c;
        const double u2 = math::length2(u);
        if (u2 > 1e-30) r.force = r.force + u * (r.dyn_q * a.ref_area_m2 * r.cl_eff / std::sqrt(u2));
    }
    return r;
}

} // namespace ve::core::sim
