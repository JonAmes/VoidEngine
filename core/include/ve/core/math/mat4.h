#pragma once
// core/math 4x4 matrices. Column-major storage (m[col*4+row]): matches the
// memory layout GLSL/Vulkan mat4 consumes and glTF node matrices — zero
// shuffling at the gfx boundary. Convention: right-handed (decision #34),
// column-vector math (v' = M * v); composite order M = T * R * S applies
// scale first, then rotation, then translation.
// Default-constructed mat4 is IDENTITY (never zero) — pinned in math_mat4_ops.

#include <array>
#include <cmath>
#include <optional>

#include "ve/core/math/base.h"
#include "ve/core/math/vec.h"

namespace ve::core::math {

template <typename T>
struct mat4 {
    // column-major: m[c*4 + r]
    std::array<T, 16> m{};
    constexpr mat4() {
        // identity: the safe default (a zero matrix silently deletes geometry)
        m = {T(1), T(0), T(0), T(0),
             T(0), T(1), T(0), T(0),
             T(0), T(0), T(1), T(0),
             T(0), T(0), T(0), T(1)};
    }
    // explicit column-major fill (projection builders and tests)
    constexpr explicit mat4(const std::array<T, 16>& cols) : m(cols) {}

    constexpr vec4<T> col(int c) const { return {m[c*4], m[c*4+1], m[c*4+2], m[c*4+3]}; }
    constexpr vec4<T> row(int r) const { return {m[r], m[4 + r], m[8 + r], m[12 + r]}; }
    static constexpr mat4 zero() { mat4 r; for (auto& v : r.m) v = T(0); return r; }
    // #34 attitude extraction: object axes in world space (rot block only)
    constexpr vec3<T> basis_x() const { return {m[0], m[1], m[2]}; }   // right
    constexpr vec3<T> basis_y() const { return {m[4], m[5], m[6]}; }   // up
    constexpr vec3<T> basis_z() const { return {m[8], m[9], m[10]}; }  // back (+Z = -forward)
    constexpr vec3<T> basis_forward() const { return -basis_z(); }     // decision #34 forward
    constexpr vec3<T> basis_translation() const { return {m[12], m[13], m[14]}; }
};

template <typename T>
constexpr mat4<T> operator*(const mat4<T>& a, const mat4<T>& b) {
    mat4<T> r = mat4<T>::zero();
    for (int c = 0; c < 4; ++c) {
        const vec4<T> bc = b.col(c);
        for (int j = 0; j < 4; ++j)
            r.m[c * 4 + j] = dot(a.row(j), bc);
    }
    return r;
}

template <typename T>
constexpr vec4<T> operator*(const mat4<T>& a, vec4<T> v) {
    return {dot(a.row(0), v), dot(a.row(1), v), dot(a.row(2), v), dot(a.row(3), v)};
}

// ---- affine transforms ----
// point: w=1 in, always perspective-divided out (exact /1 for affine mats,
// and correct for projection matrices too — one contract, no branching).
template <typename T>
constexpr vec3<T> transform_point(const mat4<T>& m, vec3<T> p) {
    const vec4<T> h = m * vec4<T>::point(p);
    return h.w == T(1) ? vec3<T>{h.x, h.y, h.z}
                       : vec3<T>{h.x / h.w, h.y / h.w, h.z / h.w};
}
template <typename T>
constexpr vec3<T> transform_direction(const mat4<T>& m, vec3<T> d) {
    const vec4<T> h = m * vec4<T>::direction(d);
    return {h.x, h.y, h.z};
}

template <typename T> constexpr mat4<T> translation(vec3<T> t) {
    mat4<T> r;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
template <typename T> constexpr mat4<T> scale(vec3<T> s) {
    mat4<T> r;
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

// Right-handed rotations (positive angle turns the first axis toward the
// second about the third: +Z turns +X -> +Y). Sine signs from the RH
// rotation formula: R_ij = cos a (delta) - sin a (Levi-Civita about axis).
template <typename T> inline mat4<T> rotation_x(T a) {
    const T c = std::cos(a), s = std::sin(a);
    return mat4<T>{std::array<T,16>{T(1),T(0),T(0),T(0),  T(0),c,s,T(0),  T(0),-s,c,T(0),  T(0),T(0),T(0),T(1)}};
}
template <typename T> inline mat4<T> rotation_y(T a) {
    const T c = std::cos(a), s = std::sin(a);
    return mat4<T>{std::array<T,16>{c,T(0),-s,T(0),  T(0),T(1),T(0),T(0),  s,T(0),c,T(0),  T(0),T(0),T(0),T(1)}};
}
template <typename T> inline mat4<T> rotation_z(T a) {
    const T c = std::cos(a), s = std::sin(a);
    return mat4<T>{std::array<T,16>{c,s,T(0),T(0),  -s,c,T(0),T(0),  T(0),T(0),T(1),T(0),  T(0),T(0),T(0),T(1)}};
}

// Right-handed view matrix (camera at eye looking at target; the camera's
// forward is -Z of view space, decision #34). Up-parallel-to-forward is the
// polar degenerate case: we fall back to the world axis least aligned with
// forward instead of producing NaN (camera looking straight up/down must work).
template <typename PosT>
mat4<PosT> look_at_rh(vec3<PosT> eye, vec3<PosT> target, vec3<PosT> up) {
    const vec3<PosT> f = normalize(target - eye);              // forward
    vec3<PosT> up_try = up;
    if (length(cross(f, up_try)) < PosT(1e-6)) {               // degenerate up
        up_try = (std::abs(f.y) < PosT(0.9)) ? vec3<PosT>{0, PosT(1), 0}
                                             : vec3<PosT>{PosT(1), 0, 0};
    }
    const vec3<PosT> s = normalize(cross(f, up_try));          // right (== #34 right_of sense)
    const vec3<PosT> u = cross(-f, s);                         // camera up
    mat4<PosT> r = mat4<PosT>::zero();
    // rows: [s | -dot(s,e)], [u | -dot(u,e)], [-f | dot(f,e)] (column-major store)
    r.m[0]=s.x;  r.m[4]=s.y;  r.m[8]=s.z;   r.m[12]=-dot(s, eye);
    r.m[1]=u.x;  r.m[5]=u.y;  r.m[9]=u.z;   r.m[13]=-dot(u, eye);
    r.m[2]=-f.x; r.m[6]=-f.y; r.m[10]=-f.z; r.m[14]= dot(f, eye);
    r.m[15]=PosT(1);
    return r;
}

template <typename T> constexpr mat4<T> transpose(const mat4<T>& a) {
    mat4<T> r = mat4<T>::zero();
    for (int c = 0; c < 4; ++c)
        for (int rw = 0; rw < 4; ++rw)
            r.m[c * 4 + rw] = a.m[rw * 4 + c];
    return r;
}

// General inverse via cofactor (Laplace) expansion: inv(r,c) =
// (-1)^(r+c) * det(minor dropping row c, col r) / det (adjugate form). Minor is built
// by an indexed scan (dd skips deleted row/col) — deterministic, no
// hand-typed index soup, and fully inlined by -O2. det expands along row 0.
// Returns nullopt for singular inputs (core is exception-free).
template <typename T>
std::optional<mat4<T>> invert(const mat4<T>& a) {
    auto minor_det = [&a](int del_r, int del_c) {
        // collect the 3x3 remaining rows, then expand along its first row
        T e[3][3]{};
        for (int rr = 0, i = 0; rr < 4; ++rr) {
            if (rr == del_r) continue;
            for (int cc = 0, j = 0; cc < 4; ++cc) {
                if (cc == del_c) continue;
                e[i][j++] = a.m[cc * 4 + rr];
            }
            ++i;
        }
        return e[0][0] * (e[1][1]*e[2][2] - e[1][2]*e[2][1])
             - e[0][1] * (e[1][0]*e[2][2] - e[1][2]*e[2][0])
             + e[0][2] * (e[1][0]*e[2][1] - e[1][1]*e[2][0]);
    };
    const auto& m = a.m;
    // det along logical row 0: its elements live at m[0],m[4],m[8],m[12]
    // (column-major!), each paired with the minor dropping (row 0, col c)
    const T det = m[0] * minor_det(0,0) - m[4] * minor_det(0,1)
                + m[8] * minor_det(0,2) - m[12] * minor_det(0,3);
    if (det > T(-1e-12) && det < T(1e-12))   // absolute band: singularity test
        return std::nullopt;
    mat4<T> out = mat4<T>::zero();
    const T inv_det = T(1) / det;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            const T sign = ((r + c) % 2 == 0) ? T(1) : T(-1);
            // inv(r,c) = adj(r,c)/det with adj(r,c) = (-1)^(r+c) * minor
            // dropping ROW c, COL r (adjugate = transposed cofactor matrix)
            out.m[c * 4 + r] = sign * minor_det(c, r) * inv_det;
        }
    return out;
}

// Fast inverse for rigid (rot+translate, uniform scale 1) matrices:
// inv = [R^T | -R^T t]. The rigid precondition is CHECKED and reported
// (nullopt), not asserted — sim code may legitimately hand us a malformed
// transform from authored data, and core is exception-free.
template <typename T>
std::optional<mat4<T>> invert_rigid(const mat4<T>& a) {
    constexpr T kOrthonormEps = T(1e-4);  // authored rotations land far inside
    const vec3<T> x = a.basis_x(), y = a.basis_y(), z = a.basis_z(), t = a.basis_translation();
    if (!approximately_equal(length2(x), T(1), kOrthonormEps) ||
        !approximately_equal(length2(y), T(1), kOrthonormEps) ||
        !approximately_equal(length2(z), T(1), kOrthonormEps) ||
        !approximately_equal(dot(x, y), T(0), kOrthonormEps) ||
        !approximately_equal(dot(y, z), T(0), kOrthonormEps) ||
        !approximately_equal(dot(z, x), T(0), kOrthonormEps) ||
        !approximately_equal(a.m[15], T(1), kOrthonormEps))
        return std::nullopt;
    // transpose rotation block; translation column = -R^T t
    mat4<T> r;
    r.m[0] = a.m[0]; r.m[1] = a.m[4]; r.m[2] = a.m[8];
    r.m[4] = a.m[1]; r.m[5] = a.m[5]; r.m[6] = a.m[9];
    r.m[8] = a.m[2]; r.m[9] = a.m[6]; r.m[10] = a.m[10];
    // (R^T t)_i = dot(column i of R, t) = dot(basis_i, t)
    const vec3<T> rt{-dot(x, t), -dot(y, t), -dot(z, t)};
    r.m[12] = rt.x; r.m[13] = rt.y; r.m[14] = rt.z;
    return r;
}

template <typename T>
bool approximately_equal(const mat4<T>& a, const mat4<T>& b, T eps) {
    for (int i = 0; i < 16; ++i)
        if (!approximately_equal(a.m[i], b.m[i], eps)) return false;
    return true;
}

using mat4f  = mat4<float>;
using dmat4  = mat4<double>;

} // namespace ve::core::math
