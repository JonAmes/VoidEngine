// Phase 7 task 7.12 — suited EVA on foot (#19d). Everything composes
// existing machinery; this TU adds only the thin composition glue.
#include "ve/core/sim/eva.h"

#include <optional>

#include "ve/core/sim/assist.h"

namespace ve::core::sim {

math::dvec3 eva_jet_force(const eva_params& p, const math::dvec3& v_actual,
                          const math::dvec3& v_desired) {
    const math::dvec3 f = p.jet_servo_k * (v_desired - v_actual);
    const double mag = math::length(f);
    // Clamp preserves direction; the thrust ceiling is authored data.
    if (mag > p.jet_max_thrust_n && mag > 0.0) {
        return f * (p.jet_max_thrust_n / mag);
    }
    return f;
}

eva_step_out eva_walk_step(rigid_state& s, const eva_params& p,
                           const math::dvec3& grav, const surface_world& ground,
                           const contact_params& cparams,
                           const math::dvec3& v_desired,
                           std::uint32_t assist_bits) {
    eva_step_out out;

    // Foot probe: point entity, "gear" offset is the origin. Inside
    // contact_vs_surface, rel_vel = foot velocity MINUS ground velocity
    // (center_vel + spin_omega x r) — the walk is frame-correct on a
    // spinning host for free (the 7.8/7.9 algebra, untouched).
    const std::optional<contact_hit> hit =
        contact_vs_surface(ground, math::dvec3{}, s);

    math::dvec3 force{};
    if (hit) {
        out.depth_m = hit->depth_m;
        out.contact_force = penalty_force(*hit, cparams);
        force = force + out.contact_force;
    }

    // Jet hover assist (the footed cousin of the ship's 4.8 assists): the
    // command carries one tick of gravity so the servo cancels free-fall —
    // pure function of (state, grav), no stored mode, same determinism
    // rule as every assist path.
    math::dvec3 target = v_desired;
    if (assist_bits & kAssist_auto_level) {
        target = v_desired + grav * kSim_tick_dt;
    }
    force = force + eva_jet_force(p, s.vel, target);

    out.accel_world = grav + force / p.body.mass;

    // Attitude assist: the ship's own auto-level/stabilizer torques (4.8),
    // fed the SAME gravity vector — "the same code that flies the ship
    // rolls the suited entity" is literal here, not a slogan.
    math::dvec3 torque{};
    if (assist_bits & kAssist_auto_level) {
        torque = torque + auto_level_torque(grav, s, kArcade_assist_gains);
    }
    if (assist_bits & kAssist_stabilizer) {
        torque = torque + stabilizer_torque(s, kArcade_assist_gains);
    }

    integrate_step(s, p.body, out.accel_world, torque);
    return out;
}

std::vector<reservoir_band> suit_air_bands() {
    return {
        {kSuit_air_low, "suit_air.low", band_dir::at_or_below},
        {kSuit_air_empty, "suit_air.empty", band_dir::at_or_below},
    };
}

} // namespace ve::core::sim
