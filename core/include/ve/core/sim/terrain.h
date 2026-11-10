#pragma once
// Phase 7 (7.2, decision #29a/b): terrain is an ANALYTIC function, not data.
// One shared height+normal sampler backs landing contacts, on-foot EVA, shell
// impacts, and (Phase 8) streamed mesh gen — renderer and sim cannot disagree
// because they call the same pure fp64 function. Zero baked terrain meshes:
// the payload is these few generator params.

#include <cstdint>

#include "ve/core/math/vec.h"

namespace ve::core::sim {

// Per-body relief parameters (#29a): a terrain IS these numbers + a seed.
// Cooked payloads carry this struct in SI (#42); Phase 7 hand-authors it in
// tests (Tier 1, pure — no cook surface for world defs until Phase 8).
struct terrain_def {
    std::uint64_t seed = 0;
    double relief_m = 0.0;        // base-octave amplitude (m)
    double base_frequency = 1.0;  // lattice cells per unit cube-face coord
    int octaves = 1;              // clamped to [1, kMax_octaves] at eval
    double lacunarity = 2.0;      // frequency multiplier per octave
    double gain = 0.5;            // amplitude multiplier per octave
};

inline constexpr int kMax_octaves = 8;

// Cubic-face chart (#29a): the dominant axis of dir selects one of six cube
// faces; (u, v) is the gnomonic projection of dir onto that face (both in
// [-1, 1]). Continuous across face boundaries (the projected 3D point agrees
// from both sides) and pole-free — unlike lat/lon. face code: 2*axis + (dir
// negative), axis X=0 Y=1 Z=2; u/v are the raw ratios of the non-dominant
// coords over |dominant| (range [-1,1]; face sign + u + v reconstruct the
// projected point). Used for authored POI/stamp coords (7.3).
struct face_uv {
    int face = 0;
    double u = 0.0;
    double v = 0.0;
};
face_uv cube_face(math::dvec3 dir);

// Relief height in meters ABOVE the body's mean surface_radius, along a
// direction expressed in the body's LOCAL frame — #41: the body's orientation
// carries spin, so callers convert world->local once and the surface rides
// the spin for free. Multi-octave value noise sampled on the cube-face chart
// (an ambient 3D integer lattice evaluated at the projected direction —
// seam-free because the projection is continuous, see notes 7.2). Pure: the
// same (def, dir) bit-identically anywhere, any time (#40: platform libm is
// in scope for sampler evals). Authored stamps (7.3) modify this field
// before noise composes; not yet applied here.
double terrain_height(const terrain_def& def, math::dvec3 dir);

// Unit surface normal at dir, body-local frame. Analytic surface
// r(theta) = (R + h(theta)) * dir: the exact normal is
//   n = normalize((R + h) * dir - dh/de1 * e1 - dh/de2 * e2)
// (derivation in terrain.cpp); tangential derivatives come from central
// differences at a FIXED angular step, so the result is pure like height().
// mean_radius_m is the body's surface_radius (the sphere the relief rides).
// Degenerate dir (zero) behaves like +Y; mean_radius_m must be > 0.
math::dvec3 terrain_normal(const terrain_def& def, double mean_radius_m, math::dvec3 dir);

} // namespace ve::core::sim
