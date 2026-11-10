#include "ve/core/sim/aero_heat.h"

#include <cmath>

#include "ve/core/sim/aero.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

double aero_heat_flux(const aero_result& r, double absorb_coeff) {
    // Convective arcade scaling: q*v (1/2 absorb rho v^3). Everything here
    // is already finite by aero_forces' contract, but Tier 1 callers may
    // hand-build results, so gate rather than trust.
    if (!std::isfinite(absorb_coeff) || absorb_coeff < 0.0) return 0.0;
    if (!std::isfinite(r.dyn_q) || !std::isfinite(r.speed_rel)) return 0.0;
    if (r.dyn_q <= 0.0 || r.speed_rel <= 0.0) return 0.0;  // no flow => no heat
    const double flux = absorb_coeff * r.dyn_q * r.speed_rel;
    return std::isfinite(flux) ? flux : 0.0;
}

void heat_budget_tick(double& heat, double flux) {
    if (!std::isfinite(flux)) return;                       // refuse to poison
    const double next = heat + flux * kSim_tick_dt;         // fixed tick only
    if (!std::isfinite(next)) return;                       // overflow => keep budget
    heat = next;
}

}  // namespace ve::core::sim
