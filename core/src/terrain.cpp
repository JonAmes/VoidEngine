#include "ve/core/sim/terrain.h"

#include <cmath>
#include <cstdint>

namespace ve::core::sim {
namespace {

// splitmix64 (Steele et al.) — avalanche mix for the lattice hash; chosen
// because it is a handful of integer ops (bit-deterministic everywhere) and
// decorrelates the linear coordinate combiner below.
constexpr std::uint64_t splitmix64(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

constexpr double kInv_2p53 = 1.0 / 9007199254740992.0; // 2^-53; h>>11 < 2^53 so the int->double conversion is exact

// Value at one lattice corner: hash(seed, x, y, z) -> [-1, 1). Coordinates
// combine linearly mod 2^64 (two's complement cast = defined wrap) before
// the avalanche mix; distinct odd 64-bit multipliers keep the combiner
// injective-ish for small offsets.
constexpr double lattice_value(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t z) {
    const std::uint64_t h = splitmix64(
        seed + std::uint64_t(x) * 0x9E3779B97F4A7C15ULL +
               std::uint64_t(y) * 0xC2B2AE3D27D4EB4FULL +
               std::uint64_t(z) * 0x165667B19E3779F9ULL);
    return 2.0 * (double(h >> 11) * kInv_2p53) - 1.0;
}

// Quintic smootherstep: C2 at cell borders, so height AND its first
// derivative are continuous across lattice cells (crucial for normals).
constexpr double quintic(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

constexpr double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Ambient 3D value noise on an integer lattice. Evaluated at the CUBE-FACE
// PROJECTED direction (#29a "cubic-face coords"): the projection is
// continuous across dominant-face switches, so a single shared lattice is
// seamless — per-face 2D lattices would seam at the face borders. Lattice
// coords stay <= ~base_freq * sqrt(3) * lac^octaves (<< 2^63) by the
// octaves/frequency caps, so the int64 conversion below never overflows.
double value_noise(std::uint64_t seed, double x, double y, double z) {
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const std::int64_t ix = std::int64_t(fx), iy = std::int64_t(fy), iz = std::int64_t(fz);
    const double tx = quintic(x - fx), ty = quintic(y - fy), tz = quintic(z - fz);
    const double c000 = lattice_value(seed, ix, iy, iz);
    const double c100 = lattice_value(seed, ix + 1, iy, iz);
    const double c010 = lattice_value(seed, ix, iy + 1, iz);
    const double c110 = lattice_value(seed, ix + 1, iy + 1, iz);
    const double c001 = lattice_value(seed, ix, iy, iz + 1);
    const double c101 = lattice_value(seed, ix + 1, iy, iz + 1);
    const double c011 = lattice_value(seed, ix, iy + 1, iz + 1);
    const double c111 = lattice_value(seed, ix + 1, iy + 1, iz + 1);
    return lerp(lerp(lerp(c000, c100, tx), lerp(c010, c110, tx), ty),
                lerp(lerp(c001, c101, tx), lerp(c011, c111, tx), ty), tz);
}

// Gnomonic projection onto the unit cube (dominant coord -> +-1). Tie order
// X,Y,Z fixed; matches cube_face() below (one projection, one truth).
math::dvec3 cube_project(math::dvec3 d) {
    const double ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    if (ax >= ay && ax >= az) return d * (1.0 / ax);
    if (ay >= az) return d * (1.0 / ay);
    return d * (1.0 / az);
}

// The raw multi-octave noise field at an ALREADY-NORMALIZED direction.
// Operation order here is the 7.2 pinned bit-contract (#40) — do not
// refactor the accumulation without re-deriving the hand pins.
double noise_field(const terrain_def& def, math::dvec3 d) {
    const math::dvec3 p = cube_project(d);
    const int oct = def.octaves < 1 ? 1 : (def.octaves > kMax_octaves ? kMax_octaves : def.octaves);
    double sum = 0.0, amp = 1.0, freq = def.base_frequency;
    for (int o = 0; o < oct; ++o) {
        sum += amp * value_noise(def.seed, p.x * freq, p.y * freq, p.z * freq);
        amp *= def.gain;
        freq *= def.lacunarity;
    }
    return def.relief_m * sum;
}

math::dvec3 normalize_dir(math::dvec3 dir) {  // shared degenerate pin (never throws)
    double len2 = math::length2(dir);
    if (len2 == 0.0) return math::dvec3(0.0, 1.0, 0.0);
    return dir * (1.0 / std::sqrt(len2));
}

// Stamp weight in [0,1] at unit direction d (angular distance via acos —
// libm is in the #40 platform contract for sampler evals). Degenerate
// stamps (non-finite fields, zero center, non-positive radius) are skipped.
double stamp_weight(const terrain_stamp& s, math::dvec3 d) {
    if (!std::isfinite(s.radius_rad) || s.radius_rad <= 0.0) return 0.0;
    if (!std::isfinite(s.level_m) || !std::isfinite(s.blend)) return 0.0;
    const double c2 = math::length2(s.center_dir);
    if (!(c2 > 0.0)) return 0.0;  // NaN center falls through here too
    const math::dvec3 c = s.center_dir * (1.0 / std::sqrt(c2));
    double cosang = math::dot(d, c);
    if (cosang > 1.0) cosang = 1.0; else if (cosang < -1.0) cosang = -1.0;
    const double dd = std::acos(cosang);
    if (dd >= s.radius_rad) return 0.0;
    if (s.type == stamp_type::crater) {
        // point profile: floor at the center, quintic bowl wall to the rim
        return quintic(1.0 - dd / s.radius_rad);
    }
    // plateau profile (flatten/mesa): full weight inside, quintic blend ring
    if (s.blend <= 0.0) return 1.0;  // hard disc (authored misuse; deterministic)
    const double inner = (1.0 - (s.blend > 1.0 ? 1.0 : s.blend)) * s.radius_rad;
    if (dd <= inner) return 1.0;
    return 1.0 - quintic((dd - inner) / (s.radius_rad - inner));
}

} // namespace

face_uv cube_face(math::dvec3 dir) {
    const double ax = std::fabs(dir.x), ay = std::fabs(dir.y), az = std::fabs(dir.z);
    int axis = 2;
    double denom = az;
    if (ax >= ay && ax >= az) { axis = 0; denom = ax; }
    else if (ay >= az) { axis = 1; denom = ay; }
    const double comps[3] = {dir.x, dir.y, dir.z};
    const double s = 1.0 / denom;
    return face_uv{2 * axis + (comps[axis] < 0.0 ? 1 : 0),
                   comps[(axis + 1) % 3] * s, comps[(axis + 2) % 3] * s};
}

double terrain_height(const terrain_def& def, math::dvec3 dir) {
    return noise_field(def, normalize_dir(dir));
}

double terrain_height(const terrain_def& def, std::span<const terrain_stamp> stamps, math::dvec3 dir) {
    if (stamps.empty()) return terrain_height(def, dir);  // bit-identical 7.2 path
    const math::dvec3 d = normalize_dir(dir);
    const double hn = noise_field(def, d);
    // #29c order: stamps compose over the FLAT baseline first, then noise
    // composes on top scaled by the worst suppression — a full-weight stamp
    // pins h to its level with zero noise influence (exact bits).
    double hb = 0.0, W = 0.0;
    for (const auto& s : stamps) {
        const double w = stamp_weight(s, d);
        if (w == 0.0) continue;
        // w==1 shortcut: lerp would round (level - hb) + hb; the pin must be
        // the authored level's exact bits.
        hb = (w == 1.0) ? s.level_m : hb + w * (s.level_m - hb);
        if (w > W) W = w;
    }
    return hb + (1.0 - W) * hn;
}

math::dvec3 terrain_normal(const terrain_def& def, double mean_radius_m, math::dvec3 dir) {
    return terrain_normal(def, mean_radius_m, std::span<const terrain_stamp>{}, dir);
}

math::dvec3 terrain_normal(const terrain_def& def, double mean_radius_m,
                           std::span<const terrain_stamp> stamps, math::dvec3 dir) {
    const math::dvec3 d = normalize_dir(dir);

    // Deterministic tangent basis: ref = axis of SMALLEST |component| (ties
    // pick the lower axis index) keeps |d x ref| >= sqrt(2/3) for any d.
    int ref_axis = 0;
    double amin = std::fabs(d.x);
    if (std::fabs(d.y) < amin) { ref_axis = 1; amin = std::fabs(d.y); }
    if (std::fabs(d.z) < amin) { ref_axis = 2; }
    math::dvec3 ref(0.0, 0.0, 0.0);
    if (ref_axis == 0) ref.x = 1.0;
    else if (ref_axis == 1) ref.y = 1.0;
    else ref.z = 1.0;
    const math::dvec3 e1 = math::normalize(math::cross(d, ref));
    const math::dvec3 e2 = math::cross(d, e1);

    // Surface r(theta) = (R + h(theta)) * dir(theta); differentiating along
    // the two tangent directions and crossing (the full derivation, checked
    // by the flat-relief pin) gives the unnormalized normal
    //   (R + h) * d - (dh/dtheta_1) * e1 - (dh/dtheta_2) * e2.
    // Fixed angular step: central difference of pure functions stays pure.
    constexpr double kEps = 1e-5;  // rad; error O(eps^2 * curvature) << relief scale
    const double c = std::cos(kEps), s = std::sin(kEps);
    const auto rot = [&](math::dvec3 e) {
        return math::normalize(d * c + e * s);
    };
    const double h0 = terrain_height(def, stamps, d);
    const double D1 = (terrain_height(def, stamps, rot(e1)) - terrain_height(def, stamps, rot(-e1))) / (2.0 * kEps);
    const double D2 = (terrain_height(def, stamps, rot(e2)) - terrain_height(def, stamps, rot(-e2))) / (2.0 * kEps);
    return math::normalize(d * (mean_radius_m + h0) - e1 * D1 - e2 * D2);
}

} // namespace ve::core::sim
