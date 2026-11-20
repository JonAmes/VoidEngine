// Phase 9 task 9.21 — FSM pilot tests.
// ai_fsm_transitions: scripted encounter, transitions pinned at EXACT ticks.
// ai_reputation_flip: kills/truces move reputation; the core band primitive
// (#20c "same threshold machinery as reservoirs") flips faction.hostile at
// the crossing tick, and the FSM engages/disengages only through that tag.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/bands.h"
#include "ve/core/sim/tags.h"
#include "ve/game/ai_pilot.h"

using ve::core::TagSet;
using ve::core::sim::apply_bands;
using ve::core::sim::band_dir;
using ve::core::sim::reservoir_band;
using ve::game::ai_intent_of;
using ve::game::ai_next_state;
using ve::game::ai_pilot_attrs;
using ve::game::ai_state;
using ve::game::ai_state_name;

namespace {

const ai_pilot_attrs kAttrs{5000.0, 8000.0, 0.3, 0.5};

// scripted encounter: slant to the hostile contact at tick t (nullopt = no
// contact). Approach 9000->4000 by t=100; hold 3000 through the integrity
// hit at t=200; retreat out of reach by t=300; re-approach to 2000 at t=400.
std::optional<double> scripted_slant(const int t) {
    if (t < 50) return 9000.0;
    if (t < 100) return 4000.0;   // inside engage range
    if (t < 300) return 3000.0;   // held through the withdrawal
    if (t < 350) return 12000.0;  // gone beyond disengage range
    if (t < 400) return 6000.0;   // back, but outside aggression reach
    return 2000.0;                // inside reach (5000*0.5 = 2500)
}

}  // namespace

TEST_CASE("ai_fsm_transitions") {
    // own integrity: 1.0 until t=200 (a hit lands, integrity -> 0.25),
    // repaired back to 1.0 at t=350 (safe behind friendly lines).
    auto integrity = [](const int t) { return (t < 200 || t >= 350) ? 1.0 : 0.25; };

    ai_state s = ai_state::patrol;
    std::vector<std::pair<int, const char*>> flips;
    for (int t = 0; t <= 500; ++t) {
        const auto before = s;
        s = ai_next_state(s, scripted_slant(t), true, integrity(t), kAttrs);
        if (before != s) flips.push_back({t, ai_state_name(s)});
    }
    // Hand-computed pins:
    //  t=50: slant 4000 <= 5000 -> engage
    //  t=200: integrity 0.25 < 0.30 -> withdraw (range holds at 3000)
    //  t=300: slant 12000 > 8000 -> patrol (withdraw clears when out of reach)
    //  t=350: slant 6000 > 2500 reach -> stays patrol (t=400 engages)
    //  t=400: slant 2000 <= 2500 -> engage (aggression 0.5 reach)
    //  no disengage flip t=100..199: hysteresis (3000-4000 < 8000)
    const std::vector<std::pair<int, std::string>> want = {
        {50, "engage"}, {200, "withdraw"}, {300, "patrol"}, {400, "engage"},
    };
    REQUIRE(flips.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        CHECK(flips[i].first == want[i].first);
        CHECK(std::string{flips[i].second} == want[i].second);
    }

    // Intent: weapons free only while engaged.
    CHECK(ai_intent_of(ai_state::engage).weapons_free);
    CHECK(!ai_intent_of(ai_state::withdraw).weapons_free);
    CHECK(!ai_intent_of(ai_state::disengage).weapons_free);
    CHECK(!ai_intent_of(ai_state::patrol).weapons_free);

    // Boundary semantics pinned at the exact comparison ticks:
    CHECK(ai_next_state(ai_state::patrol, 5000.0, true, 1.0, kAttrs) == ai_state::engage);   // <= acquires
    CHECK(ai_next_state(ai_state::patrol, 5000.0001, true, 1.0, kAttrs) == ai_state::patrol);
    CHECK(ai_next_state(ai_state::engage, 8000.0, true, 1.0, kAttrs) == ai_state::engage);   // <= holds
    CHECK(ai_next_state(ai_state::engage, 1.0, true, 0.3, kAttrs) == ai_state::engage);      // < only retreats
    CHECK(ai_next_state(ai_state::engage, 1.0, true, 0.29, kAttrs) == ai_state::withdraw);
    // aggression 0: disengage never re-bites; contact gone -> patrol.
    const ai_pilot_attrs meek{5000.0, 8000.0, 0.3, 0.0};
    CHECK(ai_next_state(ai_state::disengage, 10.0, true, 1.0, meek) == ai_state::patrol);
    // no contact ever resolves to non-aggression.
    CHECK(ai_next_state(ai_state::patrol, std::nullopt, true, 1.0, kAttrs) == ai_state::patrol);
    CHECK(ai_next_state(ai_state::engage, 100.0, false, 1.0, kAttrs) == ai_state::disengage);
    // evaluation order: BOTH fire on the same tick (out of reach AND at
    // retreat integrity) -> withdraw wins (pilot runs, does not re-hold).
    CHECK(ai_next_state(ai_state::engage, 9000.0, true, 0.25, kAttrs) == ai_state::withdraw);
}

TEST_CASE("ai_reputation_flip") {
    // Two kills (effects post -60 reputation each), then a truce (+130):
    // the crossing of the -100 threshold throws faction.hostile.merchant on
    // the exact tick the running total lands, via core apply_bands (#20c).
    const std::vector<reservoir_band> bands{
        {-100.0, "faction.hostile.merchant", band_dir::at_or_below},
    };
    auto rep_at = [](const int t) {
        double r = 0.0;
        if (t >= 60) r -= 60.0;    // first kill (convoy interdiction)
        if (t >= 120) r -= 60.0;   // second kill -> -120 crosses
        if (t >= 200) r += 130.0;  // truce -> +10, un-throws
        return r;
    };

    TagSet tags;
    ai_state s = ai_state::patrol;
    std::vector<std::pair<int, const char*>> flips;
    std::optional<std::string> first_hostile_tick;
    for (int t = 0; t <= 300; ++t) {
        apply_bands(rep_at(t), bands, tags);
        const bool hostile = tags.contains_exact("faction.hostile.merchant");
        if (hostile && !first_hostile_tick) first_hostile_tick = std::to_string(t);
        // contact always in range: the ONLY gate to engagement is the tag.
        const auto before = s;
        s = ai_next_state(s, 2000.0, hostile, 1.0, kAttrs);
        if (before != s) flips.push_back({t, ai_state_name(s)});
    }
    // Reputation -60 (t=60..119): NOT hostile, FSM stays patrol.
    CHECK(*first_hostile_tick == "120");
    // engage exactly at the flip tick; truce un-throws at t=200 -> disengage,
    // and with no hostile contact the FSM resumes patrol the next tick.
    const std::vector<std::pair<int, std::string>> want = {
        {120, "engage"}, {200, "disengage"}, {201, "patrol"},
    };
    REQUIRE(flips.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        CHECK(flips[i].first == want[i].first);
        CHECK(std::string{flips[i].second} == want[i].second);
    }
}
