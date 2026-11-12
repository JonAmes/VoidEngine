#include "ve/core/sim/replay.h"

#include <bit>
#include <cstring>

#include "ve/core/fnv.h"
#include "ve/core/math/quat.h"
#include "ve/core/sim/assist.h"
#include "ve/core/sim/well_blend.h"

namespace ve::core::sim {

using ve::core::math::dvec3;

// FNV mixing lives in ve/core/fnv.h (shared with the 6.5 stats hashes).

std::uint64_t state_hash(tick_t tick, const rigid_state& s) {
    Fnv1a64 h;
    h.mix_u64(std::bit_cast<std::uint64_t>(tick));
    const double reals[] = {s.pos.x,       s.pos.y,       s.pos.z,        s.vel.x,  s.vel.y,
                            s.vel.z,       s.att.w,       s.att.x,        s.att.y,  s.att.z,
                            s.omega_body.x, s.omega_body.y, s.omega_body.z};
    for (double d : reals)
        h.mix_f64(d);
    return h.value();
}

std::uint64_t replay_interval_hash(const std::vector<std::uint64_t>& tick_hashes,
                                   std::size_t first, std::size_t last) {
    Fnv1a64 h;
    for (std::size_t i = first; i <= last && i < tick_hashes.size(); ++i)
        h.mix_u64(tick_hashes[i]);
    return h.value();
}

void assemble_tick_inputs(const rail_world& w, const tick_command& cmd, tick_t tick,
                          const rigid_params& p, const rigid_state& s, dvec3& accel_world,
                          dvec3& torque_body) {
    const dvec3 thrust_world = math::rotate(s.att, cmd.thrust_body);
    const dvec3 gravity = total_accel(w, s.pos, tick);
    accel_world = gravity + thrust_world * (1.0 / p.mass);
    torque_body = cmd.torque_body;
    // Assists (4.8/4.9): pure functions of (state, world, tick); gated by
    // the command's assist_mask so toggles are part of the replay record.
    if (cmd.assist_mask & kAssist_stabilizer)
        torque_body = torque_body + stabilizer_torque(s, kArcade_assist_gains);
    if (cmd.assist_mask & kAssist_auto_level)
        torque_body = torque_body + auto_level_torque(gravity, s, kArcade_assist_gains);
    if (cmd.assist_mask & kAssist_air_brake) {
        // Home-well frame = stateless dominant well's rail velocity; no
        // well -> static root frame. (Pure query, so ghost/replay agree.)
        dvec3 frame_vel(0.0, 0.0, 0.0);
        if (const auto home = dominant_well_at(w, s.pos, tick))
            if (const auto fv = w.velocity(*home, tick)) frame_vel = *fv;
        accel_world = accel_world + air_brake_accel(s.vel, frame_vel, kArcade_assist_gains);
    }
    if (cmd.assist_mask & kAssist_autopilot) {
        const auto tp = w.position(cmd.autopilot_body, tick);
        const auto tv = w.velocity(cmd.autopilot_body, tick);
        if (tp && tv) {
            const dvec3 a_cmd =
                autopilot_accel(s.pos, s.vel, gravity, *tp, *tv, kArcade_assist_gains);
            accel_world = accel_world + a_cmd;
            torque_body = torque_body + autopilot_torque(s, a_cmd, kArcade_assist_gains);
        }
    }
}

void replay_step(const rail_world& w, const ReplayLog& log, std::size_t i, tick_t tick,
                 const rigid_params& p, rigid_state& s) {
    dvec3 accel_world, torque;
    assemble_tick_inputs(w, log.at(i), tick, p, s, accel_world, torque);
    integrate_step(s, p, accel_world, torque);
}

} // namespace ve::core::sim
