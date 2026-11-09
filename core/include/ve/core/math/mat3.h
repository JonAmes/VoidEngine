#pragma once
// core/math 3x3 matrices. Same contract as mat4 (decision #34 conventions):
// column-major storage (m[c*3 + r]), column-vector math (v' = M * v),
// default-constructed = IDENTITY. Lands in Phase 5 for the full inertia
// tensor (decision #45b: diagonal-only through Phase 4 by design). No
// gfx boundary needs 3x3 — this is sim-side math (inertia, attitude bases).

#include <array>

#include "ve/core/math/base.h"
#include "ve/core/math/vec.h"

namespace ve::core::math {

template <typename T>
struct mat3 {
    // column-major: m[c*3 + r]
    std::array<T, 9> m{};
    constexpr mat3() {
        m = {T(1), T(0), T(0),
             T(0), T(1), T(0),
             T(0), T(0), T(1)};
    }
    constexpr explicit mat3(const std::array<T, 9>& cols) : m(cols) {}

    constexpr vec3<T> col(int c) const { return {m[c*3], m[c*3+1], m[c*3+2]}; }
    constexpr vec3<T> row(int r) const { return {m[r], m[3 + r], m[6 + r]}; }
    static constexpr mat3 zero() { mat3 r; for (auto& v : r.m) v = T(0); return r; }

    // diag matrix from a vector (principal-axis inertias, scales)
    static constexpr mat3 diag(vec3<T> d) {
        mat3 r = mat3::zero();
        r.m[0] = d.x; r.m[4] = d.y; r.m[8] = d.z;
        return r;
    }
};

template <typename T>
constexpr mat3<T> operator*(const mat3<T>& a, const mat3<T>& b) {
    mat3<T> r = mat3<T>::zero();
    for (int c = 0; c < 3; ++c) {
        const vec3<T> bc = b.col(c);
        for (int j = 0; j < 3; ++j)
            r.m[c * 3 + j] = dot(a.row(j), bc);
    }
    return r;
}

template <typename T>
constexpr vec3<T> operator*(const mat3<T>& a, vec3<T> v) {
    return {dot(a.row(0), v), dot(a.row(1), v), dot(a.row(2), v)};
}

// Outer product u v^T (parallel-axis term uses this; column c = u * v[c]).
template <typename T>
constexpr mat3<T> outer(vec3<T> u, vec3<T> v) {
    return mat3<T>{{u.x * v.x, u.y * v.x, u.z * v.x,
                    u.x * v.y, u.y * v.y, u.z * v.y,
                    u.x * v.z, u.y * v.z, u.z * v.z}};
}

template <typename T>
constexpr mat3<T> transpose(const mat3<T>& a) {
    return mat3<T>{{a.m[0], a.m[3], a.m[6],
                    a.m[1], a.m[4], a.m[7],
                    a.m[2], a.m[5], a.m[8]}};
}

template <typename T>
constexpr T determinant(const mat3<T>& a) {
    return dot(a.col(0), cross(a.col(1), a.col(2)));  // scalar triple product
}

template <typename T> constexpr mat3<T> operator+(const mat3<T>& a, const mat3<T>& b) {
    mat3<T> r = mat3<T>::zero();
    for (int i = 0; i < 9; ++i) r.m[i] = a.m[i] + b.m[i];
    return r;
}
template <typename T> constexpr mat3<T> operator-(const mat3<T>& a, const mat3<T>& b) {
    mat3<T> r = mat3<T>::zero();
    for (int i = 0; i < 9; ++i) r.m[i] = a.m[i] - b.m[i];
    return r;
}
template <typename T> constexpr mat3<T> operator*(T s, const mat3<T>& a) {
    mat3<T> r = mat3<T>::zero();
    for (int i = 0; i < 9; ++i) r.m[i] = s * a.m[i];
    return r;
}
template <typename T> constexpr mat3<T> operator*(const mat3<T>& a, T s) { return s * a; }

// Inverse via adjugate/determinant (3x3 only; callers must ensure det != 0 —
// inertia tensors are positive definite by construction, so det > 0).
template <typename T>
constexpr mat3<T> inverse(const mat3<T>& a) {
    // Cofactors (C[r][c] = cofactor at row r col c); adjugate = C^T.
    const vec3<T> c0 = a.col(0), c1 = a.col(1), c2 = a.col(2);
    const vec3<T> r0 = cross(c1, c2), r1 = cross(c2, c0), r2 = cross(c0, c1);
    const T det = dot(c0, r0);
    // adjugate columns are the cross products (that IS C^T in column-major).
    mat3<T> adj{{r0.x, r1.x, r2.x,
                 r0.y, r1.y, r2.y,
                 r0.z, r1.z, r2.z}};
    // Note: r0/r1/r2 above are the ROWS of the cofactor expansion; putting
    // them in columns of `adj` gives the adjugate. Divide each entry by det.
    mat3<T> out = mat3<T>::zero();
    for (int i = 0; i < 9; ++i) out.m[i] = adj.m[i] / det;
    return out;
}

using fmat3 = mat3<float>;
using dmat3 = mat3<double>;

}  // namespace ve::core::math
