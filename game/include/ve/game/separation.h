#pragma once
// Phase 9 task 9.23 — separation term (#27b). ONE radius-repulsion law
// carried by NPC steering AND the player autopilot assists — no
// privileged avoidance. Pure function of positions/radii/params; the
// caller integrates it identically for every ship.
#include <span>

#include "ve/core/math/vec.h"

namespace ve::game {

struct hazard {
    core::math::dvec3 pos;
    double r;  // collision radius
};

struct separation_params {
    double margin;    // extra stand-off beyond the sum of radii
    double stiffness; // m/s^2 at full overlap of the interaction zone
};

// Repulsion acceleration on `self` from all hazards: each hazard within
// R = self_r + h.r + margin pushes along the centre line with magnitude
// stiffness * (1 - d/R) — zero at the zone edge, max at touching.
// Degenerate exact-coincident pairs push nothing (no direction to pick).
[[nodiscard]] core::math::dvec3 separation_accel(
    const core::math::dvec3& self_pos, double self_r,
    std::span<const hazard> hazards, const separation_params& p);

}  // namespace ve::game
