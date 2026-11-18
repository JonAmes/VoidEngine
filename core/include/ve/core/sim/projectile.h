#pragma once

// Phase 9 task 9.8 — gun projectiles. A projectile is an ORDINARY sim entity
// on the fixed-step integrator (PLAN Phase 9): it steps through the same
// assemble_tick_inputs + integrate_step machinery ships use — gravity wells
// affect it, no privileged physics — so time compression (the LOD lattice
// coarse-steps it like any far body) and replay hashing (state is a
// rigid_state; state_hash covers it) come for free. No second physics path.
//
// Muzzle velocity INHERITS from the firing ship: world vel = ship vel +
// aim_dir * muzzle_m_s. A ship drifting INTO its target shoots short of its
// own gun's aimpoint and vice versa — "drift cuts both ways".
//
// Despawn: lifetime (ttl) or contact. Contact is the Phase 7/9.7 continuous
// kernel — the tick's integrate segment swept against the target's oriented
// component bounds (moving-target aware) — so a round that crosses a hull
// completely inside one tick (or one coarse block) still registers. Hit
// reports the struck component id + entry hit, same report shape as lasers.

#include <cstdint>
#include <optional>
#include <span>

#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/swept.h"
#include "ve/core/sim/time.h"
#include "ve/core/sim/weapon.h"  // comp_bounds candidates

namespace ve::core::sim {

struct projectile {
    rigid_state state{};         // pos/vel world fp64; att identity, omega zero (no spin)
    std::int32_t owner = -1;     // firing ship id (opaque here; IFF layers consume)
    tick_t born = 0;
    tick_t ttl_ticks = 0;        // despawn once flight ticks exceed this at a step end
};

// Spawn at the firing ship's world state. `aim_dir` must be unit
// (approx_equal, same shape as 9.7) — the barrel axis in WORLD frame.
// Non-finite inputs or non-unit aim yield nullopt (core is exception-free).
std::optional<projectile> spawn_projectile(const rigid_state& ship, math::dvec3 aim_dir,
                                           double muzzle_m_s, tick_t now,
                                           tick_t ttl_ticks, std::int32_t owner);

enum class projectile_life : std::uint8_t { alive, struck, expired };

struct projectile_hit {
    std::uint32_t component = 0;  // struck component (named, like 9.7)
    swept_hit at{};               // entry TOI within THIS step's segment
};

struct projectile_step_result {
    projectile_life life = projectile_life::alive;
    std::optional<projectile_hit> hit;
};

// Advance one step. Normal tick: dt_ticks == 1 (bit-identical to the ship
// spine). LOD-lattice coarse block: dt_ticks == divisor, command/gravity
// sampled at `command_tick` = interval start — exactly what lod.cpp does for
// far bodies, so compressed == same physics at coarser sampling (sanctioned
// integrate_step_dt use, #4.10 pattern). Contact wins over expiry on the
// same step end; multiple targets: nearest entry wins (strict-less, ordered
// container — same total order as 9.7). A step whose END exceeds born+ttl
// does not advance and returns `expired` (idempotent terminal — despawn).
projectile_step_result projectile_step(
    const rail_world& w, projectile& pr, tick_t command_tick, std::int64_t dt_ticks,
    std::span<const comp_bounds> targets, math::dvec3 target_vel = {0.0, 0.0, 0.0});

// ---- 9.9: rocket — steerable dart (boost = bounded reservoir) ----
// Boost is the #19e reservoir pattern reduced to a transient entity: a
// bounded fp64 delta-v budget drained per fixed tick by the applied impulse
// (dv_applied = min(accel*dt, remaining); a pure function of applied
// impulse, and the floor means a dry dart coasts, never thrusts
// "negative"). No bands — nobody observes a bullet. Steering exists ONLY
// inside the boost window (full authority: heading snaps to a unit steer
// aim); after boost the dart is exactly a 9.8 projectile — "steering
// input post-boost is drift". Boost acceleration adds to the well
// acceleration from the same assemble_tick_inputs, so gravity bends the
// boost arc like everything else.

struct rocket {
    rigid_state state{};              // pos/vel fp64; att identity, omega zero (dart)
    math::dvec3 heading{0.0, 0.0, 1.0}; // thrust direction (unit), steered in boost
    std::int32_t owner = -1;
    tick_t born = 0;
    tick_t ttl_ticks = 0;
    tick_t boost_ticks = 0;           // window: steps with command_tick in [born, born+boost)
    double boost_accel = 0.0;         // m/s^2 along heading
    double dv_remaining = 0.0;        // the bounded reservoir (m/s), floored at 0
};

// Inherits the launch ship's velocity exactly like a gun round (drift cuts
// both ways): vel = ship.vel + aim*launch_m_s, heading = aim.
std::optional<rocket> launch_rocket(const rigid_state& ship, math::dvec3 aim_dir,
                                    double launch_m_s, tick_t boost_ticks,
                                    double boost_accel, double dv_budget,
                                    tick_t ttl_ticks, tick_t now, std::int32_t owner);

// `steer_aim`: desired unit heading, honored ONLY on boost steps (non-unit
// steer is ignored — drift for that tick; caller error is survivable here).
// Same terminal-expiry and contact rules as projectile_step.
projectile_step_result rocket_step(
    const rail_world& w, rocket& r, tick_t command_tick,
    std::optional<math::dvec3> steer_aim,
    std::span<const comp_bounds> targets, math::dvec3 target_vel = {0.0, 0.0, 0.0});

} // namespace ve::core::sim
