#include "ve/core/sim/replay.h"

#include <bit>
#include <cstring>

#include "ve/core/math/quat.h"
#include "ve/core/sim/assist.h"
#include "ve/core/sim/well_blend.h"

namespace ve::core::sim {

using ve::core::math::dvec3;

namespace {
// FNV-1a 64 (Q5): primes per the published spec.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

inline void fnv_mix_byte(std::uint64_t& h, std::uint8_t b) {
    h ^= b;
    h *= kFnvPrime;
}

// Byte order fixed MSB-first so the hash is byte-order independent.
inline void fnv_mix_u64(std::uint64_t& h, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8)
        fnv_mix_byte(h, static_cast<std::uint8_t>(v >> shift));
}

inline void fnv_mix_double(std::uint64_t& h, double d) {
    fnv_mix_u64(h, std::bit_cast<std::uint64_t>(d));
}
} // namespace

std::uint64_t state_hash(tick_t tick, const rigid_state& s) {
    std::uint64_t h = kFnvOffset;
    fnv_mix_u64(h, std::bit_cast<std::uint64_t>(tick));
    const double reals[] = {s.pos.x,       s.pos.y,       s.pos.z,        s.vel.x,  s.vel.y,
                            s.vel.z,       s.att.w,       s.att.x,        s.att.y,  s.att.z,
                            s.omega_body.x, s.omega_body.y, s.omega_body.z};
    for (double d : reals)
        fnv_mix_double(h, d);
    return h;
}

std::uint64_t replay_interval_hash(const std::vector<std::uint64_t>& tick_hashes,
                                   std::size_t first, std::size_t last) {
    std::uint64_t h = kFnvOffset;
    for (std::size_t i = first; i <= last && i < tick_hashes.size(); ++i)
        fnv_mix_u64(h, tick_hashes[i]);
    return h;
}

void replay_step(const rail_world& w, const ReplayLog& log, std::size_t i, tick_t tick,
                 const rigid_params& p, rigid_state& s) {
    const tick_command& cmd = log.at(i);
    const dvec3 thrust_world = math::rotate(s.att, cmd.thrust_body);
    const dvec3 gravity = total_accel(w, s.pos, tick);
    dvec3 accel_world = gravity + thrust_world * (1.0 / p.mass);
    dvec3 torque = cmd.torque_body;
    // Assists (4.8/4.9): pure functions of (state, world, tick); gated by
    // the command's assist_mask so toggles are part of the replay record.
    if (cmd.assist_mask & kAssist_stabilizer)
        torque = torque + stabilizer_torque(s, kArcade_assist_gains);
    if (cmd.assist_mask & kAssist_auto_level)
        torque = torque + auto_level_torque(gravity, s, kArcade_assist_gains);
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
            torque = torque + autopilot_torque(s, a_cmd, kArcade_assist_gains);
        }
    }
    integrate_step(s, p, accel_world, torque);
}

} // namespace ve::core::sim
