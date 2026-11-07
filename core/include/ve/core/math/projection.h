#pragma once
//
// Phase 1.6 — projection builders. THE VR-critical task: every perspective
// builder is implemented through the off-axis asymmetric form (AGENTS VR rule:
// passes must accept L/R/T/B tan parameters from day one; a symmetric-only
// shortcut would make per-eye off-axis frustum fitting for HMDs a rewrite).
//
// Conventions (decision #34 + Vulkan, our only gfx API):
//   view space: right-handed, camera looks down -Z, +Y up.
//   clip space: Vulkan. x,y in [-1,1], z in [0,1] (NDC y points DOWN —
//   hence the -2 row on y; pin tested), w = -z_view (pinhole).
//
// Off-axis derivation (l,r,b,t are TANGENTS of the four frustum planes,
// signed: left/bottom negative). Require ndc.x = clip.x/w linear in x,z with
// w = -z:  at z=-n: x=l*n -> -1, x=r*n -> +1.
//   clip.x = A x + B z;  A*l*n - B*n = -n -> A l - B = -1
//                        A*r*n - B*n = +n -> A r - B = +1
//   => A = 2/(r-l), B = (r+l)/(r-l).
// NOTE A has NO factor of n: that form only holds when l,r are near-plane
// LENGTHS; with tans the same edge maps correctly at EVERY depth
// (test checks far-plane corners too).
//   clip.y = -(C y + D z) with C = 2/(t-b), D = (t+b)/(t-b)  (negate = Vulkan y-flip).
//
// depth_range variants (Vulkan z):
//   standard: -n -> 0, -f -> 1  (GLM-style: -f/(f-n), -fn/(f-n))
//   reversed: -n -> 1, -f -> 0  (better fp precision near the camera)
//   reversed_inf_far: far at infinity: clip.z = n, ndc.z = n/w = n/distance
//   (VR standard: no far-plane pop, and the fp32 depth budget concentrates
//   at contact range where headset occlusion matters).

#include <cmath>

#include "ve/core/math/base.h"
#include "ve/core/math/mat4.h"

namespace ve::core::math {

enum class depth_range { standard_0_to_1, reversed_1_to_0, reversed_inf_far };

namespace detail {
template <typename T>
constexpr std::array<T, 4> proj_z_row(T n, T f, depth_range dr) {
    switch (dr) {
    case depth_range::standard_0_to_1:
        return {T(0), T(0), -f / (f - n), -f * n / (f - n)};
    case depth_range::reversed_1_to_0:
        return {T(0), T(0), n / (f - n), f * n / (f - n)};
    case depth_range::reversed_inf_far:
        return {T(0), T(0), T(0), n};   // ndc.z = n / distance
    }
    return {T(0), T(0), T(0), T(1)};
}
} // namespace detail

// Off-axis asymmetric perspective (both eyes, distortion fit, window-region
// renders). l < r, b < t strictly; n, f > 0 with f > n (f may be inf only in
// reversed_inf_far).
template <typename T>
mat4<T> perspective_offaxis(T n, T f, T l, T r, T b, T t,
                            depth_range dr = depth_range::standard_0_to_1) {
    const auto zrow = detail::proj_z_row(n, f, dr);
    return mat4<T>{std::array<T, 16>{
        T(2) / (r - l),  T(0),                 zrow[0],  T(0),
        T(0),            -T(2) / (t - b),      zrow[1],  T(0),
        (r + l) / (r - l), -(t + b) / (t - b), zrow[2],  T(-1),
        T(0),            T(0),                 zrow[3],  T(0)}};
}

// Symmetric convenience wrapper — implemented THROUGH the off-axis builder
// (single code path, off-axis can never be "the later refactor").
// fov_y_rad = vertical field of view; aspect = width/height.
template <typename T>
mat4<T> perspective(T fov_y_rad, T aspect, T n, T f,
                    depth_range dr = depth_range::standard_0_to_1) {
    const T t = std::tan(fov_y_rad / T(2));
    const T r = t * aspect;
    return perspective_offaxis(n, f, -r, r, -t, t, dr);
}

// Orthographic: UI/minimap passes, and any 2D-style pass. Same Vulkan NDC.
template <typename T>
mat4<T> ortho(T l, T r, T b, T t, T n, T f,
              depth_range dr = depth_range::standard_0_to_1) {
    // w = 1 here; linear maps only. z: view -n -> standard 0 / reversed 1.
    std::array<T, 4> zrow{};
    switch (dr) {
    case depth_range::standard_0_to_1:
        zrow = {T(0), T(0), T(1) / (n - f), n / (n - f)};  // clip.z = (z+n)/(n-f): z=-n -> 0, z=-f -> 1
        break;
    case depth_range::reversed_1_to_0:
        zrow = {T(0), T(0), T(1) / (f - n), f / (f - n)};
        break;
    case depth_range::reversed_inf_far:
        // no infinite ortho; clamp far instead of inventing meaning
        zrow = {T(0), T(0), T(1) / (f - n), f / (f - n)};
        break;
    }
    return mat4<T>{std::array<T, 16>{
        T(2) / (r - l),  T(0),                zrow[0], T(0),
        T(0),            -T(2) / (t - b),     zrow[1], T(0),
        -(r + l) / (r - l), -(t + b) / (t - b), zrow[2], T(0),
        T(0),            T(0),                zrow[3], T(1)}};
}

} // namespace ve::core::math
