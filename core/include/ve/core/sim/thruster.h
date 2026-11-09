#pragma once
// Phase 5 task 5.5 — thruster force-at-point machinery (core concept-free,
// AGENTS rule 1): core never reads component type ids or attribute keys.
// The GAME side builds thruster_static records from the loadout (socket
// transform + policy axis + numbers from `thrust.force`-style attrs — that
// attr-interpretation glue is game code); core turns (state, inputs) into
// world force + body torque + a per-thruster applied-force record.
//
// "thruster.main" gimbal and "thruster.rcs" pod clusters are the SAME
// machinery: a force along a socket-frame axis at a body point. Off-CoM
// engines produce real torque automatically (r × F, r = point − CoM in
// body axes); RCS translation and torque couples fall out of summation.
//
// Determinism: inputs/outputs are index-aligned ordered vectors; iteration
// is strictly input order; the applied-force record is per-tick input for
// Phase 6's force·dt/Isp reservoir drain (the drain itself is Phase 6).
#include <cstddef>
#include <vector>

#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"

namespace ve::core::sim {

// Authored once per ship (from a Loadout; hull-local = BODY axes).
struct thruster_static {
    math::dvec3 position_local{};        // force application point, body axes
    math::dquat orientation_local{};     // mount frame (socket orientation)
    math::dvec3 axis_local{0.0, 0.0, -1.0};  // thrust direction IN the mount
                                             // frame; must be unit length
                                             // (defensively normalized)
    double max_force = 0.0;              // N at throttle 1
    double gimbal_limit_rad = 0.0;       // 0 = fixed axis; else cone half-angle
};

// Per-tick control. gimbal = (pitch, yaw) deflection ANGLE in radians about
// the mount frame's +X then +Y (fixed order — deterministic); the pair's
// magnitude is clamped into gimbal_limit_rad (attitude authority for main
// engines; RCS pods author limit 0).
struct thruster_input {
    double throttle = 0.0;               // 0..1 (over-range clamped)
    math::dvec2 gimbal{};                // rad
};

// One applied thruster this tick (index-aligned to the input vectors — the
// caller's table is the identity source; phase 6 drains by index).
struct applied_force {
    std::size_t index;
    math::dvec3 force_local;             // N, BODY axes; impulse = * kSim_tick_dt
};

// Net effects for one tick. force_world feeds integrate_step's accel after
// dividing by mass; torque_body feeds integrate_step raw. Iteration is
// input order; vectors are parallel — the shorter length wins (documented:
// callers keep them equal; the min-guard is crash-safety, not policy).
struct thruster_forces {
    math::dvec3 force_world{};           // N, world axes
    math::dvec3 torque_body{};           // N·m, body axes (about CoM)
    std::vector<applied_force> applied;  // throttle>0 && max_force>0 only
};

// Unit thrust axis after gimbal (deflection cone applied first: the
// (pitch,yaw) pair is scaled into the cone before rotation).
math::dvec3 gimbal_deflected_axis(const thruster_static& t, math::dvec2 gimbal);

thruster_forces apply_thrusters(const math::dquat& att,   // body->world (for force)
                                math::dvec3 com_local,    // CoM in body axes (5.2)
                                const std::vector<thruster_static>& thrusters,
                                const std::vector<thruster_input>& inputs);

}  // namespace ve::core::sim
