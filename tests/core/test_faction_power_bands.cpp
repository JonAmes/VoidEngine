// Phase 6 task 6.14 — faction power as one more reservoir instance
// (PLAN #25c, done-when (g) part 2). NO new core code: power = the 6.9
// reservoir; supply = production + held-lane attrs, load = losses +
// contested attrs — all modifier-fed pipeline attrs; the band tags
// `faction.pressure.<band>` flip through the SAME apply_bands machinery
// as reservoirs and reputation (6.13). Scripted drain/escalation with
// binary-exact rates so every flip tick is hand-computed:
//
//   base production +120/s = +2/tick; power starts 256.
//   t=64  war starts: load +180/s (held lanes contested) -> net -1/tick
//   t=127 faction.pressure.watch (<=320) flips ON: v=320 EXACT (<= pin)
//   t=150 escalation: +120/s more load -> net -3/tick
//   t=182 faction.pressure.declining (<=200) ON: v=199 (t=181 -> 202)
//   t=200 peace: war source removed -> net 0, power flat at 148
//   t=220 rebuild lane: +180/s supply -> net +3/tick
//   t=237 declining OFF (v=202; t=236 -> 199, hysteresis-free)
//   t=277 watch OFF (v=322; t=276 -> 319 still tagged)
//   band >=400 (surge) never fires: scripted peak is 384.
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/modifiers.h>
#include <ve/core/sim/reservoir.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_pipeline.h>
#include <ve/core/sim/tags.h>
#include <ve/core/sim/time.h>

namespace {

using namespace ve::core;
using namespace ve::core::sim;

modifier_desc flat(std::string_view attr, std::string_view stack, float v,
                   std::uint32_t cap = 8) {
    modifier_desc m;
    m.target_attr = std::string(attr);
    m.stack_id = std::string(stack);
    m.op = mod_op::flat_add;
    m.value = v;
    m.policy = stack_policy::stack_n;
    m.stack_cap = cap;
    return m;
}

struct power_run {
    StatsSet st;
    ModifierStack ms;
    StatsPipeline p;
    TagSet tags;
    // Registration order is deterministic; surge rides along to pin
    // "never fires" under the same machinery.
    std::vector<reservoir_band> bands = {
        {320.0, "faction.pressure.watch", band_dir::at_or_below},
        {200.0, "faction.pressure.declining", band_dir::at_or_below},
        {400.0, "faction.pressure.surge", band_dir::at_or_above},
    };
    Reservoir power;
    std::vector<double> level;
    std::vector<bool> watch, declining, surge, anc;

    power_run() : p(st, ms), power(256.0, bands, &tags) {
        st.set_base("fp.supply", 0.0f);
        st.set_base("fp.load", 0.0f);
    }
};

power_run run_faction(std::int64_t ticks) {
    power_run r;
    r.ms.add_source(1);  // home-world production
    r.ms.add_modifier(1, flat("fp.supply", "prod", 120.0f));
    r.power.bind(&r.p, {"fp.supply", "fp.load", ""});  // no leak attr

    for (std::int64_t t = 0; t < ticks; ++t) {
        r.ms.expire_due(static_cast<tick_t>(t));
        if (t == 64) {  // war: held lanes contested
            r.ms.add_source(2);
            r.ms.add_modifier(2, flat("fp.load", "war.contested", 180.0f));
        }
        if (t == 150) {  // escalation: front-line losses
            r.ms.add_source(3);
            r.ms.add_modifier(3, flat("fp.load", "war.losses", 120.0f));
        }
        if (t == 200) {  // peace accords: contested-lane losses stop
            REQUIRE(r.ms.remove_source(2));
        }
        if (t == 220) {  // annexed lane flips to production
            r.ms.add_source(4);
            r.ms.add_modifier(4, flat("fp.supply", "rebuild", 180.0f));
        }
        r.power.step();
        r.level.push_back(r.power.value());
        r.watch.push_back(r.tags.contains("faction.pressure.watch"));
        r.declining.push_back(r.tags.contains("faction.pressure.declining"));
        r.surge.push_back(r.tags.contains("faction.pressure.surge"));
        r.anc.push_back(r.tags.contains("faction.pressure"));
    }
    return r;
}

}  // namespace

TEST_CASE("faction_power_bands") {
    const auto r = run_faction(300);

    // Phase legs, hand-computed and EXACT (all deltas dyadic):
    CHECK(r.level[49] == 356.0);    // 256 + 2*50
    CHECK(r.level[63] == 384.0);    // scripted peak
    CHECK(r.level[64] == 383.0);    // war: -1/tick from here
    CHECK(r.level[126] == 321.0);
    CHECK(r.level[127] == 320.0);   // lands ON the watch threshold
    CHECK(r.level[149] == 298.0);
    CHECK(r.level[150] == 295.0);   // escalation: -3/tick from here
    CHECK(r.level[181] == 202.0);
    CHECK(r.level[182] == 199.0);
    CHECK(r.level[199] == 148.0);
    CHECK(r.level[200] == 148.0);   // peace: net 0 — flat
    CHECK(r.level[219] == 148.0);
    CHECK(r.level[220] == 151.0);   // rebuild: +3/tick from here
    CHECK(r.level[277] == 322.0);
    CHECK(r.level[299] == 388.0);

    // Band flips at the hand-computed ticks, inclusive boundaries:
    CHECK_FALSE(r.watch[126]);
    CHECK(r.watch[127]);            // v == 320: <= flips ON exactly
    CHECK_FALSE(r.declining[181]);
    CHECK(r.declining[182]);        // 199 <= 200
    CHECK(r.declining[236]);        // 199, still declining
    CHECK_FALSE(r.declining[237]);  // 202: OFF the same tick it clears
    CHECK(r.watch[276]);            // 319: still under watch
    CHECK_FALSE(r.watch[277]);      // 322: watch OFF exactly here
    for (std::size_t t = 0; t < r.surge.size(); ++t)
        CHECK_FALSE(r.surge[t]);    // >=400 never reached (peak 384)

    // Ancestor queries ride 6.2 semantics (per-tick, while tags live):
    REQUIRE(r.declining[200]);      // declining live during the peace flat
    CHECK(r.anc[127]);              // watch live -> faction.pressure answers
    CHECK(r.anc[200]);
    CHECK_FALSE(r.anc[299]);        // all bands cleared -> query fails

    // Determinism: same script twice -> bit-identical level/tag streams.
    const auto r2 = run_faction(300);
    REQUIRE(r2.level.size() == r.level.size());
    for (std::size_t t = 0; t < r.level.size(); ++t) {
        CHECK(std::bit_cast<std::uint64_t>(r2.level[t]) ==
              std::bit_cast<std::uint64_t>(r.level[t]));
        CHECK(r2.watch[t] == r.watch[t]);
        CHECK(r2.declining[t] == r.declining[t]);
        CHECK(r2.surge[t] == r.surge[t]);
        CHECK(r2.anc[t] == r.anc[t]);
    }
}
