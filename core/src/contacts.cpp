// Phase 7 task 7.8 (#47a) — penalty contacts vs the analytic surface.
// Everything here is pure per call: no stored contacts, no solver state.
// Determinism contract (#40/#5): fp64 throughout, evaluated from the tick's
// rail snapshot only — the replay-hash tests are the enforcement channel.

#include "ve/core/sim/contacts.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ve/core/math/base.h"

namespace ve::core::sim {

namespace {
bool all_finite(math::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
} // namespace

bool contact_params_ok(const contact_params& p) {
    return std::isfinite(p.k_pushout_N_m) && p.k_pushout_N_m > 0.0 &&
           std::isfinite(p.c_normal_N_s_m) && p.c_normal_N_s_m >= 0.0 &&
           std::isfinite(p.friction_mu) && p.friction_mu >= 0.0 &&
           std::isfinite(p.c_tangent_N_s_m) && p.c_tangent_N_s_m >= 0.0;
}

std::optional<contact_hit> contact_vs_surface(const surface_world& surf,
                                              math::dvec3 foot_local_offset,
                                              const rigid_state& ship) {
    if (surf.body == nullptr) return std::nullopt;
    if (!state_ok(ship)) return std::nullopt;
    if (!all_finite(foot_local_offset)) return std::nullopt;
    if (!all_finite(surf.center) || !all_finite(surf.center_vel) ||
        !all_finite(surf.spin_omega))
        return std::nullopt;
    if (!std::isfinite(surf.body->surface_radius) || surf.body->surface_radius <= 0.0)
        return std::nullopt;
    // orient rides the same unit gate as swept OBBs (7.4): exactly unit from
    // rails, but hand-built snapshots must not silently skew the surface.
    if (!math::approximately_equal(math::length2(surf.orient), 1.0, 1e-6))
        return std::nullopt;

    // Gear foot in world, and the radial frame from the body center.
    const math::dvec3 foot = ship.pos + math::rotate(ship.att, foot_local_offset);
    const math::dvec3 rel = foot - surf.center;
    const double r = math::length(rel);
    if (!(r > 0.0)) return std::nullopt;  // degenerate: no radial direction

    // Surface view rides the spin for free: local direction, stamped
    // sampler, normal rotated back (identical conversion to every other
    // sampler caller — 7.3 contract).
    const math::dvec3 dir_local = math::normalize(math::rotate(math::conjugate(surf.orient), rel));
    double h = 0.0;
    math::dvec3 n_local = dir_local;  // smooth sphere when no terrain authored
    if (surf.terrain != nullptr) {
        h = terrain_height(*surf.terrain, surf.stamps, dir_local);
        n_local = terrain_normal(*surf.terrain, surf.body->surface_radius,
                                 surf.stamps, dir_local);
    }
    const double r_surf = surf.body->surface_radius + h;
    const double depth = r_surf - r;
    if (!(depth > 0.0)) return std::nullopt;  // not touching

    // Ground velocity at the foot's radius: orbital translation + spin sweep.
    // (cross uses rel, not the exact surface point: the error is depth*|w|,
    // depth << R always in contact, so this is far below arcade tolerances
    // and keeps the eval one cross product.)
    const math::dvec3 ground_vel =
        surf.center_vel + math::cross(surf.spin_omega, rel);
    const math::dvec3 omega_world = math::rotate(ship.att, ship.omega_body);
    const math::dvec3 foot_vel = ship.vel + math::cross(omega_world, foot - ship.pos);

    contact_hit hit;
    hit.depth_m = depth;
    hit.normal_world = math::rotate(surf.orient, n_local);
    hit.pos_world = foot;
    hit.rel_vel = foot_vel - ground_vel;
    return hit;
}

math::dvec3 penalty_force(const contact_hit& hit, const contact_params& p) {
    if (!contact_params_ok(p)) return {};
    if (!(hit.depth_m > 0.0)) return {};
    if (!std::isfinite(hit.depth_m) || !all_finite(hit.normal_world) ||
        !all_finite(hit.rel_vel))
        return {};
    const math::dvec3& n = hit.normal_world;

    // Normal: spring out, damper against approach; never pull (clamp at 0).
    const double v_n = math::dot(hit.rel_vel, n);
    const double f_n = std::max(0.0, p.k_pushout_N_m * hit.depth_m -
                                     p.c_normal_N_s_m * v_n);
    // Tangential force deliberately absent in 7.8: the analytic surface
    // normal is radial (curved tangent plane), so a sustained tangential
    // cap makes a "resting" body slide along the curvature forever — the
    // body never comes to rest on a sphere, arcade or not. The capture
    // zone's hold-down (#29g) and snap-to-rest (7.9) plus pad-frame
    // pinning carry landed bodies; Coulomb friction returns with a flat
    // pad frame (on-foot verbs, 7.13). Normal-direction compliance and
    // the authored depth readout are the 7.8 deliverables. The mu and
    // c_tangent fields stay gated finite for when that lands.
    return f_n * n;
}

} // namespace ve::core::sim
