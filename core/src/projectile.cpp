#include "ve/core/sim/projectile.h"

#include <cmath>

#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/replay.h"

namespace ve::core::sim {

std::optional<projectile> spawn_projectile(const rigid_state& ship, math::dvec3 aim_dir,
                                           double muzzle_m_s, tick_t now,
                                           tick_t ttl_ticks, std::int32_t owner)
{
    const bool finite = state_ok(ship) && std::isfinite(muzzle_m_s) &&
                        std::isfinite(aim_dir.x) && std::isfinite(aim_dir.y) &&
                        std::isfinite(aim_dir.z);
    if (!finite || !(muzzle_m_s >= 0.0) || ttl_ticks < 0) return std::nullopt;
    // Unit aim is a law (9.7 precedent): a non-unit barrel axis silently
    // scales the muzzle contribution — the gun's authored muzzle speed IS
    // the launch speed only along a unit axis.
    if (!math::approximately_equal(math::length2(aim_dir), 1.0, 1e-6))
        return std::nullopt;

    projectile pr;
    pr.state.pos = ship.pos;
    // Drift cuts both ways: the round leaves carrying the ship's velocity.
    pr.state.vel = ship.vel + aim_dir * muzzle_m_s;
    pr.born = now;
    pr.ttl_ticks = ttl_ticks;
    pr.owner = owner;
    return pr;
}

projectile_step_result projectile_step(
    const rail_world& w, projectile& pr, tick_t command_tick, std::int64_t dt_ticks,
    std::span<const comp_bounds> targets, math::dvec3 target_vel)
{
    projectile_step_result res;
    if (dt_ticks < 1) return res;  // caller error: never advance backwards/stalled
    const tick_t end = command_tick + static_cast<tick_t>(dt_ticks);
    if (end - pr.born > pr.ttl_ticks) {
        res.life = projectile_life::expired;  // terminal: despawn me
        return res;
    }

    // Ship spine, verbatim: empty command (no thrust, no assists, mult 1.0)
    // so the only acceleration is the rail world's wells at this tick.
    const tick_command k_no_cmd{};
    const rigid_params p{};  // mass/inertia irrelevant: accel is already /m, torque zero
    math::dvec3 accel_world, torque_body;
    assemble_tick_inputs(w, k_no_cmd, command_tick, p, pr.state, accel_world, torque_body);

    const math::dvec3 p0 = pr.state.pos;
    const double dt = tick_to_seconds(end - command_tick);  // 1/60 exact for one tick
    integrate_step_dt(pr.state, p, accel_world, torque_body, dt);

    // Contact: THIS step's segment (p0 -> p0 + vel_new*dt, the integrator's
    // exact grouping) swept continuously vs the target's component bounds —
    // no tunneling at any dt, coarse blocks included (#45e guarantee).
    std::optional<projectile_hit> best;
    for (const auto& c : targets) {
        const auto hit = sweep_point_vs_obb(p0, pr.state.vel, dt, c.bounds, target_vel);
        if (!hit) continue;
        if (!best || hit->t < best->at.t)
            best = projectile_hit{c.component, *hit};
    }
    if (best) {
        res.life = projectile_life::struck;
        res.hit = best;
    }
    return res;
}

} // namespace ve::core::sim
