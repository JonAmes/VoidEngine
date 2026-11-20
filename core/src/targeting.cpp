// Phase 9 task 9.17 — targeting computer. Laws + derivations in targeting.h.
#include "ve/core/sim/targeting.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ve/core/sim/rigid.h"

namespace ve::core::sim {
namespace {

bool finite3(const math::dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

std::optional<double> read_quality(const targeting_computer& tc) {
    if (!tc.pipe || tc.quality_attr.empty()) return std::nullopt;
    const auto q = tc.pipe->derived(tc.quality_attr);  // optional<float> (spine)
    if (!q || !std::isfinite(static_cast<double>(*q)) || !(*q > 0.0f)) return std::nullopt;  // fail-safe
    return std::min(static_cast<double>(*q), 1.0);  // >1 sanitized, never faster-than-instant
}

// Readouts common to every candidate (positions are solve-time facts).
void fill_readouts(deflection_solution& sol, const math::dvec3& r, const math::dvec3& w, double range) {
    using namespace math;
    sol.range_m = range;
    sol.closure_m_s = -math::dot(r, w) / range;  // range decrease rate: + = closing
    double c = dot(r, sol.aim_dir) / range;
    if (c > 1.0) c = 1.0;  // acos domain clamp (aim ≈ unit; fp can tip 1+eps)
    if (c < -1.0) c = -1.0;
    sol.deflection_rad = std::acos(c);
}

}  // namespace

using math::dvec3;

std::optional<deflection_solution> solve_deflection(const deflection_inputs& in) {
    if (!finite3(in.shooter_pos) || !finite3(in.shooter_vel) ||
        !finite3(in.target_pos) || !finite3(in.target_vel) ||
        !std::isfinite(in.shell_m_s) || !(in.shell_m_s > 0.0)) {
        return std::nullopt;
    }
    const dvec3 r = in.target_pos - in.shooter_pos;
    const dvec3 w = in.target_vel - in.shooter_vel;
    const double m = in.shell_m_s;
    const double C = math::dot(r, r);
    if (!(C > 0.0)) return std::nullopt;  // co-located: no shot to solve

    // (w.w - m*m) T^2 + 2(r.w) T + r.r = 0 — see header derivation.
    const double A = math::dot(w, w) - m * m;
    const double B = 2.0 * math::dot(r, w);
    double T = 0.0;
    if (A == 0.0) {
        if (B == 0.0) return std::nullopt;  // r.w = 0 AND |w| = m: never meets
        T = -C / B;                          // linear: T = -C/B
        if (!(T > 0.0)) return std::nullopt; // receding at shell speed: no shot
    } else {
        const double disc = B * B - 4.0 * A * C;
        if (!(disc >= 0.0)) return std::nullopt;  // no real root: can't reach
        const double sq = std::sqrt(disc);
        const double t1 = (-B - sq) / (2.0 * A);
        const double t2 = (-B + sq) / (2.0 * A);
        double best = 0.0;
        if (t1 > 0.0 && std::isfinite(t1)) best = t1;
        if (t2 > 0.0 && std::isfinite(t2) && !(best > 0.0 && best < t2))
            best = best > 0.0 ? std::min(best, t2) : t2;
        if (!(best > 0.0)) return std::nullopt;
        T = best;
    }

    deflection_solution sol;
    // Unit by construction at the root (|r + w*T| = m*T); NOT renormalized —
    // bit-reproducible exact aim (9.16 M10 lesson).
    sol.aim_dir = (r + w * T) * (1.0 / (m * T));
    sol.tof_s = T;
    fill_readouts(sol, r, w, std::sqrt(C));
    return sol;
}

deflection_solution refine_deflection(const deflection_inputs& in,
                                      const deflection_solution& flat,
                                      const rail_world& w, tick_t from_tick,
                                      std::size_t horizon_ticks,
                                      std::uint32_t iters) {
    const dvec3 r = in.target_pos - in.shooter_pos;
    const dvec3 wv = in.target_vel - in.shooter_vel;
    const double m = in.shell_m_s;
    const std::size_t horizon = std::min(horizon_ticks, kTargeting_refine_max_horizon);
    const std::uint32_t n_iters = std::min(iters, kTargeting_refine_max_iters);

    struct fly_result {
        double miss = std::numeric_limits<double>::infinity();
        std::size_t argmin = 0;      // ticks after from_tick
        double cpa_tau = 0.0;        // segment time of the CPA point
        dvec3 aim{};
        dvec3 round_pos{};           // sim position at argmin
        dvec3 v0{};                  // spawn world velocity at argmin's candidate
        bool ok = false;
    };
    const auto fly = [&](dvec3 aim) -> fly_result {
        fly_result fr{.aim = aim};
        rigid_state ship;
        ship.pos = in.shooter_pos;
        ship.vel = in.shooter_vel;
        auto pr = spawn_projectile(ship, aim, m, from_tick,
                                   static_cast<tick_t>(horizon ? horizon : 1), -1);
        if (!pr) return fr;  // bad aim (non-unit/NaN): infinite miss
        fr.v0 = pr->state.vel;
        // Continuous CPA per segment: semi-implicit Euler traverses each
        // tick as a STRAIGHT segment at the post-step velocity, so the
        // round/model gap over the segment is |dp + dv*tau|, minimized in
        // closed form — no sampling quantization (sampled CPA floors at
        // ~v_perp*dt/2 = metres and would mask convergence).
        dvec3 prev = pr->state.pos;
        for (std::size_t n = 1; n <= horizon; ++n) {
            const auto res = projectile_step(w, *pr, from_tick + static_cast<tick_t>(n - 1),
                                             1, std::span<const comp_bounds>{});
            const double t0 = static_cast<double>(n - 1) * kSim_tick_dt;
            const dvec3 dp = prev - (in.target_pos + in.target_vel * t0);
            const dvec3 dv = pr->state.vel - in.target_vel;
            const double dv2 = math::dot(dv, dv);
            double tau = dv2 > 0.0 ? -(math::dot(dp, dv)) / dv2 : 0.0;
            if (tau < 0.0) tau = 0.0;
            if (tau > kSim_tick_dt) tau = kSim_tick_dt;
            const dvec3 gap = dp + dv * tau;
            const double d = math::length(gap);
            if (d < fr.miss) {
                fr.miss = d; fr.argmin = n; fr.cpa_tau = tau;
                fr.round_pos = prev + pr->state.vel * tau;  // gap point on this segment
                fr.ok = true;
            }
            prev = pr->state.pos;
            if (res.life != projectile_life::alive) break;
        }
        return fr;
    };

    fly_result best = fly(flat.aim_dir);
    dvec3 aim_cur = flat.aim_dir;
    for (std::uint32_t it = 0; it < n_iters; ++it) {
        if (!best.ok) break;
        // CPA-consistent arrival time: the segment START plus tau, so the
        // drift D below is evaluated at the SAME instant as round_pos.
        const double T = static_cast<double>(best.argmin - 1) * kSim_tick_dt + best.cpa_tau;
        // Gravity drift of THIS candidate: sim position minus the straight
        // path its spawn velocity defines (exact at spawn, fp elsewhere).
        const dvec3 D = best.round_pos - (in.shooter_pos + best.v0 * T);
        if (math::length(D) <= kTargeting_correct_eps_m) break;  // void/noise: NO-OP law
        // New round ≈ old arc (drift carries over): straight_new(T) must land
        // at model(T) - D  =>  u' = (r + w*T - D)/(m*T).
        aim_cur = math::normalize((r + wv * T - D) * (1.0 / (m * T)));
        if (!finite3(aim_cur) || !(math::length(aim_cur) > 0.0)) break;
        fly_result cand = fly(aim_cur);
        if (cand.ok && cand.miss < best.miss) best = cand;  // strict: flat wins ties
    }

    deflection_solution sol = flat;
    if (best.ok) {
        sol = flat;
        sol.aim_dir = best.aim;
        sol.tof_s = static_cast<double>(best.argmin) * kSim_tick_dt;
    }
    fill_readouts(sol, r, wv, std::sqrt(math::dot(r, r)));
    return sol;
}

void targeting_engage(targeting_computer& tc, tick_t now) { tc.lock_begin = now; }

bool targeting_lock_ready(const targeting_computer& tc, tick_t now) {
    const auto q = read_quality(tc);
    if (!q || tc.lock_begin < 0) return false;
    const auto required = static_cast<tick_t>(
        std::ceil(kTargeting_lock_base_s * static_cast<double>(kSim_tick_hz) / *q));
    return now - tc.lock_begin >= required - 1;
}

std::optional<deflection_solution> targeting_solution(
    const targeting_computer& tc, const deflection_inputs& in,
    const rail_world& w, tick_t now, std::size_t horizon_ticks) {
    const auto q = read_quality(tc);
    if (!q || !targeting_lock_ready(tc, now)) return std::nullopt;
    const auto flat = solve_deflection(in);
    if (!flat) return std::nullopt;
    const auto iters = static_cast<std::uint32_t>(
        static_cast<double>(kTargeting_refine_max_iters) * *q);  // floor
    return refine_deflection(in, *flat, w, now, horizon_ticks, iters);
}

std::optional<deflection_solution> rocket_designation(const rocket& dart, tick_t now,
                                                      dvec3 target_pos, dvec3 target_vel) {
    // Boost window only: steering authority is a boost-window property (9.9);
    // outside it, aim commands are drift, so the assist says nothing.
    if (!(now >= dart.born) || !(now < dart.born + dart.boost_ticks)) return std::nullopt;
    const double speed = math::length(dart.state.vel);
    if (!(speed > 0.0)) return std::nullopt;
    deflection_inputs in;
    in.shooter_pos = dart.state.pos;
    in.shooter_vel = dart.state.vel;
    in.target_pos = target_pos;
    in.target_vel = target_vel;
    in.shell_m_s = speed;  // current dart speed; re-solved every tick -> self-correcting
    return solve_deflection(in);
}

}  // namespace ve::core::sim
