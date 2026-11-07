#pragma once
// core/math vectors. Hand-rolled per doctrine: plain aggregates + operators,
// no expression templates (nothing to optimize yet; clarity + determinism).
// Width rule (decision #3): render math uses float aliases (vec3f...),
// sim positions/velocities use double aliases (dvec3...). Frame convention is
// decision #34 (RH, Y-up, -Z fwd); pinned by tests/math_vec.cpp.

#include "ve/core/math/base.h"

namespace ve::core::math {

template <typename T>
struct vec2 {
    T x{}, y{};
    constexpr vec2() = default;
    constexpr vec2(T x_, T y_) : x(x_), y(y_) {}
    friend constexpr bool operator==(const vec2&, const vec2&) = default; // exact compare: tests/contracts only, never sim logic
};

template <typename T>
struct vec3 {
    T x{}, y{}, z{};
    constexpr vec3() = default;
    constexpr vec3(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}
    friend constexpr bool operator==(const vec3&, const vec3&) = default;
};

template <typename T>
struct vec4 {
    T x{}, y{}, z{}, w{};
    constexpr vec4() = default;
    constexpr vec4(T x_, T y_, T z_, T w_) : x(x_), y(y_), z(z_), w(w_) {}
    friend constexpr bool operator==(const vec4&, const vec4&) = default;
    // w-semantics: points carry w=1 (positions, affected by translation),
    // vectors carry w=0 (directions, immune to translation) — used by mat4.
    static constexpr vec4 point(const vec3<T>& p) { return {p.x, p.y, p.z, T(1)}; }
    static constexpr vec4 direction(const vec3<T>& d) { return {d.x, d.y, d.z, T(0)}; }
};

// ---- component-wise ops + scalar ops (constexpr: no transcendentals) ----
// explicit impls per width keep -Werror happy without SFINAE gymnastics
template <typename T> constexpr vec2<T> operator+(vec2<T> a, vec2<T> b) { return {a.x + b.x, a.y + b.y}; }
template <typename T> constexpr vec2<T> operator-(vec2<T> a, vec2<T> b) { return {a.x - b.x, a.y - b.y}; }
template <typename T> constexpr vec2<T> operator-(vec2<T> a) { return {-a.x, -a.y}; }
template <typename T> constexpr vec2<T> operator*(vec2<T> v, T s) { return {v.x * s, v.y * s}; }
template <typename T> constexpr vec2<T> operator*(T s, vec2<T> v) { return v * s; }

template <typename T> constexpr vec4<T> operator+(vec4<T> a, vec4<T> b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
template <typename T> constexpr vec4<T> operator-(vec4<T> a, vec4<T> b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
template <typename T> constexpr vec4<T> operator-(vec4<T> a) { return {-a.x, -a.y, -a.z, -a.w}; }
template <typename T> constexpr vec4<T> operator*(vec4<T> v, T s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
template <typename T> constexpr vec4<T> operator*(T s, vec4<T> v) { return v * s; }

// vec3 written explicitly below:
template <typename T> constexpr vec3<T> operator+(vec3<T> a, vec3<T> b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
template <typename T> constexpr vec3<T> operator-(vec3<T> a, vec3<T> b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
template <typename T> constexpr vec3<T> operator-(vec3<T> a) { return {-a.x, -a.y, -a.z}; }
template <typename T> constexpr vec3<T> operator*(vec3<T> v, T s) { return {v.x * s, v.y * s, v.z * s}; }
template <typename T> constexpr vec3<T> operator*(T s, vec3<T> v) { return v * s; }
template <typename T> constexpr vec3<T> operator/(vec3<T> v, T s) { return {v.x / s, v.y / s, v.z / s}; }

// ---- products / lengths (float versions need <cmath>: std::sqrt) ----
template <typename T> constexpr T dot(vec2<T> a, vec2<T> b) { return a.x * b.x + a.y * b.y; }
template <typename T> constexpr T dot(vec3<T> a, vec3<T> b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
template <typename T> constexpr T dot(vec4<T> a, vec4<T> b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

// RIGHT-HANDED cross (decision #34 frame): cross(X,Y) = Z, cyclic.
template <typename T> constexpr vec3<T> cross(vec3<T> a, vec3<T> b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

template <typename T> constexpr T length2(vec2<T> v) { return dot(v, v); }
template <typename T> constexpr T length2(vec3<T> v) { return dot(v, v); }
template <typename T> inline T length(vec2<T> v) { return std::sqrt(length2(v)); }
template <typename T> inline T length(vec3<T> v) { return std::sqrt(length2(v)); }

// Zero-safe: normalizing the zero vector returns zero (documented contract;
// callers who need an error signal check length2 first — determinism beats
// an assert in sim hot paths).
template <typename T> inline vec3<T> normalize(vec3<T> v) {
    const T len = length(v);
    return len == T(0) ? vec3<T>{} : v / len;
}
template <typename T> inline vec2<T> normalize(vec2<T> v) {
    const T len = length(v);
    return len == T(0) ? vec2<T>{} : v / len;
}

template <typename T> constexpr vec2<T> lerp(vec2<T> a, vec2<T> b, T t) { return a + (b - a) * t; }
template <typename T> constexpr vec3<T> lerp(vec3<T> a, vec3<T> b, T t) { return a + (b - a) * t; }

// proj of a onto b = (a·b / b·b) b ; rej = a - proj (perpendicular remainder).
// Used by assist controllers (well crossfades, slide-along-surface clamps).
template <typename T> constexpr vec3<T> project(vec3<T> a, vec3<T> onto) {
    const T bb = length2(onto);
    return bb == T(0) ? vec3<T>{} : onto * (dot(a, onto) / bb);
}
template <typename T> constexpr vec3<T> reject(vec3<T> a, vec3<T> keep_out) {
    return a - project(a, keep_out);
}

template <typename T> bool approximately_equal(vec2<T> a, vec2<T> b, T eps) {
    return approximately_equal(a.x, b.x, eps) && approximately_equal(a.y, b.y, eps);
}
template <typename T> bool approximately_equal(vec3<T> a, vec3<T> b, T eps) {
    return approximately_equal(a.x, b.x, eps) && approximately_equal(a.y, b.y, eps)
        && approximately_equal(a.z, b.z, eps);
}
template <typename T> bool approximately_equal(vec4<T> a, vec4<T> b, T eps) {
    return approximately_equal(a.x, b.x, eps) && approximately_equal(a.y, b.y, eps)
        && approximately_equal(a.z, b.z, eps) && approximately_equal(a.w, b.w, eps);
}

// ---- aliases: fp32 render aliases + fp64 sim aliases (decision #3) ----
using vec2f = vec2<float>;  using vec3f = vec3<float>;  using vec4f = vec4<float>;
using dvec2 = vec2<double>; using dvec3 = vec3<double>; using dvec4 = vec4<double>;

// Decision #34 basis (frame pinned here, asserted by convention test):
inline constexpr vec3f kAxisXf{1.f, 0.f, 0.f};      // right
inline constexpr vec3f kAxisYf{0.f, 1.f, 0.f};      // up
inline constexpr vec3f kAxisZf{0.f, 0.f, 1.f};
inline constexpr vec3f kForwardf{0.f, 0.f, -1.f};   // -Z is forward
// right = cross(forward, up): (0,0,-1)x(0,1,0) = (+1,0,0). Handy RH identity;
// also pins the right vector a camera rig must produce.
template <typename T> constexpr vec3<T> right_of(vec3<T> forward, vec3<T> up) {
    return cross(forward, up);
}

} // namespace ve::core::math
