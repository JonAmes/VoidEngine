#include "ve/core/sim/swept.h"

#include <cmath>
#include <limits>

namespace ve::core::sim {
namespace {

bool all_finite(math::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Shared implementation; `expand` inflates the half-extents (capsule radius).
// Slab-clip the time interval [0, dt] against the box in the box-local
// frame: for each local axis the segment must satisfy |x0 + v*t| <= h,
// giving an entry time (ta) and exit time (tb) per axis; the hit is the
// LAST entry, valid iff it precedes the FIRST exit.
std::optional<swept_hit> sweep_impl(
    math::dvec3 p0, math::dvec3 vel, double dt, double expand,
    const obb& box, math::dvec3 target_vel)
{
    if (!(dt >= 0.0)) return std::nullopt;  // also rejects NaN dt
    if (!(expand >= 0.0)) return std::nullopt;
    if (!all_finite(p0) || !all_finite(vel) || !all_finite(target_vel) ||
        !all_finite(box.center) || !all_finite(box.half_extents))
        return std::nullopt;
    if (box.half_extents.x < 0.0 || box.half_extents.y < 0.0 || box.half_extents.z < 0.0)
        return std::nullopt;
    // rotate() assumes a unit attitude; authored OBBs must satisfy this
    // (same tolerance shape as invert_unit, tightened — rotate does not
    // renormalize, so a non-unit orient would scale the reported normal).
    if (!math::approximately_equal(math::length2(box.orient), 1.0, 1e-6))
        return std::nullopt;

    const math::dquat qc = math::conjugate(box.orient);  // exact, no sqrt
    const math::dvec3 x0 = math::rotate(qc, p0 - box.center);
    const math::dvec3 v = math::rotate(qc, vel - target_vel);
    const math::dvec3 h{
        box.half_extents.x + expand,
        box.half_extents.y + expand,
        box.half_extents.z + expand,
    };

    double t0 = 0.0, t1 = dt;
    int entry_axis = -1;   // slab whose entry clipped t0 (hit plane)
    int exit_axis = -1;    // slab with the earliest exit (start-inside case)
    double exit_best = std::numeric_limits<double>::infinity();  // uncapped
    for (int i = 0; i < 3; ++i) {
        const double xi = i == 0 ? x0.x : (i == 1 ? x0.y : x0.z);
        const double vi = i == 0 ? v.x : (i == 1 ? v.y : v.z);
        const double hi = i == 0 ? h.x : (i == 1 ? h.y : h.z);
        if (vi == 0.0) {
            if (xi < -hi || xi > hi) return std::nullopt;  // parallel outside
            continue;
        }
        double ta = (-hi - xi) / vi;
        double tb = (hi - xi) / vi;
        if (ta > tb) std::swap(ta, tb);  // vi < 0: high plane crossed first
        if (ta > t0) { t0 = ta; entry_axis = i; }
        if (tb < exit_best) { exit_best = tb; exit_axis = i; }
        if (tb < t1) t1 = tb;
        if (t0 > t1) return std::nullopt;  // slab window closed before entry
    }
    if (t0 > dt) return std::nullopt;  // first entry after the tick

    // Outward normal (local) of the plane hit at t0. (dvec3 has no
    // subscript — axis selection via helper.)
    const auto vi_of = [&](int axis) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); };
    math::dvec3 n_local{0.0, 0.0, 0.0};
    const auto set_axis = [&](int axis, double val) {
        if (axis == 0) n_local.x = val; else if (axis == 1) n_local.y = val; else n_local.z = val;
    };
    if (entry_axis >= 0) {
        set_axis(entry_axis, vi_of(entry_axis) > 0.0 ? -1.0 : 1.0);  // face back at the sweep
    } else {
        // Started (or was clipped to be) inside the slab window: no entry
        // plane crossed. With zero relative velocity there is no sweep
        // event; otherwise report t=0 with the exit-plane normal negated
        // (push-out direction toward the side the sweep entered through).
        if (exit_axis < 0) return std::nullopt;
        set_axis(exit_axis, vi_of(exit_axis) > 0.0 ? -1.0 : 1.0);  // -(exit normal)
    }

    swept_hit hit{};
    hit.t = t0;
    hit.pos = p0 + vel * t0;
    hit.normal = math::rotate(box.orient, n_local);
    return hit;
}

} // namespace

std::optional<swept_hit> sweep_point_vs_obb(
    math::dvec3 p0, math::dvec3 vel, double dt,
    const obb& box, math::dvec3 target_vel)
{
    return sweep_impl(p0, vel, dt, 0.0, box, target_vel);
}

std::optional<swept_hit> sweep_capsule_vs_obb(
    math::dvec3 p0, math::dvec3 vel, double dt, double radius,
    const obb& box, math::dvec3 target_vel)
{
    if (!std::isfinite(radius)) return std::nullopt;
    return sweep_impl(p0, vel, dt, radius, box, target_vel);
}

} // namespace ve::core::sim
