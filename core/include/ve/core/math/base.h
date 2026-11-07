#pragma once
// core/math base: constants + angle utilities. Pure functions (determinism:
// no wall clock, no state). Frame convention pinned by PLAN decision #34:
// right-handed, Y-up, -Z forward — angles are mathematically positive (CCW)
// about the named axis when viewed from +axis toward the origin.
// fp32 here is for render math and stats values; sim positions use fp64
// (decision #3), so every utility comes in both widths.

#include <cmath>
#include <limits>
#include <type_traits>

namespace ve::core::math {

inline constexpr double kPi_d  = 3.14159265358979323846264338327950288;
inline constexpr double kTau_d = 6.28318530717958647692528676655900577; // 2*pi
inline constexpr double kHalfPi_d = 1.57079632679489661923132169163975144;
inline constexpr float  kPi_f  = 3.14159265358979323846f;
inline constexpr float  kTau_f = 6.28318530717958647693f;
inline constexpr float  kHalfPi_f = 1.57079632679489661923f;

// Defaults chosen per regime (docs/phase-01-math/notes.md): absolute for
// |x|<1, relative beyond — see approximately_equal.
inline constexpr float  kDefaultEpsF = 1e-6f;
inline constexpr double kDefaultEpsD = 1e-12;

constexpr double deg_to_rad(double deg) { return deg * (kPi_d / 180.0); }
constexpr double rad_to_deg(double rad) { return rad * (180.0 / kPi_d); }
constexpr float  deg_to_rad(float deg)  { return deg * (kPi_f / 180.0f); }
constexpr float  rad_to_deg(float rad)  { return rad * (180.0f / kPi_f); }

// Wrap to [-pi, pi). fmod keeps the dividend's sign, so one add/sub of tau
// lands any fmod result in range. Boundary rule: +pi maps to -pi (half-open).
inline double wrap_angle(double a) {
    a = std::fmod(a, kTau_d);
    if (a >= kPi_d) a -= kTau_d;
    else if (a < -kPi_d) a += kTau_d;
    return a;
}
inline float wrap_angle(float a) {
    a = std::fmod(a, kTau_f);
    if (a >= kPi_f) a -= kTau_f;
    else if (a < -kPi_f) a += kTau_f;
    return a;
}

// Mixed-regime comparison (the notes.md contract): eps is *absolute* while
// |x| < 1 (relative eps is meaningless near zero — 1e-12 absolute would let
// 0 and 1e-13 compare equal), and *relative* beyond (ulp spacing grows with
// magnitude, so a fixed absolute eps goes false on values that must match).
// scale = max(|a|, |b|, 1) implements exactly that switch in one expression.
template <typename T>
bool approximately_equal(T a, T b, T eps) {
    static_assert(std::is_floating_point_v<T>);
    const T diff = std::fabs(a - b);
    if (diff == T(0)) return true;
    T scale = std::fabs(a);
    if (std::fabs(b) > scale) scale = std::fabs(b);
    if (scale < T(1)) scale = T(1);
    return diff <= eps * scale;
}
inline bool approximately_equal(float a, float b)  { return approximately_equal(a, b, kDefaultEpsF); }
inline bool approximately_equal(double a, double b){ return approximately_equal(a, b, kDefaultEpsD); }

inline bool is_finite(double a) { return std::isfinite(a); }
inline bool is_finite(float a)  { return std::isfinite(a); }

constexpr double lerp(double a, double b, double t) { return a + (b - a) * t; }
constexpr float  lerp(float a, float b, float t)    { return a + (b - a) * t; }

constexpr double clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr float  clamp(float v, float lo, float hi)    { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace ve::core::math
