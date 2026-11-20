#pragma once
// Phase 9 task 9.17 — targeting computer (PLAN: assist controller, "the
// output of assist is a suggestion"). Phase 4 pattern again: PURE
// functions of (state, world, tick) — nothing rides the force path, the
// computer never touches anyone's state — so assist-on replay stays
// bit-deterministic and the ghost sim needs no second physics: the
// refinement flies ROUNDS THROUGH THE LIVE SPINE (spawn_projectile +
// projectile_step with no hull), the exact 4.10 ghost doctrine.
//
// Two-stage law (PLAN: "analytic first pass, ghost sim refines near
// wells"):
//   PASS 1 (analytic, flat space): the classic lead quadratic. The round's
//     world vel is shooter_vel + aim*muzzle (9.8 inheritance), so in the
//     shooter's frame the intercept condition is |r + w*T| = m*T with
//     r = target-shooter, w = target_vel - shooter_vel:
//       (w.w - m*m) T^2 + 2(r.w) T + r.r = 0.
//     Smallest strictly-positive real root; aim = (r + w*T)/(m*T) (unit by
//     construction at the root — NOT renormalized, so the exact aim is
//     bit-reproducible, 9.16 M10 lesson). No root (shell slower than the
//     escape speed, or the target recedes too fast) = nullopt: no solution.
//   PASS 2 (ghost refine, wells): each candidate aim spawns a real round
//     stepped tick-by-tick through the same gravity everything else feels;
//     the sim's closest-approach miss vector e (target MODEL position minus
//     round position at the argmin sample) corrects the next candidate:
//     u' = normalize(r + w*T* + e). Gravity drifts the round off the flat
//     cone by e; re-aiming past it cancels it. Fixed iteration count,
//     best-of-all-candidates wins (miss never worse than the flat pass by
//     construction). NO-OP LAW: if |e| <= kTargeting_correct_eps_m the
//     correction loop STOPS — in a void the round tracks the flat model to
//     fp precision, so the analytic aim comes back BIT-IDENTICAL (mirrors
//     4.8 auto-level's void no-op; also the loop's natural convergence test).
//
// Pilot stats scale lock time and deflection quality (PLAN; the gun doctrine
// of opaque pipeline keys — game glue maps pilot stats to these attrs):
//   quality q = pipeline attr, clamped (0,1]; missing/NaN/<=0 = assist
//     REFUSES everything (fail-safe: an unauthored gunner is not a gunner).
//   lock: required_ticks = ceil(kTargeting_lock_base_s * kSim_tick_hz / q) —
//     better pilot, faster lock. Re-engage restarts the clock (target swap).
//   deflection quality: refinement iterations = floor(q * kTargeting_refine
//     _max_iters) — better pilot, deeper ghost solve. No extra aim jitter:
//     the gun's OWN 9.10 spread cone is the weapon's error term; adding an
//     assist cone would double-count it (recorded deviation-of-note).
//
// Optional rocket designation assist (9.9 darts): inside the boost window
// the dart has full steering authority, so the assist re-solves the same
// quadratic from the DART's current state every tick (shell speed = its
// current speed); 9.9's steer law snaps heading to the answer. Outside the
// window, steering input is drift (9.9) and the assist says nothing.

#include <cstdint>
#include <optional>
#include <string>

#include "ve/core/math/vec.h"
#include "ve/core/sim/projectile.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

struct deflection_inputs {
    math::dvec3 shooter_pos{}, shooter_vel{};
    math::dvec3 target_pos{}, target_vel{};  // caller's model (contact record, 9.16)
    double shell_m_s = 0.0;                  // round's OWN speed (muzzle / launch)
};

struct deflection_solution {
    math::dvec3 aim_dir{};   // unit world direction to aim/fire along
    double tof_s = 0.0;      // intercept time (pass-1 root or best sim sample)
    double range_m = 0.0;    // |target - shooter| at solve time
    double closure_m_s = 0.0; // range DECREASE rate: positive = closing
    double deflection_rad = 0.0; // angle between LOS and aim_dir
};

// Pass 1. Non-finite inputs, shell_m_s <= 0, or no positive real root ->
// nullopt. Co-located (range 0) -> nullopt (no shot to solve).
std::optional<deflection_solution> solve_deflection(const deflection_inputs& in);

// Hand-authored lock budget (Q6 doctrine: constants tuned by tests).
inline constexpr double kTargeting_lock_base_s = 1.0;       // q=1 -> 1 s lock
inline constexpr std::uint32_t kTargeting_refine_max_iters = 8;
inline constexpr std::size_t kTargeting_refine_max_horizon = 1024; // sim steps
// Below this the sim miss is fp noise around the flat path, not gravity
// drift: correction stops, analytic aim returned untouched (void no-op law).
inline constexpr double kTargeting_correct_eps_m = 1e-3;

// Pass 2. Steps up to min(horizon_ticks, kTargeting_refine_max_horizon)
// real ticks per candidate, at most min(iters, kTargeting_refine_max_iters)
// candidates INCLUDING the flat pass. Returns the candidate with the
// smallest simulated closest approach; its tof_s is the sim argmin (s),
// deflection recomputed from its aim. Never worse than `flat` (it is a
// candidate). The target model stays constant-velocity — the same model
// pass 1 solved; only the ROUND's physics is simulated.
deflection_solution refine_deflection(const deflection_inputs& in,
                                      const deflection_solution& flat,
                                      const rail_world& w, tick_t from_tick,
                                      std::size_t horizon_ticks,
                                      std::uint32_t iters);

// ---- the computer (per targeting unit; glue drives it) ----
struct targeting_computer {
    std::int32_t component = -1;     // targeting unit id (opaque; stats subject)
    StatsPipeline* pipe = nullptr;
    std::string quality_attr;        // "" = attr absent (pipeline answers absent)
    tick_t lock_begin = -1;          // -1 = never engaged
};

// Begin/restart a lock at command tick `now` (reticle on target).
void targeting_engage(targeting_computer& tc, tick_t now);

// q read + sanitize (clamp >1 to 1; missing/NaN/<=0 = refuse), then
// lock_ready = engaged && now - lock_begin >= ceil(lock_base_s*hz/q) - 1.
bool targeting_lock_ready(const targeting_computer& tc, tick_t now);

// Full two-stage solution, gated on the lock. iters = floor(q * max).
// nullopt while not locked (or no root). `flat_only` = skip pass 2 (tests,
// or wells known absent — the pass-2 no-op makes this an optimization only).
std::optional<deflection_solution> targeting_solution(
    const targeting_computer& tc, const deflection_inputs& in,
    const rail_world& w, tick_t now, std::size_t horizon_ticks);

// Rocket designation (optional leg): dart-state re-solve inside the boost
// window [born, born+boost_ticks) with shell speed = |dart vel|; aim =
// solution from the DART's pos/vel. Window closed, zero dart speed, or no
// root -> nullopt.
std::optional<deflection_solution> rocket_designation(const rocket& dart, tick_t now,
                                                      math::dvec3 target_pos, math::dvec3 target_vel);

} // namespace ve::core::sim
