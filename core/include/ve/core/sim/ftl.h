#pragma once
// Phase 8 task 8.12 — FTL warp speed multiplier (decision #22b/#22d): a
// ramped velocity multiplier inside the SAME fixed-tick integrator — no
// second space layer, no hyperspace, no gates. Core holds the mechanics
// (a ramped-multiplier state machine + a "burning" drain gate); the
// ftl.* attr names are game-side glue — it aggregates ftl.speed_mult /
// ftl.spool_s into an ftl_drive each tick and drains ftl.flow through
// the #19e reservoir (a gated `ftl.drain` load attr, mirroring 6.11's
// propellant glue: drain is a pure per-tick function, never stored burn
// state).
//
// The ramp is ANALYTIC and anchored, never an accumulator:
//     mult(t) = anchor_mult ± ramp_rate * tick_to_seconds(t - anchor_tick)
// tick_to_seconds divides an integer tick count by 60.0 — exact for every
// tick count (repeated kSim_tick_dt multiplication is NOT: 240 * (1/60)
// != 4.0 bitwise), so a full ramp lands its end at a hand-computable
// tick and an interrupted ramp lands mult == 1 at the exactly symmetric
// tick (the same fp64 product appears on both sides of the subtraction).
// Full-cycle ramp endpoints are BIT-exact when rate*seconds is exact in
// fp64 (test authors pick those); interrupted ramps are pinned by an
// op-order port — same doctrine as the 8.11 rail pins.
//
// Warped mult tracks the LIVE aggregate (#21b: stats are the truth): a
// dark-not-dead drive whose ftl.speed_mult is function-zeroed (or drops
// to <= 1) mid-warp collapses to idle INSTANTLY — function-zeroing is
// instant by design, a ramp would invent thrust the stack denied.
// engage() refuses invalid aggregates (speed_mult <= 1, spool_s <= 0,
// non-finite) — guards are mutation-checked in test_ftl_spool.
//
// fp64 throughout (AGENTS rule 5): mult is simulation state feeding the
// integrator's position step; determinism rides the per-tick command
// stream (tick_command::speed_mult), so replay/scrubber reproduce warp
// legs from recorded commands alone.
#include <cstdint>

#include "ve/core/sim/time.h"  // tick_t, tick_to_seconds

namespace ve::core::sim {

// Per-tick snapshot of the drive's AGGREGATED stats (game glue builds
// this from StatsPipeline::derived of ftl.speed_mult / ftl.spool_s).
struct ftl_drive {
    double speed_mult = 1.0;  // target multiplier (engage requires > 1)
    double spool_s = 0.0;     // ramp seconds, both directions (#22d: one authored spool)
};

enum class ftl_phase : std::uint8_t {
    idle,          // mult == 1, drive cold
    spooling_up,   // 1 -> speed_mult at ramp_rate
    warped,        // mult == live speed_mult
    spooling_down, // last mult -> 1 at ramp_rate (authored OR forced)
};

struct ftl_state {
    ftl_phase phase = ftl_phase::idle;
    double mult = 1.0;          // THE value feeding tick_command::speed_mult
    double ramp_rate = 0.0;     // (target - 1) / spool_s, latched at engage
    tick_t anchor_tick = 0;     // last phase transition
    double anchor_mult = 1.0;   // mult at the anchor (up: 1.0; down: live value)
};

// Finite, mult > 1, spool_s > 0 — the engage precondition and the
// warped-state survival condition (a collapsed aggregate reads invalid).
bool ftl_drive_ok(const ftl_drive& d);

// Command doors (call at a tick boundary, before that tick's ftl_step).
// engage: only from idle, only with a valid drive; latches ramp_rate.
// disengage: any engaged phase -> spooling_down from THIS tick's mult
//   (bubble-edge forced decel, 8.13, uses the same door — same machinery,
//   authored and forced are indistinguishable to the integrator).
// Both return false and change nothing when the request is invalid.
bool ftl_engage(const ftl_drive& d, ftl_state& st, tick_t now);
bool ftl_disengage(ftl_state& st, tick_t now);

// Evaluate state for sim tick `now` (call exactly once per tick, after
// command doors). Mult applies to the position step of that tick.
void ftl_step(const ftl_drive& d, ftl_state& st, tick_t now);

// Drain gate for the #19e reservoir load: true whenever the drive is
// engaged (spool ticks burn too — propellant is the budget, #22d).
bool ftl_burning(const ftl_state& st);

}  // namespace ve::core::sim
