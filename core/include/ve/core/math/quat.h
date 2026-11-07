#pragma once
// core/math quaternions, euler angles, rig frames. Convention
// (decision #34): right-handed; rotation quaternion acts v' = q v q*
// (Hamilton; the product (a*b) applies b first, then a — same composition
// order as mat4 M1*M2). Euler convention (docs/phase-01-math/notes.md):
// intrinsic yaw(Y) -> pitch(X) -> roll(Z): q = qYaw * qPitch * qRoll.
// Attitude doctrine: the sim integrates angular velocity into the
// quaternion and renormalizes every fixed step; euler is authoring/UI only
// (accumulated euler gimbal-couples and endangers replay determinism).
// fp32 attitude is the rendering/stats width; sim ships dquat where the
// step count demands it (same template).

#include <cmath>
#include <optional>

#include "ve/core/math/base.h"
#include "ve/core/math/mat4.h"
#include "ve/core/math/vec.h"

namespace ve::core::math {

template <typename T>
struct quat {
    // Hamilton: (x,y,z) vector part + w scalar; default = identity
    T x{}, y{}, z{}, w{T(1)};
    friend constexpr bool operator==(const quat&, const quat&) = default;  // tests/contracts only
};

using quatf = quat<float>;
using dquat = quat<double>;

template <typename T>
constexpr T dot(const quat<T>& a, const quat<T>& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
}
template <typename T> constexpr T length2(const quat<T>& q) { return dot(q, q); }
template <typename T> T length(const quat<T>& q) { return std::sqrt(length2(q)); }

template <typename T>
constexpr quat<T> operator*(const quat<T>& a, const quat<T>& b) {
    // Hamilton product: "a after b"
    return {a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}

template <typename T> constexpr quat<T> conjugate(const quat<T>& q) { return {-q.x, -q.y, -q.z, q.w}; }

// componentwise ops (interpolation helpers only — composition is operator*)
template <typename T> constexpr quat<T> operator+(const quat<T>& a, const quat<T>& b) { return {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; }
template <typename T> constexpr quat<T> operator-(const quat<T>& a, const quat<T>& b) { return {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}; }
template <typename T> constexpr quat<T> operator*(const quat<T>& q, T s) { return {q.x*s, q.y*s, q.z*s, q.w*s}; }

template <typename T>
quat<T> normalize(const quat<T>& q) {
    const T len = length(q);
    if (len == T(0)) return quat<T>{};   // zero -> identity (zero-safe, no NaN)
    return {q.x/len, q.y/len, q.z/len, q.w/len};
}

// Unit-quaternion inverse; input must be near-unit (checked — core is
// exception-free, a drifting quat from bad integration must not silently
// produce scaled rotations).
template <typename T>
std::optional<quat<T>> invert_unit(const quat<T>& q) {
    if (!approximately_equal(length2(q), T(1), T(1e-3))) return std::nullopt;
    return conjugate(q);
}

// Right-handed axis-angle: positive angle turns CCW looking BACK along the
// axis (thumb = axis, fingers curl first->second: +X toward +Y about +Z).
template <typename T>
quat<T> quat_from_axis_angle(vec3<T> axis, T angle) {
    const vec3<T> a = normalize(axis);   // zero axis -> identity (no NaN)
    const T half = angle / T(2), s = std::sin(half);
    return {a.x*s, a.y*s, a.z*s, std::cos(half)};
}

// ---- single-axis helpers (decision #34: up = +Y) ----
template <typename T> quat<T> quat_yaw(T rad)   { return quat_from_axis_angle(vec3<T>{T(0),T(1),T(0)}, rad); }
template <typename T> quat<T> quat_pitch(T rad) { return quat_from_axis_angle(vec3<T>{T(1),T(0),T(0)}, rad); }
template <typename T> quat<T> quat_roll(T rad)  { return quat_from_axis_angle(vec3<T>{T(0),T(0),T(1)}, rad); }

// ---- euler (DEGREES) <-> quaternion, notes.md order q = Yaw*Pitch*Roll ----
template <typename T>
quat<T> quat_from_euler(T yaw_deg, T pitch_deg, T roll_deg) {
    return quat_yaw(deg_to_rad(yaw_deg)) * quat_pitch(deg_to_rad(pitch_deg))
         * quat_roll(deg_to_rad(roll_deg));
}

template <typename T>
struct euler_angles { T yaw_deg{}, pitch_deg{}, roll_deg{}; };

// Inverse of quat_from_euler (Tait-Bryan YXZ). Near gimbal lock
// (|pitch| -> 90 deg) the yaw/roll split is degenerate; we fold the whole
// residual into yaw with roll=0 (the conventional stable choice) so the
// round-trip still reproduces the rotation exactly.
template <typename T>
euler_angles<T> quat_to_euler(const quat<T>& q) {
    const quat<T> u = normalize(q);
    // R elements in terms of q (Hamilton v'=qvq*):
    //   R[1][2] = 2(yz - wx) etc.; pitch = asin(-R[1][2]) = asin(2(wx-yz))
    T sp = T(2) * (u.w*u.x - u.y*u.z);
    if (sp > T(1))  sp = T(1);   // clamp: fp roundoff outside asin domain
    if (sp < T(-1)) sp = T(-1);
    constexpr T kLockCos = T(1e-6);
    if (T(1) - sp*sp < kLockCos) {  // cos(pitch) ~ 0: gimbal lock branch
        // roll folded into yaw: yaw = atan2(-R[2][0], R[0][0])
        const T r20 = T(2) * (u.x*u.z - u.w*u.y);
        const T r00 = T(1) - T(2) * (u.y*u.y + u.z*u.z);
        return {rad_to_deg(std::atan2(-r20, r00)), rad_to_deg(std::asin(sp)), T(0)};
    }
    const T yaw  = std::atan2(T(2)*(u.x*u.z + u.w*u.y),
                              T(1) - T(2)*(u.x*u.x + u.y*u.y)); // atan2(R[0][2],R[2][2])
    const T roll = std::atan2(T(2)*(u.x*u.y + u.w*u.z),
                              T(1) - T(2)*(u.x*u.x + u.z*u.z)); // atan2(R[1][0],R[1][1])
    return {rad_to_deg(yaw), rad_to_deg(std::asin(sp)), rad_to_deg(roll)};
}

// ---- quaternion <-> mat4 ----
template <typename T>
mat4<T> to_mat4(const quat<T>& q) {
    // rotation part of q acting as v' = q v q* (basis columns of #34 frame)
    const T x = q.x, y = q.y, z = q.z, w = q.w;
    return mat4<T>{std::array<T,16>{
        T(1)-T(2)*(y*y+z*z), T(2)*(x*y+w*z),       T(2)*(x*z-w*y),       T(0),
        T(2)*(x*y-w*z),      T(1)-T(2)*(x*x+z*z),  T(2)*(y*z+w*x),       T(0),
        T(2)*(x*z+w*y),      T(2)*(y*z-w*x),       T(1)-T(2)*(x*x+y*y),  T(0),
        T(0),                T(0),                 T(0),                 T(1)}};
}

// Extract the rotation from a rigid mat4 (translation ignored). Returns
// nullopt when the 3x3 block is not orthonormal (scaled/sheared input is a
// data error, not a rotation). Shepperd's method — numerically stable in
// every attitude quadrant.
template <typename T>
std::optional<quat<T>> quat_from_mat4(const mat4<T>& m) {
    constexpr T kOrthonormEps = T(1e-4);
    const vec3<T> x = m.basis_x(), y = m.basis_y(), z = m.basis_z();
    if (!approximately_equal(length2(x), T(1), kOrthonormEps) ||
        !approximately_equal(length2(y), T(1), kOrthonormEps) ||
        !approximately_equal(length2(z), T(1), kOrthonormEps) ||
        !approximately_equal(dot(x, y), T(0), kOrthonormEps) ||
        !approximately_equal(dot(y, z), T(0), kOrthonormEps) ||
        !approximately_equal(dot(z, x), T(0), kOrthonormEps))
        return std::nullopt;
    const T r00 = m.m[0], r11 = m.m[5], r22 = m.m[10];
    const T trace = r00 + r11 + r22;
    if (trace > T(0)) {
        const T s = std::sqrt(trace + T(1)) * T(2);   // s = 4w
        return quat<T>{(m.m[6] - m.m[9]) / s, (m.m[8] - m.m[2]) / s, (m.m[1] - m.m[4]) / s, s / T(4)};
    } else if (r00 > r11 && r00 > r22) {
        const T s = std::sqrt(T(1) + r00 - r11 - r22) * T(2);   // s = 4x
        return quat<T>{s / T(4), (m.m[1] + m.m[4]) / s, (m.m[2] + m.m[8]) / s, (m.m[6] - m.m[9]) / s};
    } else if (r11 > r22) {
        const T s = std::sqrt(T(1) + r11 - r00 - r22) * T(2);   // s = 4y
        return quat<T>{(m.m[1] + m.m[4]) / s, s / T(4), (m.m[6] + m.m[9]) / s, (m.m[8] - m.m[2]) / s};
    } else {
        const T s = std::sqrt(T(1) + r22 - r00 - r11) * T(2);   // s = 4z
        return quat<T>{(m.m[2] + m.m[8]) / s, (m.m[6] + m.m[9]) / s, s / T(4), (m.m[1] - m.m[4]) / s};
    }
}

// Rotate a vector: v' = q v q*. Written out: t = 2(q_vec x v);
// v' = v + w*t + q_vec x t (standard Hamilton identity, 2 crosses).
template <typename T>
constexpr vec3<T> rotate(const quat<T>& q, vec3<T> v) {
    const vec3<T> qv{q.x, q.y, q.z};
    const vec3<T> t = cross(qv, v) * T(2);
    return v + q.w * t + cross(qv, t);
}

// Spherical interpolation, shortest arc (q and -q are the same rotation).
template <typename T>
quat<T> slerp(quat<T> a, quat<T> b, T t) {
    if (dot(a, b) < T(0)) { b = {-b.x, -b.y, -b.z, -b.w}; }   // shortest arc
    T d = dot(a, b);
    if (d > T(0.9995)) return nlerp(a, b, t);                 // linear near-parallel
    const T theta = std::acos(d > T(1) ? T(1) : d), sth = std::sin(theta);
    const T w1 = std::sin((T(1) - t) * theta) / sth, w2 = std::sin(t * theta) / sth;
    return {a.x*w1 + b.x*w2, a.y*w1 + b.y*w2, a.z*w1 + b.z*w2, a.w*w1 + b.w*w2};
}

// Normalized linear interpolation — cheap stand-in when angular deltas are
// small (camera smoothing, attachment blending). Cheaper than slerp,
// visually identical under ~10 deg.
template <typename T>
quat<T> nlerp(quat<T> a, quat<T> b, T t) {
    if (dot(a, b) < T(0)) b = {-b.x, -b.y, -b.z, -b.w};   // shortest arc
    return normalize(a + (b - a) * t);
}

// ---- TRS decompose / compose (authored node matrices from glTF cook) ----
template <typename T>
struct trs { vec3<T> position{}; quat<T> rotation{}; vec3<T> scale{T(1), T(1), T(1)}; };
using trsf = trs<float>;
using trsd = trs<double>;

// M = T * R * S (same order as everything else). A negative-determinant
// block (mirrored authoring, legal in glTF) is folded into scale.x < 0;
// zero-scale columns are degenerate -> nullopt.
template <typename T>
std::optional<trs<T>> trs_decompose(const mat4<T>& m) {
    vec3<T> sx = m.basis_x(), sy = m.basis_y(), sz = m.basis_z();
    const T lx = length(sx), ly = length(sy), lz = length(sz);
    if (lx == T(0) || ly == T(0) || lz == T(0)) return std::nullopt;
    // rotation block = columns / scale; check it ORTHONORMALIZES (a shear
    // cannot TRS-decompose and must be reported, not silently approximated)
    sx = sx / lx; sy = sy / ly; sz = sz / lz;
    constexpr T kOrthEps = T(1e-4);
    if (!approximately_equal(dot(sx, sy), T(0), kOrthEps) ||
        !approximately_equal(dot(sy, sz), T(0), kOrthEps) ||
        !approximately_equal(dot(sz, sx), T(0), kOrthEps))
        return std::nullopt;
    trs<T> out;
    out.position = m.basis_translation();
    out.scale = {lx, ly, lz};
    const T det3 = dot(sx, cross(sy, sz));
    if (det3 < T(0)) { out.scale.x = -lx; sx = sx * T(-1); }  // mirror -> negative x scale
    // build orthonormal mat to reuse Shepperd extraction
    mat4<T> r;
    r.m[0]=sx.x; r.m[1]=sx.y; r.m[2]=sx.z;
    r.m[4]=sy.x; r.m[5]=sy.y; r.m[6]=sy.z;
    r.m[8]=sz.x; r.m[9]=sz.y; r.m[10]=sz.z;
    auto q = quat_from_mat4(r);
    if (!q) return std::nullopt;
    out.rotation = *q;
    return out;
}

template <typename T>
mat4<T> trs_compose(const trs<T>& t) {
    return translation(t.position) * to_mat4(t.rotation) * scale(t.scale);
}

// Integrate body angular velocity (rad/s, BODY frame) over one step:
// q_next = normalize( q * exp_half(omega*dt) ) — right-multiply because
// omega is measured in the object's own frame. Deterministic: fixed dt,
// no wall-clock; the normalize keeps drift bounded identically on every run.
template <typename T>
quat<T> integrate_angular(const quat<T>& q, vec3<T> omega_body, T dt) {
    const T speed = length(omega_body);
    if (speed == T(0)) return q;
    const quat<T> dq = quat_from_axis_angle(omega_body / speed, speed * dt);
    return normalize(q * dq);
}

// ---- rig frame: sim attitude + position (decision #34 semantics) ----
template <typename PosT>
struct rigid_frame {
    vec3<PosT> position{};
    quatf rotation{};                      // attitude; fp32 (stats/render width)
    vec3f basis_right()   const { return rotate(rotation, kAxisXf); }
    vec3f basis_up()      const { return rotate(rotation, kAxisYf); }
    vec3f basis_forward() const { return rotate(rotation, kForwardf); }
};

// model matrix for the gfx boundary: T(pos) * R(rot), fp32 output
template <typename PosT>
mat4f to_model_matrix(const rigid_frame<PosT>& f) {
    mat4f r = to_mat4(f.rotation);
    r.m[12] = static_cast<float>(f.position.x);
    r.m[13] = static_cast<float>(f.position.y);
    r.m[14] = static_cast<float>(f.position.z);
    return r;
}

using rigf  = rigid_frame<float>;
using drig  = rigid_frame<double>;   // sim frame: fp64 position (decision #3/#22)

} // namespace ve::core::math
