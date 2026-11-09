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
    if (step_divisor > kGhost_preview_max_step_divisor)
        step_divisor = kGhost_preview_max_step_divisor;  // alias refusal, header comment
    // Command-index count, not command count: step k reads
    // log[first_cmd + k*step_divisor], so the log end bounds k by
    // ceil(remaining/D) — bounding by `remaining` alone reads out of
    // bounds when a window starts near the end of a short log (4.11 caught
    // this via a final 912-tick window at stride 4).
    const std::size_t cmd_stride_count =
        (log.size() - first_cmd + step_divisor - 1) / step_divisor;
    const std::size_t steps =
        std::min({horizon_ticks / step_divisor, cmd_stride_count, kGhost_preview_max_steps});
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
