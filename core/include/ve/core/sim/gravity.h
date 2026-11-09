#pragma once
// core/sim gravity wells (decision #22, Q4): arcade field per body —
// constant surface_g near the surface, C1 ramp to exactly 0 at the well
// edge, exactly 0 outside (no 1/r^2: the slice wants predictable arcs,
// not Kepler orbits). The well follows its body, sampled from the rail
// at the queried tick (#17a) — same analytic purity as the rails.
//
// Profile derivation (C1 everywhere): for r in [taper, well], with
// u = (r - taper)/(well - taper) and smoothstep s(u) = 3u^2 - 2u^3
// (s'(0) = s'(1) = 0), g(r) = surface_g * (1 - s(u)). Then
// g(taper) = surface_g with slope 0 matching the constant core, and
// g(well) = 0 with slope 0 matching the void — both joins are slope-
// continuous, so a ship crossing the well edge feels no acceleration
// step. Also 1 - s(u) = (1-u)^2 (1+2u): the edge falloff is quadratic.

#include "ve/core/math/vec.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// Signed magnitude (>= 0) of the well's pull at radial distance r from the
// body center. body.well_radius == 0 means "no well" -> 0 everywhere.
double well_g_magnitude(const body_def& body, double r);

// Accel at world pos toward a body KNOWN to sit at body_center (pure math,
// no rail lookup). Direction = (center - pos)/r; zero at r == 0 (no
// direction to pull) and outside the well (exactly zero dvec3).
math::dvec3 well_accel_at(const body_def& body, const math::dvec3& body_center,
                          const math::dvec3& pos);

// Convenience: center sampled from the rail at the SAME tick the caller's
// physics step uses — bit-identical to well_accel_at(def, position(id, t), pos).
math::dvec3 well_accel(const rail_world& world, std::int32_t body_id,
                       const math::dvec3& pos, tick_t t);

} // namespace ve::core::sim
