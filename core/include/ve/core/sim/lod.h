#pragma once
// Phase 8 task 8.15 — time compression + physics LOD (#22a tool, #45a
// integer tick, user decision #53). World-time compression = integer
// tick multiples: a "far" body integrates once every m ticks, each step
// advancing its sim time by exactly the elapsed tick gap since that
// body's last step — a rational multiple of the canonical tick riding
// 4.10's authorized variable-dt path (integrate_step_warp_dt), never a
// free-float dt and never skipped sim time. A divisor-1 body rides the
// same lattice at dt = kSim_tick_dt and consumes log.at(tick) exactly
// like replay_step, so a full-rate run under this scheduler is the
// reference run BIT-FOR-BIT.
//
// Near/far is observer-relative (#53a): the distance to the observer
// entity is what the game supplies per tick; lod_divisor re-evaluates
// the authored bands against it EVERY tick, reservoir_band contract
// (bands.h, 6.9/20c): hysteresis-free, registration order, first
// at_or_below match wins. The payload is an integer step divisor
// instead of a tag. LOD is what the player can see.
//
// Sampling rule (#53b): a coarse step evaluates the logged command,
// gravity, assists, and the warp multiplier once at the interval-start
// tick (the tick right after the body's last step) — never mid-interval.
// The lattice is pure integer state (divisor + last-step tick); a
// divisor drop resumes full rate on the switch tick (that step's dt
// covers the whole elapsed gap — the catch-up jump), a rise leaves the
// body's sim time behind the clock by at most divisor-1 ticks (LOD lag:
// visible, bounded, deterministic).
#include <cstdint>
#include <vector>

#include "ve/core/sim/rails.h"
#include "ve/core/sim/replay.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// #22a: world compression runs 1x-1000x. Gate everything against this.
inline constexpr std::uint64_t kLod_max_divisor = 1000;

// Authored LOD band: the divisor applies while distance-to-observer
// <= max_dist_m. Bands ascend by max_dist_m; a distance past the last
// band gets divisor 1 (unknown territory errs toward fidelity).
struct lod_band {
    double max_dist_m = 0.0;
    std::uint64_t divisor = 1;
};

// Load-time gate (NOT per tick): thresholds finite and strictly
// ascending, divisors in [1, kLod_max_divisor].
bool lod_bands_ok(const std::vector<lod_band>& bands);

// Per-tick re-evaluation, reservoir_band contract (#53a): first band in
// registration order with dist <= max_dist_m supplies the divisor;
// no match -> 1. An out-of-range divisor in an unvalidated list also
// falls back to 1 — lod_bands_ok is the authored-data gate, this is the
// runtime safety net (a 0 divisor would stall a body's sim time forever).
std::uint64_t lod_divisor(double dist_m, const std::vector<lod_band>& bands);

// Per-body lattice phase (#53b). last_step_tick = the sim tick whose
// post-step state the body holds; -1 = pre-tick-0 (initial) state.
struct lod_body {
    std::uint64_t divisor = 1;
    tick_t last_step_tick = -1;
};

// Due iff the body is at least `divisor` ticks stale against the clock.
constexpr bool lod_step_due(const lod_body& b, tick_t tick) {
    return tick - b.last_step_tick >= static_cast<tick_t>(b.divisor);
}

// The schedule of divisor changes recorded for compressed captures
// (#53c): applied AT `tick` (the tick the game's band evaluation picked
// it), strictly ascending, alongside the tick-DENSE command log — the
// log stays one command per tick; a compressed body consumes the
// interval-start command and simply never applies the others.
struct lod_transition {
    tick_t tick = 0;
    std::uint64_t divisor = 1;
};
// Schedule equality is part of the contract (#53c): determinism runs
// and scrub-vs-live comparisons compare recorded schedules directly.
inline bool operator==(const lod_transition& a, const lod_transition& b) {
    return a.tick == b.tick && a.divisor == b.divisor;
}

// Closed-form integer lattice walk (#53c, scrubber reconstruction):
// given a recorded schedule, the divisor in force at tick t (all
// transitions with tick <= t applied), that body's last-step tick as
// of t, and the number of steps applied at ticks in [0, t]. All
// arithmetic is integer; steps inside a constant-divisor interval are
// counted by division (O(|sched|), no per-tick loop).
struct lod_lattice {
    std::uint64_t divisor = 1;
    tick_t last_step_tick = -1;
    std::uint64_t steps_applied = 0;
};
lod_lattice lod_lattice_at(const std::vector<lod_transition>& sched, tick_t t);

// One lattice tick for a body: no-op (false) unless due; otherwise the
// step samples at the interval-start tick (last_step_tick + 1) — that
// tick's logged command and rail-world time — integrates with
// dt = tick_to_seconds(tick - last_step_tick) (exact elapsed gap, #45a:
// divisor/60 as a rational, never divisor*repeated-tick-mult), and
// resolves the #50 safe-bubble wall over the WHOLE coarse sweep via
// replay.cpp's shared wall helper — one wall codepath for replay_step,
// lod_replay_step, and the scrubber, bit-identical by construction.
// Returns true and stamps b.last_step_tick = tick when a step ran.
bool lod_replay_step(const rail_world& w, const ReplayLog& log, tick_t tick,
                     const rigid_params& p, rigid_state& s, lod_body& b);

}  // namespace ve::core::sim
