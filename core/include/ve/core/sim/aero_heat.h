#pragma once

// Phase 7 task 7.7 — the re-entry heat NUMBER (PLAN aero bullet: "a budget,
// not a sim"; #41: re-entry = fast flight + this number).
//
// Heat is a scalar fp64 budget integrated per fixed tick from the aero heat
// flux. The flux is a pure function of the SAME (rho, |v_rel|) the 7.6
// aero_forces already computed, on the classic convective scaling:
//
//     flux         = absorb_coeff * dyn_q * |v_rel|     (= 1/2 * absorb * rho * v^3)
//     heat(t+1)    = heat(t) + flux * kSim_dt
//
// No cooling model: survivability IS the entry profile — the v^3 term makes
// steep/fast entries bake while shallow ones barely register, and loitering
// in dense air still accrues (the honest budget behaviour). Hand-computable
// end to end like all of Phase 7 aero.
//
// The TOLERANCE deliberately does not live here: it is the
// `heat.tolerance` attr (game registry 7.5), so per-hull and per-modifier
// variation rides the Phase 6 stats pipeline for free. Over-tolerance
// damage is damage-as-data: the caller posts a negative flat_add modifier
// (6.x apply_effect pattern) whenever this budget exceeds the derived
// tolerance. Core just returns the flux and integrates the budget.
#include "ve/core/sim/aero.h"

namespace ve::core::sim {

// Heat flux for one tick's aero state. Non-finite inputs or a negative
// absorb_coeff give 0 (a body either authored absorption or it cooks in
// silence — never NaN).
[[nodiscard]] double aero_heat_flux(const aero_result& r, double absorb_coeff);

// One budget tick: heat += flux * kSim_dt. A non-finite flux or a step that
// would overflow to non-finite is refused — the budget never poisons.
void heat_budget_tick(double& heat, double flux);

}  // namespace ve::core::sim
