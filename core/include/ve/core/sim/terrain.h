#pragma once
// Phase 7 (7.2, decision #29a/b): terrain is an ANALYTIC function, not data.
// One shared height+normal sampler backs landing contacts, on-foot EVA, shell
// impacts, and (Phase 8) streamed mesh gen — renderer and sim cannot disagree
// because they call the same pure fp64 function. Zero baked terrain meshes:
// the payload is these few generator params.

#include <cstdint>
#include <span>

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
// in scope for sampler evals. See the stamped overload for authored POIs
// (#29c).
double terrain_height(const terrain_def& def, math::dvec3 dir);

// Authored stamps (#29c, runtime side): POIs, craters, landing pads. A stamp
// is (type, body-local center dir, ANGULAR radius rad, level m above mean
// surface, blend fraction of radius). The stack is applied BEFORE noise:
// stamps blend a base profile over the flat baseline and suppress noise
// amplitude by their weight, then noise composes on top:
//   h(dir) = h_base(dir) + (1 - W(dir)) * noise(dir),  W = max stamp weight
// so a full-weight interior is EXACTLY the level (zero noise influence) —
// that is what makes the pad guarantee below a proof, not a tolerance wish.
// Stack order matters (later stamps blend over earlier results).
// Types:
//   flatten: plateau profile (full weight inside (1-blend)*radius, quintic
//            blend ring out to radius), target = level_m.
//   mesa:    same profile, target = level_m (authored as a raised plateau;
//            semantics identical to flatten — the authored level's sign and
//            magnitude make the landform).
//   crater:  point profile: full weight at the center, quintic taper to zero
//            at radius (bowl floor at level_m, walls form from the blend).
// Degenerate stamps (zero/non-finite center, non-positive/non-finite radius)
// are skipped (zero weight); blend clamped to [0, 1].
enum class stamp_type { flatten, crater, mesa };
struct terrain_stamp {
    stamp_type type = stamp_type::flatten;
    math::dvec3 center_dir{0.0, 1.0, 0.0};  // body-local; normalized at eval
    double radius_rad = 0.0;                // angular radius (radians)
    double level_m = 0.0;                   // target height above mean radius
    double blend = 0.25;                    // blend ring as fraction of radius
};

// Stamped variant: stamps ride the SAME local-frame contract as everything
// here — world-space callers rotate dir by the body's 7.1 orientation(), so
// stamps spin with the body for free (#41).
double terrain_height(const terrain_def& def, std::span<const terrain_stamp> stamps, math::dvec3 dir);

// Unit surface normal at dir, body-local frame. Analytic surface
// r(theta) = (R + h(theta)) * dir: the exact normal is
//   n = normalize((R + h) * dir - dh/de1 * e1 - dh/de2 * e2)
// (derivation in terrain.cpp); tangential derivatives come from central
// differences at a FIXED angular step, so the result is pure like height().
// mean_radius_m is the body's surface_radius (the sphere the relief rides).
// Degenerate dir (zero) behaves like +Y; mean_radius_m must be > 0.
math::dvec3 terrain_normal(const terrain_def& def, double mean_radius_m, math::dvec3 dir);

// Stamped normal variant: same formula, height() replaced by the stamped
// height — blend rings therefore tilt the normal exactly as authored, and a
// flatten stamp's full-weight interior has normal EXACTLY radial (h is
// bit-constant over the inner disk, so the tangential differences vanish):
// pad plane ∥ local well normal with no tolerance needed (#29c).
math::dvec3 terrain_normal(const terrain_def& def, double mean_radius_m,
                           std::span<const terrain_stamp> stamps, math::dvec3 dir);

} // namespace ve::core::sim
