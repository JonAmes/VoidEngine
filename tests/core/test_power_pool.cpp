// test: power_pool — 6.10 (#19f) done-when (d) pt2: oversubscription drops
// LOWEST priority first; restoration re-lights highest-first; a shed system
// goes dark-not-dead (#26 zeroing) and its aggregates return to baseline
// BIT-EXACTLY when restored. All numbers hand-computed, binary-exact.
#include <bit>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/power.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"

namespace {
using namespace ve::core;
using namespace ve::core::sim;

modifier_desc flat(std::string_view attr, std::string_view id, float v) {
    auto m = modifier_desc{};
    m.target_attr = std::string(attr);
    m.stack_id = std::string(id);
    m.op = mod_op::flat_add;
    m.value = v;
    m.policy = stack_policy::exclusive;
    return m;
}

std::vector<power_system> ship_systems() {
    // draw / priority per PLAN-shaped example: bridge > sensors > lasers > ab
    return {
        {"bridge", "bridge.draw", "bridge.fn", "bridge.priority"},
        {"sensors", "sensors.draw", "sensors.fn", "sensors.priority"},
        {"lasers", "lasers.draw", "lasers.fn", "lasers.priority"},
        {"ab", "ab.draw", "ab.fn", "ab.priority"},
    };
}

struct Rig {
    StatsSet st;
    ModifierStack ms;
    StatsPipeline sp;
    Rig() : sp(st, ms) {
        st.set_base("power.supply", 100.0f);
        st.set_base("bridge.draw", 10.0f);
        st.set_base("sensors.draw", 20.0f);
        st.set_base("lasers.draw", 50.0f);
        st.set_base("ab.draw", 40.0f);
        st.set_base("bridge.priority", 90.0f);
        st.set_base("sensors.priority", 50.0f);
        st.set_base("lasers.priority", 30.0f);
        st.set_base("ab.priority", 10.0f);
        for (const char* fn : {"bridge.fn", "sensors.fn", "lasers.fn", "ab.fn"})
            st.set_base(fn, 1.0f);
    }
};
}  // namespace

TEST_CASE("power_pool") {
    SUBCASE("moderate oversubscription drops ONLY the lowest priority") {
        Rig r;
        PowerPool pool(r.sp, r.ms, "power.supply", ship_systems());
        pool.rebalance();  // 120 > 100 -> shed ab (p10): 80 <= 100
        CHECK_FALSE(pool.lit("ab"));
        CHECK(pool.lit("bridge"));
        CHECK(pool.lit("sensors"));
        CHECK(pool.lit("lasers"));
        CHECK(pool.total_draw() == 80.0);
        CHECK(pool.supply() == 100.0);
        // dark-not-dead: function AND own draw read zero DERIVED...
        CHECK(r.sp.derived("ab.fn") == 0.0f);
        CHECK(r.sp.derived("ab.draw") == 0.0f);
        // ...while baselines are untouched (that's the "not dead" part).
        CHECK(r.st.base("ab.fn") == 1.0f);
        CHECK(r.st.base("ab.draw") == 40.0f);
        // Idempotent: re-running changes nothing.
        pool.rebalance();
        CHECK_FALSE(pool.lit("ab"));
        CHECK(pool.total_draw() == 80.0);
    }

    SUBCASE("deep brownout drops lowest-first; restore re-lights highest-first; baseline returns bit-exactly") {
        Rig r;
        PowerPool pool(r.sp, r.ms, "power.supply", ship_systems());
        r.ms.add_source(1);
        r.ms.add_modifier(1, flat("power.supply", "damage", -70.0f));  // supply 30
        pool.rebalance();
        // 120 > 30: shed ab (p10) -> 80; still > 30: shed lasers (p30) -> 30.
        CHECK_FALSE(pool.lit("ab"));
        CHECK_FALSE(pool.lit("lasers"));
        CHECK(pool.lit("bridge"));
        CHECK(pool.lit("sensors"));
        CHECK(pool.total_draw() == 30.0);

        // Repair: supply back to 100. Restore highest-priority dark first:
        // lasers (p30, draw 50): 30+50=80 <= 100 -> lit. ab (p10, 40):
        // 80+40=120 > 100 -> STAYS dark (greedy, no thrash).
        r.ms.remove_source(1);
        pool.rebalance();
        CHECK(pool.lit("lasers"));
        CHECK_FALSE(pool.lit("ab"));
        CHECK(pool.total_draw() == 80.0);

        // Full supply: everything back, aggregates BIT-EXACT to baseline.
        r.ms.add_source(2);
        r.ms.add_modifier(2, flat("power.supply", "boost", 20.0f));  // 120
        pool.rebalance();
        CHECK(pool.lit("ab"));
        CHECK(std::bit_cast<std::uint32_t>(*r.sp.derived("ab.fn")) ==
              std::bit_cast<std::uint32_t>(1.0f));
        CHECK(std::bit_cast<std::uint32_t>(*r.sp.derived("lasers.draw")) ==
              std::bit_cast<std::uint32_t>(50.0f));
        CHECK(pool.total_draw() == 120.0);
    }

    SUBCASE("priority is an ATTRIBUTE: live re-ranking changes who sheds") {
        Rig r;
        PowerPool p2(r.sp, r.ms, "power.supply", ship_systems(),
                     PowerPool::kDefaultBase + 64);
        r.ms.add_source(3);
        r.ms.add_modifier(3, flat("ab.priority", "promotion", 90.0f));  // ab p=100
        p2.rebalance();
        // Priorities now bridge90 ab100 sensors50 lasers30: lowest live is
        // lasers -> shed ONLY lasers: 120-50 = 70 <= 100.
        CHECK_FALSE(p2.lit("lasers"));
        CHECK(p2.lit("ab"));
        CHECK(p2.total_draw() == 70.0);
    }

    SUBCASE("priority ties shed by registration order (deterministic tie-break)") {
        Rig r;
        std::vector<power_system> sys{
            {"first", "a.draw", "a.fn", "tie.priority"},
            {"second", "b.draw", "b.fn", "tie.priority"},
        };
        r.st.set_base("a.draw", 70.0f);
        r.st.set_base("b.draw", 40.0f);
        r.st.set_base("tie.priority", 50.0f);
        r.st.set_base("a.fn", 1.0f);
        r.st.set_base("b.fn", 1.0f);
        PowerPool pool(r.sp, r.ms, "power.supply", std::move(sys));
        pool.rebalance();  // 110 > 100; tie -> LAST registered sheds
        CHECK(pool.lit("first"));
        CHECK_FALSE(pool.lit("second"));
        CHECK(pool.total_draw() == 70.0);
    }

    SUBCASE("missing supply attr = 0 supply = everything dark (fail-closed budget)") {
        Rig r;
        PowerPool pool(r.sp, r.ms, "no.such.supply", ship_systems());
        pool.rebalance();
        CHECK_FALSE(pool.lit("bridge"));
        CHECK_FALSE(pool.lit("ab"));
        CHECK(pool.total_draw() == 0.0);
        CHECK(r.sp.derived("bridge.fn") == 0.0f);
    }

    SUBCASE("twice-run scripted scenario is identical (determinism)") {
        auto run = [] {
            Rig r;
            PowerPool pool(r.sp, r.ms, "power.supply", ship_systems());
            std::string trace;
            for (int tick = 0; tick < 12; ++tick) {
                if (tick == 4) {
                    r.ms.add_source(10 + tick);
                    r.ms.add_modifier(10 + tick, flat("power.supply", "dip", -60.0f));
                }
                if (tick == 8)
                    r.ms.remove_source(14);
                pool.rebalance();
                for (const auto& s : {"bridge", "sensors", "lasers", "ab"})
                    trace += pool.lit(s) ? '1' : '0';
                trace += '|';
            }
            return trace;
        };
        CHECK(run() == run());
    }
}
