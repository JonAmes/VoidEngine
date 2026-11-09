#include "ve/core/sim/preview.h"

#include <algorithm>

#include "ve/core/math/vec.h"

namespace ve::core::sim {

using ve::core::math::dvec3;

std::vector<rigid_state> ghost_preview(const rail_world& w, const ReplayLog& log,
                                       std::size_t first_cmd, tick_t from_tick,
                                       const rigid_state& start, const rigid_params& p,
                                       std::size_t horizon_ticks, std::size_t step_divisor) {
    if (step_divisor == 0 || first_cmd >= log.size())
        return {};
    const std::size_t steps =
        std::min({horizon_ticks / step_divisor, log.size() - first_cmd, kGhost_preview_max_steps});
    std::vector<rigid_state> out;
    out.reserve(steps);
    rigid_state s = start;  // the ghost is a COPY; caller's state untouched
    // Coarse dt: computed multiple of the canonical tick (header rule).
    const double dt = static_cast<double>(step_divisor) * kSim_tick_dt;
    for (std::size_t k = 0; k < steps; ++k) {
        const std::size_t cmd_i = first_cmd + k * step_divisor;
        const tick_t tick = from_tick + static_cast<tick_t>(k * step_divisor);
        // Substep reduction samples the COMMAND at the same reduced rate
        // as the physics — HUD honesty: the ghost shows "if I keep holding
        // what the log says at these moments", not a re-optimized pilot.
        dvec3 accel_world, torque;
        assemble_tick_inputs(w, log.at(cmd_i), tick, p, s, accel_world, torque);
        integrate_step_dt(s, p, accel_world, torque, dt);
        out.push_back(s);
    }
    return out;
}

} // namespace ve::core::sim
