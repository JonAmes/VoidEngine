#pragma once
// Phase 7 task 7.8 (PLAN contacts bullet, #47a) — landed-body contacts are
// PENALTY-based: overlap produces a spring-damper push-out + Coulomb-capped
// tangential friction, evaluated at the body's moving/spinning pose at the
// current tick (#41). No contact solver, no stored contacts, no impulses —
// deterministic by construction at the fixed tick (#47a). Gear "squash" is
// simply the derived readout `depth_m` of each authored contact point;
// NOTHING breaks here — gear failure is never a contact concern (arcade,
// per checklist 7.8).
//
// The surface is the #29b analytic sampler (7.2/7.3): height/normal ride the
// body's LOCAL frame, the spin orientation (7.1) converts once, and the
// stamp set spins with the body for free. A pad authored by 7.3 is flat to
// the bit, so pad-rest pins are exact (see notes 7.3).

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/terrain.h"

namespace ve::core::sim {

// Hull-level penalty tuning (SI, authored per ship — arcade numbers; the
// integrator's fixed tick sets the stability envelope, k/c stay far below
// the tick-rate limit so no substeps are ever needed).
struct contact_params {
    double k_pushout_N_m = 0.0;   // penalty spring, > 0
    double c_normal_N_s_m = 0.0;  // normal damping, >= 0
    double friction_mu = 0.0;     // Coulomb cap coefficient, >= 0
    double c_tangent_N_s_m = 0.0; // tangential damping (static-friction feel
                                  // near rest), >= 0
};
bool contact_params_ok(const contact_params& p);

// The analytic surface of one body AS OF the current tick: rail pose + spin
// sampled by the caller (rails stay position/velocity-only + analytic
// orientation; this struct just carries the snapshot, pure-in/pure-out).
struct surface_world {
    const body_def* body = nullptr;         // surface_radius
    const terrain_def* terrain = nullptr;   // relief params (#29a); null = smooth sphere
    std::span<const terrain_stamp> stamps;  // authored POIs/pads (7.3)
    math::dvec3 center{};                   // world, rail position at tick t
    math::dvec3 center_vel{};               // world, rail velocity at tick t
    math::dquat orient{};                   // body-local -> world, 7.1 at tick t
    math::dvec3 spin_omega{};               // rad/s, world (0 = no spin)
};

// One authored contact point (gear foot / hull probe), hull-local offset.
// The squash readout for that gear IS contact_hit::depth_m — no extra state.
struct contact_hit {
    double depth_m = 0.0;         // > 0: penetration depth (gear squash)
    math::dvec3 normal_world{};   // surface normal (out of the body)
    math::dvec3 pos_world{};      // gear foot world position
    math::dvec3 rel_vel{};        // foot velocity minus ground velocity
};

// Contact probe: evaluates the gear foot (ship.att maps the local offset to
// world) against the surface. std::nullopt = NO contact (foot above the
// surface) and on degenerate input (never throws; the caller's state gates
// via state_ok semantics). The surface view is built exactly like every
// other caller: dir_local = orient * (foot - center), so spin, orbit
// translation and stamps all ride for free.
std::optional<contact_hit> contact_vs_surface(const surface_world& surf,
                                              math::dvec3 foot_local_offset,
                                              const rigid_state& ship);

// Penalty force (world) for one hit: F = max(0, k*depth - c_n*v_n) * n
//   + friction, where friction opposes rel tangential velocity with
// magnitude min(mu*F_n, c_t*|v_t|) — Coulomb during slide, linear near
// rest (static-friction feel with zero jitter at rest). Contacts never
// pull (F_n clamped at 0), so separated stacks add nothing.
math::dvec3 penalty_force(const contact_hit& hit, const contact_params& p);

} // namespace ve::core::sim
