// Phase 9 task 9.23 — the one separation law (#27b). Deterministic:
// pure fp64 arithmetic, hazards iterated in caller (ordered) order.
#include "ve/game/separation.h"

#include "ve/core/math/vec.h"

namespace ve::game {

core::math::dvec3 separation_accel(const core::math::dvec3& self_pos,
                                   const double self_r,
                                   const std::span<const hazard> hazards,
                                   const separation_params& p) {
    core::math::dvec3 acc{};
    for (const auto& h : hazards) {
        const core::math::dvec3 delta = self_pos - h.pos;
        const double d = core::math::length(delta);
        const double R = self_r + h.r + p.margin;  // interaction zone
        if (d <= 0.0 || d >= R) continue;  // no direction / out of zone
        const double strength = p.stiffness * (1.0 - d / R);
        acc = acc + delta * (strength / d);
    }
    return acc;
}

}  // namespace ve::game
