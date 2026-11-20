#pragma once
// Phase 9 task 9.10 — the gun firing cycle: per-gun cooldown from
// weapon.rof, spread cone, heat accumulate/dissipate through the stats
// spine, and an overheated gun refusing to fire (PLAN #19/#21: "damage/
// range/rof/spread/heat are all attributes" — the firing gate reads them
// LIVE from the pipeline every attempt, so any modifier stack rides free).
//
// Laws (each pinned by test gun_rof_ticks / gun_heat_cycle):
//   COOLDOWN: interval_ticks = llround(kSim_tick_hz / rof); a shot is
//     allowed when no prior shot or now - last_shot >= interval. rof
//     missing/<=0/non-finite = no rate of fire = NEVER fires (fail-SAFE:
//     a gun nobody configured cannot spray — opposite of the reservoir's
//     fail-open flows because firing is an act, not a flow).
//   HEAT: one #19e Reservoir per gun, UNBOUNDED above — heat past the
//     limit is the overheating signal; clamping it would erase the
//     overshoot that keeps the gun refused while it cools. Dissipation is
//     the leak attr (J/s) through the reservoir integrator (gun_tick once
//     per fixed tick); a shot pushes the per-shot heat attr (J) through
//     the set_value door. The overheat limit is the band threshold
//     (at_or_above) throwing its tag — same hysteresis-free machinery as
//     propellant.empty (dark-not-dead): other systems (AI, presentation,
//     9.11) read the TAG; the firing gate refuses while
//     heat.value() >= capacity (identical condition, checked live).
//   SPREAD: a seeded stream keyed on (world seed, gun component id, fired
//     SHOT count) — the #18d/#20d/#31b pattern; the fired direction is
//     uniform inside the cone of half-angle spread (rad). Keyed on the
//     SHOT count, not the tick: refused attempts consume nothing, and the
//     same shot number always deflects identically. spread missing/<=0/
//     non-finite = exact aim. A refusal never changes any state (no
//     heat, no cooldown stamp, no stream advance).
//
// Opaque keys (reservoir.h doctrine): the core never NAMES game attr keys
// or band tags — the caller hands the keys in once at gun_configure; game
// glue supplies the ratified weapon.* registry strings.
//
// gun_slot is not relocatable-by-copy: the reservoir's TagSet pointer aims
// at its own member. Store in place (pointer/unique_ptr containers).
#include <cstdint>
#include <optional>
#include <string>

#include "ve/core/math/vec.h"
#include "ve/core/sim/reservoir.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/tags.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// Which live attrs gate this gun ("" = attribute absent).
struct gun_attrs {
    std::string rof;            // 1/s        — cooldown (fail-safe: missing = never fires)
    std::string spread;         // rad        — cone half-angle (missing/<=0 = exact aim)
    std::string heat_limit;     // J          — overheat band threshold (missing/<=0 = never overheats)
    std::string heat_per_shot;  // J          — heat pushed per fired shot
    std::string heat_leak;      // J/s        — dissipation (reservoir leak attr)
    std::string overheat_tag;   // thrown by the at_or_above band
};

class Occupancy;  // 5.11 occupancy view (gate query only; never stored)

struct gun_slot {
    std::int32_t component = -1;      // gun component id: rng stream key
    std::string gunner_seat;          // 9.19 (#18a): empty = fixed mount, pilot-
                                      // operated via the targeting computer (#12);
                                      // non-empty = crew-gated turret: the seat
                                      // instance id that must be manned+non-dark
    TagSet tags;                      // overheat tag lives here
    Reservoir heat{0.0, {}, &tags};   // unbounded; leak bound at configure
    tick_t last_shot = -1;            // -1 = never fired (first shot is free)
    std::uint64_t shots = 0;          // fired-shot counter: stream key
    StatsPipeline* pipe = nullptr;    // set by gun_configure
    gun_attrs attrs;

    gun_slot() = default;
    gun_slot(const gun_slot&) = delete;             // tags pointer self-reference
    gun_slot& operator=(const gun_slot&) = delete;
};

// Wire this gun: bind the heat reservoir's leak attr to the pipeline and
// arm the overheat band at heat_limit (missing/<=0/non-finite = NO band —
// never overheats). Idempotent; keeps current heat.
void gun_configure(gun_slot& gun, StatsPipeline& pipe, gun_attrs attrs);

// One fixed sim tick of dissipation (leak attr -> reservoir integrator +
// band re-evaluation).
void gun_tick(gun_slot& gun);

struct fire_result {
    math::dvec3 aim{};         // the ACTUAL (spread-deflected) unit direction
    tick_t at = 0;
    double heat_after = 0.0;   // gun heat right after the shot landed
};

// One firing attempt at command tick `now` along unit `aim` (unit-aim law
// as 9.7-9.9: non-finite or non-unit aim is REFUSED, state untouched).
// `occ`: occupancy view for crew-gated guns (9.19). A gated gun with no
// view REFUSES (fail-safe). Refusal consumes nothing — the shot stream is
// untouched, so the manning gate never shifts spread draws.
std::optional<fire_result> gun_try_fire(gun_slot& gun, math::dvec3 aim,
                                        tick_t now, std::uint64_t world_seed,
                                        const Occupancy* occ = nullptr);

}  // namespace ve::core::sim
