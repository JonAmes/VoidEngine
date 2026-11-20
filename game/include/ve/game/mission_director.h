#pragma once
// Phase 9 task 9.20 — runtime mission director, game-side (#20d/#25b/c).
//
// The director instantiates AUTHORED templates (bounty, patrol, escort,
// delivery, interdiction) from AGGREGATE signals only — reputation band
// tags (#20c), market scarcity bands (#20b), faction power bands (#25c),
// activity counters — plus a seeded RNG stream keyed on (world seed, tick):
// the exact #18d pattern. No stored stream position anywhere: replay
// recomputes the identical mission sequence from (seed, tick, signals)
// alone (determinism rule #5). Band tags arrive already reduced by the
// reservoir threshold machinery — the director reads tags, never numbers
// ("everything reads tags, never the number", #25a).
//
// Target selection is the #25b seam: an NPC engages a target iff a stance
// tag (authored faction-vs-faction) OR a reputation tag (#20c threshold
// flip) marks that target's owner hostile. Ordered by id — never a set.
//
// Game-side (AGENTS rule 1: core never names missions/bounties/factions).
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/time.h"

namespace ve::game {

// One authored template (slice scale: the five #20d kinds are just ids).
struct mission_template {
    std::string id;                        // e.g. "bounty.hostile_raider"
    std::vector<std::string> require_tags; // ALL must be live signals to be a
                                           // candidate (empty = always live)
    std::uint64_t weight = 1;              // pick weight; 0 = dormant
    std::vector<std::string> regions;      // ordered candidate spawn regions
};

// Aggregate signal snapshot for one tick: band tags + reduced counters,
// assembled by the caller from faction StatsSets, market reservoirs, and
// activity tallies. The director never reaches into those systems.
struct director_signals {
    std::vector<std::string> tags;         // e.g. "faction.pressure.high",
                                           // "market.scarce.fuel", "faction.hostile"
};

// A spawned mission: a tag/effect state machine's birth certificate (#20d).
struct mission_offer {
    std::uint64_t id;                      // deterministic instance id
    std::string template_id;
    std::string region;
    core::sim::tick_t offered_tick;
};

// Per-tick authoring knobs (from the map def; the slice keeps them global).
struct director_law {
    double spawn_density = 0.01;           // P(offer attempt) per tick, [0,1]
    std::int64_t min_gap_ticks = 600;      // global cooldown between offers
};

// One director tick. Pure function of (templates, signals, tick, world seed)
// — keyed streams splitmix64(seed ^ (tick*K2 + K3)) then chained per roll
// (attempt gate, weighted template pick, region pick, instance id), so two
// calls at the same tick with the same inputs return the identical offer.
// `last_offer_tick` (nullopt = none yet) carries the cooldown; the caller
// threads it as sim state. Returns nullopt for "no offer this tick".
[[nodiscard]] std::optional<mission_offer> director_tick(
    const std::vector<mission_template>& templates,
    const director_signals& signals,
    const director_law& law,
    core::sim::tick_t tick,
    std::uint64_t world_seed,
    std::optional<core::sim::tick_t> last_offer_tick);

// #25b: NPC target selection, owner x stance x reputation. `stance_tags`
// are the actor faction's AUTHORED stances ("stance.hostile.merchant");
// `rep_tags` the actor-side reputation band tags from #20c flips
// ("faction.hostile.merchant"). A candidate is engaged iff its owner (not
// the actor's own faction) is hostile under EITHER — stance OR reputation.
// Returns candidate ids in ascending order (ordered container, rule #5).
struct target_candidate {
    std::uint32_t id;
    std::string owner;                     // "merchant", "hostile", ...
};
[[nodiscard]] std::vector<std::uint32_t> select_targets(
    std::string_view own_faction,
    const std::vector<std::string>& stance_tags,
    const std::vector<std::string>& rep_tags,
    std::span<const target_candidate> candidates);

}  // namespace ve::game
