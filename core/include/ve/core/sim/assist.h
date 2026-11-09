#pragma once
// Phase 4 task 4.8 — toggleable attitude assists (Elite-style), Q6: gains are
// hand-authored constants, tuned by tests, not by a hidden tuning pass.
//
// Both assists are PURE functions of (state, gravity-at-tick) — same rule as
// the well blend (4.5): nothing stateful rides the force/torque path, so
// assist-on replay stays bit-deterministic and the 4.10 ghost sim inherits
// them for free through the shared replay_step.
//
// Authority split (mirrors Elite's controls):
//  - stabilizer bit: kills ALL body rotation (yaw included).
//  - auto-level bit: levels body +Y against local "up" (away from the home
//    well's pull) but has NO yaw authority — heading survives. The D-term
//    damps only the tilt-plane ω (ω minus its body-ŷ component); exact while
//    level, arcade-close while tilted (documented approximation).
//  - In the void (gravity exactly 0) auto-level is a bitwise no-op: no
//    reference "down" exists, so it must not invent one.

#include "ve/core/math/vec.h"
#include "ve/core/sim/rigid.h"

namespace ve::core::sim {

// assist_mask bit positions in tick_command (4.7 froze this shape).
inline constexpr std::uint32_t kAssist_stabilizer = 1u << 0;
inline constexpr std::uint32_t kAssist_auto_level = 1u << 1;

struct assist_gains {
    double level_kp;  // N*m per rad of tilt error (cross-product form)
    double level_kd;  // N*m per (rad/s) of tilt-plane ω
    double stab_kd;   // N*m per (rad/s) of any ω
};

// Hand-authored arcade defaults (Q6). Sizing notes: with I ~ 2e5 kg*m^2,
// level_kp/level_kd give a ~5 rad/s, ~critically-damped level loop;
// stab_kd is a ~2 s rotation time constant. Tests pin the outcomes.
inline constexpr assist_gains kArcade_assist_gains{5.0e6, 2.0e6, 1.0e5};

// Torque (body frame) that levels body +Y against up_world = -accel/|accel|.
// accel_world is the SAME total_accel the integrator sees (pass it, do not
// re-derive — single gravity codepath). Returns exact zero when |accel| is 0.
math::dvec3 auto_level_torque(const math::dvec3& accel_world, const rigid_state& s,
                              const assist_gains& g);

// Torque (body frame) killing all rotation: tau = -stab_kd * omega_body.
math::dvec3 stabilizer_torque(const rigid_state& s, const assist_gains& g);

} // namespace ve::core::sim
