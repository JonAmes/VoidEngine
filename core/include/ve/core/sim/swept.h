#pragma once

// Swept-collision primitives (#45e): continuous point/capsule vs oriented
// component bounds (OBB), TOI resolved inside one fixed tick. The Phase 8
// warp and Phase 9 ballistics dependencies: no tunneling at any speed step —
// a bullet crossing a hull completely within one tick must still report the
// entry hit. #47a penalty contacts later consume the TOI + normal here.
//
// Determinism: pure fp64 arithmetic, no wall-clock, no stored state; called
// with the poses sampled at a tick (7.1 orientation), so replay hashes ride
// it for free. Slab-clipping (ray/OBB) in the box-local frame.

#include <optional>

#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// Oriented bounds: center + half-extents along LOCAL axes + local->world
// attitude (#45e "oriented component bounds" — hull components carry these).
struct obb {
    math::dvec3 center{0.0, 0.0, 0.0};
    math::dvec3 half_extents{0.0, 0.0, 0.0};
    math::dquat orient{};
};

struct swept_hit {
    double t = 0.0;        // seconds from sweep start, in [0, dt]
    math::dvec3 pos{};     // moving point (capsule AXIS point) at TOI, world
    math::dvec3 normal{};  // box face normal pointing back at the incoming
                           // sweep (unit, world); for a start-inside hit the
                           // exit-side normal negated (see impl).
};

// Continuous sweep of the point p(tau) = p0 + vel*tau, tau in [0, dt],
// against `box` translating at target_vel (constant over the tick; its
// orientation is fixed for this call — spin is handled by pose sampling).
// Returns the ENTRY hit, or nullopt for no hit in [0, dt]. Degenerate box
// (non-finite fields, half-extents <= 0) or non-finite motion => nullopt
// (core is exception-free). Starting inside the box => hit at t = 0.
std::optional<swept_hit> sweep_point_vs_obb(
    math::dvec3 p0, math::dvec3 vel, double dt,
    const obb& box, math::dvec3 target_vel = {0.0, 0.0, 0.0});

// Capsule = swept sphere of radius r along the path. Faces/edges/corners use
// the slab expansion by r: EXACT against faces (TOI shifts by exactly r
// along the face normal); near edges/corners the L-inf expansion hits EARLY
// by at most r*(sqrt(2)-1) of path — never late, so tunneling is impossible
// (arcade-accepted conservatism, documented #45e; sharp Minkowski corners
// only matter for contact stacking, #47a layering can refine later).
std::optional<swept_hit> sweep_capsule_vs_obb(
    math::dvec3 p0, math::dvec3 vel, double dt, double radius,
    const obb& box, math::dvec3 target_vel = {0.0, 0.0, 0.0});

} // namespace ve::core::sim
