#pragma once
//
// Phase 1.7 — coordinate utilities.
//
// Spherical <-> cartesian, #34-aware: Y-up means the polar axis is +Y, so we
// use (range, azimuth around +Y, elevation off the XZ plane) instead of the
// math-textbook inclination form — azimuth then matches yaw and stays
// intuitive for authored directions. POLAR SINGULARITY: at elevation ±pi/2
// azimuth is undefined; to_spherical pins it to 0 deterministically (never
// NaN, never "whatever atan2 felt like") so sim code stays replay-safe.
//
// Floating origin (required by decision #3 real-scale): sim positions are
// fp64 world coords, but fp32 render math dies past ~1e7 m (see
// math_fp64_precision). The fix is a world origin: entities keep world
// positions; render/UI work in local = world - origin coords, small enough
// for fp32. Recenter = one shared shift applied to EVERY live position, so
// inter-entity deltas are preserved (each subtraction rounds independently —
// error bounded by a couple of ulps, tested).

#include <cmath>

#include "ve/core/math/base.h"
#include "ve/core/math/vec.h"

namespace ve::core::math {

template <typename T>
struct spherical { T range{}, az_rad{}, el_rad{}; };  // el in [-pi/2, pi/2], az in [-pi, pi)

template <typename T>
constexpr vec3<T> spherical_to_cart(const spherical<T>& s) {
    const T c = std::cos(s.el_rad);
    return {s.range * c * std::cos(s.az_rad),   // az=0 -> +X
            s.range * std::sin(s.el_rad),
            s.range * c * std::sin(s.az_rad)};  // az=+pi/2 -> +Z
}

template <typename T>
spherical<T> cart_to_spherical(const vec3<T>& v) {
    spherical<T> s;
    s.range = length(v);
    if (s.range == T(0)) return s;                       // origin: az/el arbitrary, pinned 0
    s.el_rad = std::asin(v.y / s.range);                 // clamp handled by asin domain rules
    if (std::abs(v.x) < T(1e-12) && std::abs(v.z) < T(1e-12)) {
        s.az_rad = T(0);                                 // exact poles: singularity pinned
    } else {
        s.az_rad = wrap_angle(std::atan2(v.z, v.x));
    }
    return s;
}

// ---- floating origin ----

// One origin per sim world; positions themselves stay ABSOLUTE fp64 and are
// NEVER rewritten (that would break replay identity). Only the local frame
// moves: local = world - origin, small enough for fp32 render math.
struct origin_frame {
    dvec3 origin{};

    constexpr dvec3 to_local(const dvec3& world) const { return world - origin; }
    constexpr dvec3 to_world(const dvec3& local) const { return local + origin; }

    // 8.9: the ONLY sanctioned path from world coords to GPU (fp32) space —
    // subtract in fp64 FIRST, then cast. The reverse order destroys metre
    // detail past ~1e7 m (fp32 ulp at 1 AU is 16 km); the order here keeps
    // locals as close to the world delta as fp32 can represent.
    constexpr vec3f to_local_f32(const dvec3& world) const {
        const dvec3 l = to_local(world);
        return vec3f{float(l.x), float(l.y), float(l.z)};
    }

    // Pure comparison: decision depends only on these numbers (replay-safe).
    bool needs_shift(const dvec3& focus, double threshold) const {
        return length(focus - origin) > threshold;
    }
    // Move the local frame onto focus. World positions do NOT change.
    // Returns the delta to add to any CACHED locals (local_new =
    // local_old + delta). Deterministic: no clock, no iteration order.
    dvec3 shift_to(const dvec3& focus) {
        const dvec3 delta = origin - focus;   // world fixed; locals translate by this
        origin = focus;
        return delta;
    }
};

} // namespace ve::core::math
