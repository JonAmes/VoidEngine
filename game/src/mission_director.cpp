// Phase 9 task 9.20 — mission director implementation (#20d/#25b/c).
#include "ve/game/mission_director.h"

#include <algorithm>
#include <cmath>

#include "ve/core/rng.h"

namespace ve::game {
namespace {

// Splitmix64 chaining constants (same mix as every sim stream — see
// ve/core/rng.h; K2/K3 are its avalanche multipliers reused as stream
// salts so (seed,tick) keys and per-roll chains can never collide).
constexpr std::uint64_t kK1 = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kK2 = 0xBF58476D1CE4E5B9ULL;
constexpr std::uint64_t kK3 = 0x94D049BB133111EBULL;

bool all_live(const std::vector<std::string>& need,
              const std::vector<std::string>& live) {
    for (const auto& t : need)
        if (std::find(live.begin(), live.end(), t) == live.end()) return false;
    return true;
}

}  // namespace

std::optional<mission_offer> director_tick(
    const std::vector<mission_template>& templates,
    const director_signals& signals,
    const director_law& law,
    const core::sim::tick_t tick,
    const std::uint64_t world_seed,
    const std::optional<core::sim::tick_t> last_offer_tick) {
    // Cooldown first: a gated tick draws nothing from the stream (the
    // stream is keyed, never advanced — but keep the gate order so the
    // hand-port of the chain matches exactly).
    if (last_offer_tick && tick - *last_offer_tick < law.min_gap_ticks)
        return std::nullopt;

    // (seed, tick) is the whole stream key (#18d pattern: tick IS the
    // stream position — the director evaluates every tick, so no stored
    // counter exists to drift).
    std::uint64_t h = core::splitmix64(
        world_seed ^ (static_cast<std::uint64_t>(tick) * kK2 + kK3));

    // Roll 1: attempt gate. density <= 0 never fires; a NaN density
    // (corrupt def) fails the comparison and never fires (fail-safe).
    const double u = core::unit01(h);
    if (!(u < law.spawn_density)) return std::nullopt;

    // Candidate set: authored order, weight>0, all require_tags live.
    std::uint64_t total = 0;
    for (const auto& t : templates)
        if (t.weight > 0 && all_live(t.require_tags, signals.tags))
            total += t.weight;  // slice-scale weights: u64 sum cannot wrap
    if (total == 0) return std::nullopt;

    // Roll 2: weighted pick, walking candidates in authored order.
    h = core::splitmix64(h ^ kK1);
    std::uint64_t sel = h % total;
    const mission_template* pick = nullptr;
    for (const auto& t : templates) {
        if (!(t.weight > 0 && all_live(t.require_tags, signals.tags))) continue;
        if (sel < t.weight) { pick = &t; break; }
        sel -= t.weight;
    }
    if (!pick) return std::nullopt;  // unreachable; fail-safe, never OOB

    // Roll 3: region pick (authored order). Empty regions = regionless job.
    std::string region;
    if (!pick->regions.empty()) {
        h = core::splitmix64(h ^ kK2);
        region = pick->regions[h % pick->regions.size()];
    }

    // Instance id: final chain value — unique per (seed,tick) key, and
    // two offers can only share a key on the same tick, which the
    // cooldown forbids.
    mission_offer offer;
    offer.id = core::splitmix64(h ^ kK3);
    offer.template_id = pick->id;
    offer.region = std::move(region);
    offer.offered_tick = tick;
    return offer;
}

std::vector<std::uint32_t> select_targets(
    const std::string_view own_faction,
    const std::vector<std::string>& stance_tags,
    const std::vector<std::string>& rep_tags,
    const std::span<const target_candidate> candidates) {
    std::vector<std::uint32_t> out;
    for (const auto& c : candidates) {
        if (c.owner == own_faction) continue;  // never shoot our own
        // #25b authored stance: "stance.hostile.<owner>"
        const auto want = "stance.hostile." + c.owner;
        if (std::find(stance_tags.begin(), stance_tags.end(), want) != stance_tags.end()) {
            out.push_back(c.id);
            continue;
        }
        // #20c reputation flip: "faction.hostile.<owner>"
        const auto rep = "faction.hostile." + c.owner;
        if (std::find(rep_tags.begin(), rep_tags.end(), rep) != rep_tags.end())
            out.push_back(c.id);
    }
    std::ranges::sort(out);  // ordered by id regardless of input order (rule #5)
    return out;
}

}  // namespace ve::game
