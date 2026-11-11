#pragma once
// Phase 7 task 7.12 — suited EVA on foot (#19d). NOT a special physics
// path: an EVA entity is a rigid body in the same sim (same integrator,
// same well blend, same contact toolkit as 7.8) plus one Phase-6 reservoir
// for suit air. The load-bearing claim this header exists to make
// testable: the SAME code that flies the ship rolls the suited entity.
//
// Pieces (all pure or reservoir-state, no new physics machinery):
//  - suit air: a per-EVA Reservoir (6.9) drained by an activity load attr;
//    low/empty bands throw tags at hand-computed ticks. Being inside a
//    pressurized section simply stops feeding the load — no separate mode.
//  - jets: authored max thrust; the translational command is an error
//    (desired - actual) velocity servo clamped to that thrust; attitude is
//    held by the SAME auto-level torque the ship's assist uses (4.8), fed
//    the SAME total_accel the integrator sees.
//  - stand: the 7.8 surface_world snapshot already carries the ground
//    velocity (center_vel + spin_omega x r) that contact rel_vel subtracts
//    — standing still on a spinning body is proven against that algebra.
//  - home well: the walker carries a home_well_tracker (hysteresis rule);
//    "down = the moon, not the giant in its sky" is proven by a
//    nested-well walk with pinned switch ticks.

#include <cstdint>
#include <vector>

#include "ve/core/math/vec.h"
#include "ve/core/sim/assist.h"
#include "ve/core/sim/contacts.h"
#include "ve/core/sim/gravity.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/reservoir.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/terrain.h"
#include "ve/core/sim/well_blend.h"

namespace ve::core::sim {

// Authored suit/jet numbers for one EVA entity (cooked data eventually;
// authored inline all the way through Phase 7, like everything else here).
struct eva_params {
    rigid_params body;             // mass/inertia of suit + entity
    double jet_max_thrust_n = 0.0; // low-thrust ceiling (N)
    double jet_servo_k = 0.0;      // N per (m/s) of velocity error
};

// Translational jet force (world frame): k * (v_desired - v_actual),
// magnitude-clamped to jet_max_thrust_n. Pure; the desired velocity is
// whatever the verb/assist layer asks for this tick.
math::dvec3 eva_jet_force(const eva_params& p, const math::dvec3& v_actual,
                          const math::dvec3& v_desired);

// One footed tick: gravity accel `grav` is the caller's 4.5 total_accel
// (single gravity codepath rule, 4.8: passed in, never re-derived here) +
// jet servo force + penalty contact force (7.8 toolkit, point "gear" at
// the entity origin; rel_vel already subtracts the spinning ground's
// velocity) -> the SAME integrate_step the ship uses. Attitude: the SAME
// assist torques as the ship (4.8), fed the SAME grav vector.
struct eva_step_out {
    math::dvec3 accel_world;    // gravity + jets + contact, world frame
    math::dvec3 contact_force;  // penalty force this tick (zero if airborne)
    double depth_m = 0.0;       // contact depth (0 if airborne)
};
eva_step_out eva_walk_step(rigid_state& s, const eva_params& p,
                           const math::dvec3& grav, const surface_world& ground,
                           const contact_params& cparams,
                           const math::dvec3& v_desired,
                           std::uint32_t assist_bits);

// Suit air: the Phase-6 band set for one EVA entity. load_attr drains while
// exposed; being inside a pressurized section simply stops feeding the
// load. Tags: "suit_air.low", "suit_air.empty" (ancestor "suit_air").
inline constexpr double kSuit_air_low = 25.0;
inline constexpr double kSuit_air_empty = 0.0;
std::vector<reservoir_band> suit_air_bands();

} // namespace ve::core::sim
