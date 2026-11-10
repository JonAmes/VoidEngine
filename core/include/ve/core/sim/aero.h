#pragma once
// core/sim arcade atmosphere (#17, #41, 7.6): aero forces exist ONLY inside
// a body's well and ONLY for bodies that authored an atmosphere (airless
// moons author none). Hand-authored SI structs this phase (#45d/#42b); the
// cooked world schema stays Phase 8.
//
// Model (arcade, hand-computable end to end):
//   rho(h) = rho0 * exp(-h/H)          h = radial altitude above surface_radius
//   q      = 1/2 * rho * |v_rel|^2     v_rel = vel - air_vel (spin co-rotation
//                                      rides air_vel; see coair_velocity)
//   CL(a)  = lift_coeff * sin(pi*a / (2*a_max)) for a < 2*a_max, else 0
//            a = angle between body forward (-Z, #34) and v_rel, in [0,pi].
//            CL peaks at exactly lift_coeff at the AUTHORED a_max (the
//            "forgiving stall": smooth rolloff, zero past 2*a_max, never a
//            cliff, never negative).
//   F      = q*A*( CL*u - CD*vhat )    u = normalize(fwd - (fwd.vhat)*vhat)
// Drag opposes the relative flow; lift lies in the flow/nose plane,
// perpendicular to the flow, on the nose side (flat-plate arcade).
//
// Deterministic: pure function of (state, params) at a tick; no wall clock,
// no stored state, no allocation. Degenerate input yields ZERO force —
// never a throw, never a NaN hit. Callers add result.force / mass into
// integrate_step's accel_world (the Phase-4 integrator owns the feel curve;
// aero just contributes force like thrust and wells do).

#include "ve/core/math/vec.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"

namespace ve::core::sim {

// Authored per hull (the drag.coeff / lift.coeff attr values from #47b,
// carried directly here in Tier-1 tests). a_max is the authored stall
// onset in rad; must be > 0 and < pi/2 for the forgiving shape to be
// meaningful (validated by aero_params_ok).
struct aero_params {
    double ref_area_m2 = 0.0;   // > 0
    double drag_coeff = 0.0;    // >= 0
    double lift_coeff = 0.0;    // >= 0, peak CL at a_max
    double max_aoa_rad = 0.0;   // > 0, < pi/2
};
bool aero_params_ok(const aero_params& a);

// rho0 = sea-level density (kg/m^3; 0 = no atmosphere), H = scale height.
// Pure exp curve — h may be negative (canyons/sub-surface arcade) and the
// density just grows along the same curve.
double atmosphere_density(double rho0_kg_m3, double scale_height_m, double alt_m);

// Air carried by body spin (#41): v_air = omega x r at the sample point.
// spin_axis_world is the body's spin axis expressed in WORLD (rotate the
// normalized def axis by the parent attitude); rate rad/s. Zero rate ->
// zero wind, bit-exact.
math::dvec3 coair_velocity(double spin_rate_rad_s, math::dvec3 spin_axis_world,
                           math::dvec3 center, math::dvec3 pos);

struct aero_result {
    math::dvec3 force{};   // N, world (ZERO outside wells / no atmosphere)
    double dyn_q = 0.0;    // 1/2 rho v^2 at the sample
    double aoa_rad = 0.0;  // forward-vs-flow angle, [0, pi]
    double speed_rel = 0.0;
    double cl_eff = 0.0;   // post-stall effective CL (for readouts/tests)
};

// Evaluate aero force for a rigid body in body's atmosphere. center = the
// body's center at this tick (world, from rail pose). #17 hard gate: no
// atmosphere authored, body has no well, or |pos-center| > well_radius ->
// zero force. air_vel: world air velocity at pos (coair_velocity, or zero
// for a still atmosphere).
aero_result aero_forces(const body_def& body, math::dvec3 center, math::dvec3 air_vel,
                        const rigid_state& s, const aero_params& a);

} // namespace ve::core::sim
