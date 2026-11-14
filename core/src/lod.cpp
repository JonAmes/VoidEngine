// Phase 8 task 8.15 — time compression + physics LOD (decision #53).
// See lod.h for the contract. This file is the whole scheduler: bands ->
// divisor, divisor -> lattice, lattice -> one shared-spine step. No game
// concept names: the observer distance arrives as a plain double from
// game glue; the payload is an integer divisor (core/, rule #1).
#include "ve/core/sim/lod.h"

#include <cmath>

namespace ve::core::sim {

using ve::core::math::dvec3;

bool lod_bands_ok(const std::vector<lod_band>& bands) {
    double prev = -1.0;  // distances are non-negative; the first threshold must clear this
    for (const lod_band& b : bands) {
        if (!std::isfinite(b.max_dist_m) || b.max_dist_m <= prev)
            return false;  // thresholds ascend strictly: band list is unambiguous
        if (b.divisor < 1 || b.divisor > kLod_max_divisor)
            return false;  // #22a: 1x-1000x; 0 would stall a body's sim time forever
        prev = b.max_dist_m;
    }
    return true;
}

std::uint64_t lod_divisor(double dist_m, const std::vector<lod_band>& bands) {
    // reservoir_band contract (6.9/20c, #53a): first registered match wins,
    // re-evaluated fresh every tick (no edges, no hysteresis).
    for (const lod_band& b : bands) {
        if (dist_m <= b.max_dist_m)
            return (b.divisor >= 1 && b.divisor <= kLod_max_divisor) ? b.divisor : 1;
    }
    return 1;  // past the last band: unknown territory errs toward fidelity
}

lod_lattice lod_lattice_at(const std::vector<lod_transition>& sched, tick_t t) {
    // Closed-form integer walk (#53c): between transitions the lattice is
    // an arithmetic progression, so steps are counted by division — no
    // per-tick loop, no float. Transitions apply AT their tick (the live
    // band evaluation recorded them for that tick), so each transition
    // closes the previous constant-divisor interval at tick-1.
    lod_lattice L{};
    tick_t cursor = 0;  // ticks in [cursor, ...] not yet accounted
    const auto advance = [&](tick_t latest) {
        if (latest < cursor)
            return;
        const tick_t d = static_cast<tick_t>(L.divisor);
        tick_t next = L.last_step_tick + d;  // earliest due tick
        if (next < cursor)                   // realign into [cursor, latest] (ceil div)
            next += ((cursor - next + d - 1) / d) * d;
        if (next <= latest) {
            const std::uint64_t n = static_cast<std::uint64_t>((latest - next) / d + 1);
            L.last_step_tick = next + static_cast<tick_t>(n - 1) * d;
            L.steps_applied += n;
        }
        cursor = latest + 1;
    };
    for (const lod_transition& tr : sched) {
        if (tr.tick > t)
            break;
        advance(tr.tick - 1);
        L.divisor = tr.divisor;
    }
    advance(t);
    return L;
}

bool lod_replay_step(const rail_world& w, const ReplayLog& log, tick_t tick,
                     const rigid_params& p, rigid_state& s, lod_body& b) {
    if (!lod_step_due(b, tick))
        return false;
    // #53b: sample ONCE at the interval-start tick — that tick's recorded
    // command (gravity/assists/warp mult all ride it), rails at that tick.
    const tick_t start = b.last_step_tick + 1;
    dvec3 accel_world, torque;
    assemble_tick_inputs(w, log.at(static_cast<std::size_t>(start)), start, p, s,
                         accel_world, torque);
    // dt = the exact elapsed tick gap (#45a: tick_to_seconds divides by 60,
    // correctly rounded; at divisor 1 this is bit-identical to replay_step:
    // same command index, same integrate_step_warp_dt(dt = kSim_tick_dt)).
    const double dt = tick_to_seconds(tick - b.last_step_tick);
    const double mult = log.at(static_cast<std::size_t>(start)).speed_mult;
    const dvec3 p0 = s.pos;
    integrate_step_warp_dt(s, p, accel_world, torque, mult, dt);
    // One wall codepath for every step shape (#50, 8.13 helper): the coarse
    // sweep resolves against bubble centres AT `tick` (the time the step
    // LANDS at — the interval-start sample is for forces, not the wall).
    resolve_warp_bubble_wall(w, p0, s, mult, dt, tick);
    b.last_step_tick = tick;
    return true;
}

}  // namespace ve::core::sim
