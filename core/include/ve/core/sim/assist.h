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
inline constexpr std::uint32_t kAssist_air_brake = 1u << 2;   // 4.9
inline constexpr std::uint32_t kAssist_autopilot = 1u << 3;   // 4.9

struct assist_gains {
    double level_kp;   // N*m per rad of tilt error (cross-product form)
    double level_kd;   // N*m per (rad/s) of tilt-plane ω
    double stab_kd;    // N*m per (rad/s) of any ω
    double ab_k;          // 4.9: air-brake accel per (m/s) of frame-relative v
    double ap_k_v;        // 4.9: autopilot velocity-servo accel per (m/s) of
                          // commanded-vs-actual relative velocity
    double ap_stop_m;     // 4.9: fraction of max_accel reserved for stopping
                          // (v_allow = sqrt(2*ap_stop_m*max_accel*dist);
                          // < 1 guarantees the clamp can always brake)
    double ap_att_kp;     // 4.9: autopilot attitude N*m per rad of aim error
    double ap_att_kd;     // 4.9: autopilot attitude N*m per (rad/s)
    double max_accel;     // accel ceiling for every assist accel (the
                          // assist's thrust budget; tests keep command thrust
                          // well below it so the clamp is observable)
};

// Hand-authored arcade defaults (Q6). Sizing notes: with I ~ 2e5 kg*m^2,
// level_kp/level_kd give a ~5 rad/s, ~critically-damped level loop;
// stab_kd is a ~2 s rotation time constant; guidance/attitude loops sized in
// 4.9 notes. Tests pin the outcomes.
inline constexpr assist_gains kArcade_assist_gains{5.0e6, 2.0e6, 1.0e5,
                                                   0.3, 0.5, 0.8, 2.0e7, 4.0e6, 40.0};

// Torque (body frame) that levels body +Y against up_world = -accel/|accel|.
// accel_world is the SAME total_accel the integrator sees (pass it, do not
// re-derive — single gravity codepath). Returns exact zero when |accel| is 0.
math::dvec3 auto_level_torque(const math::dvec3& accel_world, const rigid_state& s,
                              const assist_gains& g);

// Torque (body frame) killing all rotation: tau = -stab_kd * omega_body.
math::dvec3 stabilizer_torque(const rigid_state& s, const assist_gains& g);

// ---- 4.9 translation assists. Same purity rule as above: world/tick are
// inputs (rail-sampled), so air-brake/autopilot replay identically and the
// 4.10 ghost inherits them through the shared replay_step.

// Air brake: accel collapsing frame-relative velocity (v - v_frame) to 0,
// clamped to g.max_accel. v_frame is the home well body's rail velocity
// (replay_step resolves it via dominant_well + body_velocity; the caller
// never picks a frame).
math::dvec3 air_brake_accel(const math::dvec3& vel, const math::dvec3& frame_vel,
                            const assist_gains& g);

// Autopilot guidance accel (world frame). Stopping-distance law: the
// closing speed is capped at v_allow = sqrt(2*stop_m*max_accel*dist) so the
// thrust clamp can ALWAYS brake to zero at the target (a plain position PD
// saturates during the long clamp phase and overshoots by kilometres —
// measured, 4.9 notes). Velocity servo on (target - ship) relative to the
// MOVING target frame, minus gravity (the assist flies the line; the pilot
// trusts it). Clamped to g.max_accel.
math::dvec3 autopilot_accel(const math::dvec3& pos, const math::dvec3& vel,
                            const math::dvec3& gravity, const math::dvec3& target_pos,
                            const math::dvec3& target_vel, const assist_gains& g);

// Autopilot attitude torque (body frame): drives body forward (-Z, #34)
// onto the accel command direction; full-ω damping (unlike auto-level, the
// autopilot owns yaw too). Zero-magnitude commands hold attitude.
math::dvec3 autopilot_torque(const rigid_state& s, const math::dvec3& accel_cmd,
                             const assist_gains& g);

} // namespace ve::core::sim
