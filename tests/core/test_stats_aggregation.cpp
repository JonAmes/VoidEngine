// Phase 6 task 6.4 / done-when (a) — aggregation pipeline: base -> flat ->
// percent -> multiplier -> clamp with hand-computed suites (stack caps,
// exclusivity, clamp order, policy mix) and the dirty-flag contract
// (recompute-on-read, never-per-tick).
//
// Every pin uses binary-exact values (n/2^k) so CHECK(==) is a bit pin:
// the pipeline's whole point is that two runs resolve identically.
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"

using namespace ve::core;

namespace {

modifier_desc mk(std::string attr, std::string stack_id, mod_op op, float v,
                 stack_policy pol = stack_policy::exclusive, std::uint32_t cap = 1,
                 std::optional<std::uint64_t> exp = std::nullopt) {
    return modifier_desc{std::move(attr), std::move(stack_id), op, v, pol, cap, exp, {}};
}

}  // namespace

TEST_CASE("stats_aggregation") {
    SUBCASE("hand-computed full pipeline — order is pinned") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        st.set_base("pilot.competence", 100.0f);
        ms.add_source(1);
        ms.add_modifier(1, mk("pilot.competence", "s.f1", mod_op::flat_add, 10.0f, stack_policy::stack_n, 9));
        ms.add_modifier(1, mk("pilot.competence", "s.f2", mod_op::flat_add, 5.0f, stack_policy::stack_n, 9));
        ms.add_modifier(1, mk("pilot.competence", "s.p1", mod_op::percent, 100.0f, stack_policy::stack_n, 9));
        ms.add_modifier(1, mk("pilot.competence", "s.p2", mod_op::percent, -50.0f, stack_policy::stack_n, 9));
        ms.add_modifier(1, mk("pilot.competence", "s.m1", mod_op::multiplier, 2.0f, stack_policy::stack_n, 9));
        ms.add_modifier(1, mk("pilot.competence", "s.m2", mod_op::multiplier, 0.25f, stack_policy::stack_n, 9));
        // ((100 + 15) * 1.5) * 0.5 = 86.25 exactly.
        CHECK(p.derived("pilot.competence") == 86.25f);
        // A wrong stage order (percent before flat) gives 82.5 — the exact
        // pin above discriminates: (100*1.5 + 15) * 0.5 = 82.5.
    }

    SUBCASE("percent values are percent-POINTS") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        st.set_base("hull.armor", 100.0f);
        ms.add_source(1);
        ms.add_modifier(1, mk("hull.armor", "p.25", mod_op::percent, 25.0f));
        CHECK(p.derived("hull.armor") == 125.0f);
        ms.add_modifier(1, mk("hull.armor", "p.m100", mod_op::percent, -100.0f, stack_policy::stack_n, 2));
        // Σ = 25 - 100 = -75 points -> factor 0.25 -> 25.0 exactly.
        CHECK(p.derived("hull.armor") == 25.0f);
    }

    SUBCASE("clamp runs AFTER every stage (clamp order)") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        ms.add_source(1);
        st.set_base("a.cap_hi", 10.0f);
        st.set_base("b.cap_lo", 40.0f);
        st.set_base("c.unbounded", -1000.0f);
        p.set_clamp("a.cap_hi", 0.0f, 50.0f);
        p.set_clamp("b.cap_lo", 20.0f, 50.0f);
        ms.add_modifier(1, mk("a.cap_hi", "f100", mod_op::flat_add, 100.0f));
        ms.add_modifier(1, mk("b.cap_lo", "q4", mod_op::multiplier, 0.25f));
        ms.add_modifier(1, mk("c.unbounded", "f-1k", mod_op::flat_add, -1000.0f));
        CHECK(p.derived("a.cap_hi") == 50.0f);    // 110 clamped post-hoc
        CHECK(p.derived("b.cap_lo") == 20.0f);    // clamp-after-multiplier (pre-mult clamp would leave 10)
        CHECK(p.derived("c.unbounded") == -2000.0f);
        // Clamp vs percent: 10 * 11 = 110 -> 100 (a pre-percent clamp could never re-exceed 100*11).
        st.set_base("d.pctclamp", 10.0f);
        p.set_clamp("d.pctclamp", 0.0f, 100.0f);
        ms.add_modifier(1, mk("d.pctclamp", "p1k", mod_op::percent, 1000.0f));
        CHECK(p.derived("d.pctclamp") == 100.0f);
    }

    SUBCASE("stack caps + exclusivity feed the arithmetic (done-when (a))") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        st.set_base("hull.health", 100.0f);
        ms.add_source(1);
        ms.add_source(2);
        auto burn = [] { return mk("hull.health", "effect.burn", mod_op::flat_add, -10.0f, stack_policy::stack_n, 3); };
        REQUIRE(ms.add_modifier(1, burn()) == mod_add_result::added);
        REQUIRE(ms.add_modifier(2, burn()) == mod_add_result::added);
        REQUIRE(ms.add_modifier(1, burn()) == mod_add_result::added);
        REQUIRE(ms.add_modifier(1, burn()) == mod_add_result::rejected_cap);
        CHECK(p.derived("hull.health") == 70.0f);  // exactly 3 stacks land
        // Exclusive regen: first carrier wins, second rejected.
        REQUIRE(ms.add_modifier(1, mk("hull.health", "aura.regen", mod_op::flat_add, 10.0f)) == mod_add_result::added);
        REQUIRE(ms.add_modifier(2, mk("hull.health", "aura.regen", mod_op::flat_add, 50.0f)) == mod_add_result::rejected_exclusive);
        CHECK(p.derived("hull.health") == 80.0f);
        // Replace policy on percents: two +50s collapse into one -50.
        st.set_base("ship.turn", 100.0f);
        REQUIRE(ms.add_modifier(1, mk("ship.turn", "aura.turn", mod_op::percent, 50.0f, stack_policy::stack_n, 9)) == mod_add_result::added);
        REQUIRE(ms.add_modifier(2, mk("ship.turn", "aura.turn", mod_op::percent, 50.0f, stack_policy::stack_n, 9)) == mod_add_result::added);
        CHECK(p.derived("ship.turn") == 200.0f);  // 1 + 100/100 = 2
        REQUIRE(ms.add_modifier(1, mk("ship.turn", "aura.turn", mod_op::percent, -50.0f, stack_policy::replace)) == mod_add_result::replaced);
        CHECK(p.derived("ship.turn") == 50.0f);   // single -50 -> 0.5
    }

    SUBCASE("tick expiry drives the sim loop, pipeline follows on read") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        st.set_base("hull.speed", 10.0f);
        ms.add_source(1);
        ms.add_modifier(1, mk("hull.speed", "mod.ab", mod_op::multiplier, 2.0f, stack_policy::exclusive, 1, 10));
        CHECK(p.derived("hull.speed") == 20.0f);
        // Sim tick loop: ticks 0..9 live, 10 drops.
        for (std::uint64_t tick = 0; tick <= 9; ++tick) {
            ms.expire_due(tick);
            CHECK(p.derived("hull.speed") == 20.0f);
        }
        ms.expire_due(10);
        CHECK(p.derived("hull.speed") == 10.0f);  // back to base, no re-add needed
    }

    SUBCASE("dirty flags: recompute-on-read, never-per-tick") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        st.set_base("stat.a", 10.0f);
        st.set_base("stat.b", 20.0f);
        ms.add_source(1);
        ms.add_modifier(1, mk("stat.a", "x", mod_op::flat_add, 1.0f));
        CHECK(p.recompute_count() == 0);
        CHECK(p.derived("stat.a") == 11.0f);
        CHECK(p.recompute_count() == 1);  // one walk for a
        for (int i = 0; i < 100; ++i)
            (void)p.derived("stat.a");
        CHECK(p.recompute_count() == 1);  // clean reads never re-walk
        (void)p.derived("stat.b");  // warm b once (cold read = one legit walk)
        CHECK(p.recompute_count() == 2);
        // 600 sim ticks of reads, nothing mutated -> zero recomputes (the
        // PLAN "never a full recompute every tick" assertion).
        for (std::uint64_t tick = 0; tick < 600; ++tick) {
            ms.expire_due(tick);  // nothing due
            (void)p.derived("stat.a");
            (void)p.derived("stat.b");
        }
        CHECK(p.recompute_count() == 2);
        // Any mutation re-dirties (per-SIDE coarse clock: b re-walks too).
        ms.add_modifier(1, mk("stat.b", "y", mod_op::flat_add, 2.0f));
        CHECK(p.derived("stat.b") == 22.0f);
        CHECK(p.recompute_count() == 3);
        CHECK(p.derived("stat.a") == 11.0f);
        CHECK(p.recompute_count() == 4);
    }

    SUBCASE("staleness is per-SIDE — stack-only mutation visible with base revisions far ahead (6.10 regression)") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        ms.add_source(1);
        st.set_base("k", 10.0f);   // stack mutated BEFORE heavy base authoring:
        CHECK(p.derived("k") == 10.0f);  // stamp (1, 0)
        for (int i = 1; i <= 50; ++i)
            st.set_base("filler", static_cast<float>(i));  // set_rev races to 51
        (void)p.derived("k");      // base side moved: one legit re-walk
        const auto walks = p.recompute_count();
        ms.add_modifier(1, mk("k", "late", mod_op::flat_add, 5.0f));  // stack side only
        // A max() clock sits frozen at 51 here and serves a stale 10.0 —
        // the exact invisibility the 6.10 power pool hit on every modifier.
        CHECK(p.derived("k") == 15.0f);
        CHECK(p.recompute_count() == walks + 1);
    }

    SUBCASE("full recompute == incremental, bit-identical") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        st.set_base("k.1", 3.5f);
        st.set_base("k.2", -7.25f);
        st.set_base("k.3", 100.0f);
        p.set_clamp("k.3", 0.0f, 60.0f);
        ms.add_source(1);
        ms.add_source(2);
        ms.add_modifier(1, mk("k.1", "a", mod_op::percent, 50.0f, stack_policy::stack_n, 9));
        ms.add_modifier(2, mk("k.1", "b", mod_op::multiplier, 0.125f, stack_policy::stack_n, 9));
        ms.add_modifier(2, mk("k.3", "c", mod_op::flat_add, 1000.0f, stack_policy::stack_n, 9));
        std::vector<float> inc;
        for (const auto& e : st.entries())
            inc.push_back(*p.derived(e.key));
        p.recompute_all();
        std::vector<float> full;
        for (const auto& e : st.entries())
            full.push_back(*p.derived(e.key));
        CHECK(inc == full);  // bit-identical, not Approx
    }

    SUBCASE("fp32 overflow saturates — no NaN/inf escapes") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        st.set_base("huge.attr", 1e30f);
        ms.add_source(1);
        ms.add_modifier(1, mk("huge.attr", "m1", mod_op::multiplier, 1e30f, stack_policy::stack_n, 2));
        float d = *p.derived("huge.attr");
        CHECK(std::isfinite(d));
        CHECK(d == std::numeric_limits<float>::max());
        // NaN path via stage-1 overflow then ×0: 1e30 + 3e38 + 3e38 -> inf,
        // inf * 0 -> NaN -> saturates to +/-max, never NaN.
        ms.add_modifier(1, mk("huge.attr", "fa", mod_op::flat_add, 3e38f, stack_policy::stack_n, 3));
        ms.add_modifier(1, mk("huge.attr", "fb", mod_op::flat_add, 3e38f, stack_policy::stack_n, 3));
        ms.add_modifier(1, mk("huge.attr", "m0", mod_op::multiplier, 0.0f, stack_policy::stack_n, 2));
        float d2 = *p.derived("huge.attr");
        CHECK(std::isfinite(d2));
        CHECK(std::fabs(d2) == std::numeric_limits<float>::max());
    }

    SUBCASE("clamp registration validation + unknown attrs nullopt") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        CHECK_FALSE(p.set_clamp("", 0.0f, 1.0f));
        CHECK_FALSE(p.set_clamp("x", 5.0f, 1.0f));       // lo > hi
        CHECK_FALSE(p.set_clamp("x", NAN, 1.0f));        // non-finite bound
        CHECK(p.set_clamp("x", -1.0f, 1.0f));            // allowed before any entry exists
        CHECK(p.clamp_for("x") == clamp_range{-1.0f, 1.0f});
        CHECK(p.set_clamp("x", 0.0f, 2.0f));             // overwrite in place
        CHECK(p.clamp_for("x") == clamp_range{0.0f, 2.0f});
        CHECK(p.clear_clamp("x"));
        CHECK_FALSE(p.clear_clamp("x"));
        CHECK_FALSE(p.clamp_for("x").has_value());
        CHECK_FALSE(p.derived("never.set").has_value());
        // Stack-free StatsSet stays self-consistent (6.1 mirror).
        st.set_base("solo.stat", 4.5f);
        CHECK(p.derived("solo.stat") == 4.5f);
    }
}
