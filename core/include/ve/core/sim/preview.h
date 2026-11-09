#pragma once
// Phase 4 task 4.10 — trajectory preview (ghost sim). A COPY of the ship's
// state plus the log's future commands rolled forward on the SAME physics
// (assemble_tick_inputs — gravity, thrust, assists — and integrate_step_dt)
// at reduced substep rate: one integration per step_divisor ticks with
// dt = step_divisor*kSim_tick_dt. There is deliberately NO second physics
// implementation; preview and live can only differ by integration rate, and
// the pins below bound exactly how much that costs.
//
// The ghost is HUD prediction ONLY — never sim truth, never hashed into
// replay integrity checks. Sim code still advances exclusively at the
// canonical tick (#45a); the coarse dt here is a computed rational
// multiple of it, so determinism holds: same inputs -> bit-identical
// preview across runs.

#include <cstddef>
#include <vector>

#include "ve/core/sim/replay.h"

namespace ve::core::sim {

// Hard budget on preview work (HUD must stay cheap every frame). Both the
// caller's horizon AND the log's end are clamped to this many steps.
inline constexpr std::size_t kGhost_preview_max_steps = 1024;

// Divisor cap: the coarse dt must keep the STIFFEST assist loop inside the
// integrator's stability region. The autopilot attitude loop (ap_att_kp,
// critically damped at ~10 rad/s for arcade masses) plus its flip-phase
// stall kick are hand-authored for the 60 Hz canonical tick; sampled at
// 4 ticks or coarser, omega diverges (measured: blowup by tick ~600 at
// dt=1/6, 4.11 notes). Physics-only logs are fine coarser, but the cap is
// uniform — a preview that would alias is REFUSED (clamped), not run wrong.
inline constexpr std::size_t kGhost_preview_max_step_divisor = 4;

// Roll a copy of `start` forward over the log's commands starting at
// command index `first_cmd` / tick `from_tick`. Returns one state per
// preview step (after that step); at most min(kGhost_preview_max_steps,
// horizon_ticks/step_divisor, commands left) samples. `step_divisor` >= 1;
// divisor 1 reproduces the live sim BITWISE (same codepath, same dt).
std::vector<rigid_state> ghost_preview(const rail_world& w, const ReplayLog& log,
                                       std::size_t first_cmd, tick_t from_tick,
                                       const rigid_state& start, const rigid_params& p,
                                       std::size_t horizon_ticks, std::size_t step_divisor);

} // namespace ve::core::sim
