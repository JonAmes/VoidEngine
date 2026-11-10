// Phase 6 task 6.13 — reputation attrs + faction tag flips (done-when (g)
// pt 1, PLAN #20c): reputation is an attribute on the faction's OWN
// StatsSet; player actions nudge it as instant effects (6.6: no expiry —
// they live until the source is removed); faction tags flip through the
// SAME threshold machinery as reservoirs (6.13 bands.h), re-evaluated
// every fixed tick — so flips land at hand-computed ticks, and reverse
// crossings un-throw the tags with zero hysteresis.
#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/bands.h>
#include <ve/core/sim/effects.h>
#include <ve/core/sim/modifiers.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_pipeline.h>
#include <ve/core/sim/tags.h>
#include <ve/core/sim/time.h>

namespace {

using namespace ve::core;
using namespace ve::core::sim;

modifier_desc flat(std::string_view attr, std::string_view stack, float v) {
    modifier_desc m;
    m.target_attr = std::string(attr);
    m.stack_id = std::string(stack);
    m.op = mod_op::flat_add;
    m.value = v;
    m.policy = stack_policy::stack_n;
    m.stack_cap = 64;
    return m;
}

}  // namespace

TEST_CASE("reputation_tags") {
    StatsSet st;
    ModifierStack ms;
    StatsPipeline p(st, ms);
    TagSet tags;

    st.set_base("rep", 50.0f);  // faction reputation attr, opaque key
    const std::vector<reservoir_band> bands = {
        {20.0, "faction.hostile", band_dir::at_or_below},
        {60.0, "faction.friendly", band_dir::at_or_above},
    };
    // Authoring state evaluates at tick 0, exactly like a reservoir at
    // construction: 50 sits in the dead band -> neither tag.
    apply_bands(static_cast<double>(*p.derived("rep")), bands, tags);
    CHECK_FALSE(tags.contains("faction.hostile"));
    CHECK_FALSE(tags.contains("faction.friendly"));

    std::vector<bool> hostile, friendly;
    std::vector<float> rep;
    for (std::int64_t t = 0; t < 120; ++t) {
        ms.expire_due(static_cast<tick_t>(t));
        if (t == 40) {
            // Player strike: INSTANT effect (lives until removed).
            effect_desc fx;
            fx.effect_id = "player.strike";
            fx.bundle = {flat("rep", "strike", -30.0f)};
            REQUIRE(apply_effect(ms, 10, fx) == effect_result::applied);
        }
        if (t == 70) {
            // Peace accord: another persistent nudge.
            effect_desc fx;
            fx.effect_id = "peace.accord";
            fx.bundle = {flat("rep", "accord", 45.0f)};
            REQUIRE(apply_effect(ms, 11, fx) == effect_result::applied);
        }
        if (t == 95) {
            // Strike forgiven: removing the effect source restores the attr
            // bit-exactly (6.6 instant-effect contract).
            ms.remove_source(10);
        }
        const float v = *p.derived("rep");
        apply_bands(static_cast<double>(v), bands, tags);  // SAME machinery
        rep.push_back(v);
        hostile.push_back(tags.contains("faction.hostile"));
        friendly.push_back(tags.contains("faction.friendly"));
    }

    // Hand-computed flip ticks.
    CHECK(rep[39] == 50.0f);
    CHECK(rep[40] == 20.0f);   // 50 - 30, lands ON the threshold: <= flips
    CHECK_FALSE(hostile[39]);
    CHECK(hostile[40]);        // flip EXACTLY at tick 40
    CHECK(hostile[69]);
    CHECK(rep[70] == 65.0f);   // 50 - 30 + 45
    CHECK_FALSE(hostile[70]);  // hysteresis-free: hostile gone same tick
    CHECK(friendly[70]);       // friendly flips EXACTLY at tick 70
    CHECK(rep[94] == 65.0f);
    CHECK(rep[95] == 95.0f);   // strike removed -> 50 + 45, bit-exact
    CHECK(friendly[95]);
    // Ancestor queries ride 6.2 semantics for free:
    CHECK(tags.contains("faction"));
    CHECK_FALSE(tags.contains("faction.hostile.deep"));

    // Determinism: same script twice -> identical tag/rep streams.
    StatsSet st2;
    ModifierStack ms2;
    StatsPipeline p2(st2, ms2);
    TagSet tags2;
    st2.set_base("rep", 50.0f);
    apply_bands(static_cast<double>(*p2.derived("rep")), bands, tags2);
    for (std::int64_t t = 0; t < 120; ++t) {
        ms2.expire_due(static_cast<tick_t>(t));
        if (t == 40) {
            effect_desc fx;
            fx.effect_id = "player.strike";
            fx.bundle = {flat("rep", "strike", -30.0f)};
            REQUIRE(apply_effect(ms2, 10, fx) == effect_result::applied);
        }
        if (t == 70) {
            effect_desc fx;
            fx.effect_id = "peace.accord";
            fx.bundle = {flat("rep", "accord", 45.0f)};
            REQUIRE(apply_effect(ms2, 11, fx) == effect_result::applied);
        }
        if (t == 95)
            ms2.remove_source(10);
        const float v = *p2.derived("rep");
        apply_bands(static_cast<double>(v), bands, tags2);
        CHECK(std::bit_cast<std::uint32_t>(v) ==
              std::bit_cast<std::uint32_t>(rep[static_cast<std::size_t>(t)]));
        CHECK(tags2.contains("faction.hostile") ==
              hostile[static_cast<std::size_t>(t)]);
        CHECK(tags2.contains("faction.friendly") ==
              friendly[static_cast<std::size_t>(t)]);
    }
}
