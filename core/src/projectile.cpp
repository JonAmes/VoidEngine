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

std::optional<rocket> launch_rocket(const rigid_state& ship, math::dvec3 aim_dir,
                                    double launch_m_s, tick_t boost_ticks,
                                    double boost_accel, double dv_budget,
                                    tick_t ttl_ticks, tick_t now, std::int32_t owner)
{
    const bool finite = state_ok(ship) && std::isfinite(launch_m_s) && std::isfinite(boost_accel) &&
                        std::isfinite(dv_budget) && std::isfinite(aim_dir.x) &&
                        std::isfinite(aim_dir.y) && std::isfinite(aim_dir.z);
    if (!finite || !(launch_m_s >= 0.0) || !(boost_accel >= 0.0) || !(dv_budget >= 0.0) ||
        boost_ticks < 0 || ttl_ticks < 0)
        return std::nullopt;
    if (!math::approximately_equal(math::length2(aim_dir), 1.0, 1e-6))  // unit aim law (9.8)
        return std::nullopt;

    rocket r;
    r.state.pos = ship.pos;
    r.state.vel = ship.vel + aim_dir * launch_m_s;  // inherits launch-ship drift
    r.heading = aim_dir;
    r.born = now;
    r.ttl_ticks = ttl_ticks;
    r.boost_ticks = boost_ticks;
    r.boost_accel = boost_accel;
    r.dv_remaining = dv_budget;
    r.owner = owner;
    return r;
}

projectile_step_result rocket_step(
    const rail_world& w, rocket& r, tick_t command_tick, std::optional<math::dvec3> steer_aim,
    std::span<const comp_bounds> targets, math::dvec3 target_vel)
{
    projectile_step_result res;
    const tick_t end = command_tick + 1;
    if (end - r.born > r.ttl_ticks) {
        res.life = projectile_life::expired;  // terminal, idempotent (9.8 rule)
        return res;
    }
    const double dt = tick_to_seconds(1);  // boost drain is tick-exact: single tick only

    // Boost window [born, born+boost_ticks): steering authority exists ONLY
    // here; post-boost steps ignore steer -> the dart is a 9.8 round (drift).
    double a_boost = 0.0;
    if (command_tick >= r.born && command_tick < r.born + r.boost_ticks) {
        if (steer_aim && math::approximately_equal(math::length2(*steer_aim), 1.0, 1e-6))
            r.heading = *steer_aim;
        // #19e reservoir drain, reduced: applied impulse is min(desired,
        // remaining) — the min IS the floor (a dry dart coasts, the budget
        // never goes negative, the drained amount is a pure function of
        // what was applied).
        const double want = r.boost_accel * dt;
        const double applied = want < r.dv_remaining ? want : r.dv_remaining;
        r.dv_remaining -= applied;
        a_boost = applied / dt;  // == boost_accel while the reservoir lasts
    }

    const tick_command k_no_cmd{};
    const rigid_params p{};
    math::dvec3 accel_world, torque_body;
    assemble_tick_inputs(w, k_no_cmd, command_tick, p, r.state, accel_world, torque_body);
    accel_world = accel_world + r.heading * a_boost;  // wells + boost, one integration

    const math::dvec3 p0 = r.state.pos;
    integrate_step_dt(r.state, p, accel_world, torque_body, dt);

    std::optional<projectile_hit> best;
    for (const auto& c : targets) {
        const auto hit = sweep_point_vs_obb(p0, r.state.vel, dt, c.bounds, target_vel);
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
