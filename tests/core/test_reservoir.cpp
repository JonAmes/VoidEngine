// test: reservoir — 6.9 (#19e) done-when (d) part 1: a reservoir drains /
// refills / throws threshold tags at HAND-COMPUTED ticks. All rates chosen
// binary-exact (net 30/s -> 0.5/tick) so crossings land on pinned ticks and
// fp64 accumulation never drifts. Hysteresis-free band re-eval every tick:
// reverse crossings un-throw tags.
#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/reservoir.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"

namespace {
using namespace ve::core;
using namespace ve::core::sim;

modifier_desc rate(std::string_view attr, float v) {
    auto m = modifier_desc{};
    m.target_attr = std::string(attr);
    m.stack_id = "flow";
    m.op = mod_op::flat_add;
    m.value = v;
    m.policy = stack_policy::exclusive;
    return m;
}

struct Rig {
    StatsSet st;
    ModifierStack ms;
    StatsPipeline sp;
    TagSet tags;
    Rig() : sp(st, ms) {
        st.set_base("tank.supply", 0.0f);
        st.set_base("tank.load", 0.0f);
        st.set_base("tank.leak", 0.0f);
    }
};

std::vector<reservoir_band> pressure_bands() {
    return {
        {75.0, "tank.low", band_dir::at_or_below},
        {0.0, "tank.empty", band_dir::at_or_below},
        {95.0, "tank.full", band_dir::at_or_above},
    };
}
}  // namespace

TEST_CASE("reservoir") {
    SUBCASE("drain: value and tag crossings at hand-computed ticks") {
        Rig r;
        Reservoir res(100.0, pressure_bands(), &r.tags);
        res.bind(&r.sp, {"tank.supply", "tank.load", "tank.leak"});
        r.ms.add_source(1);
        r.ms.add_modifier(1, rate("tank.load", 30.0f));  // net -30/s -> -0.5/tick

        // 100 - 0.5t: tank.low at t=50 (exactly 75), empty at t=200 (exactly 0).
        for (int t = 1; t <= 49; ++t) {
            res.step();
            CHECK_FALSE(r.tags.contains("tank.low"));
        }
        res.step();  // t=50
        CHECK(res.value() == 75.0);
        CHECK(r.tags.contains("tank.low"));
        CHECK(r.tags.contains("tank"));  // 6.2 ancestor query answers

        for (int t = 51; t <= 199; ++t) {
            res.step();
            CHECK_FALSE(r.tags.contains("tank.empty"));
        }
        res.step();  // t=200
        CHECK(res.value() == 0.0);
        CHECK(r.tags.contains("tank.empty"));
        CHECK(res.tick() == 200);

        // Dry: nonzero load against an empty tank never goes negative.
        for (int t = 0; t < 10; ++t)
            res.step();
        CHECK(res.value() == 0.0);
    }

    SUBCASE("refill is exact AND hysteresis-free: reverse crossing un-throws") {
        Rig r;
        Reservoir res(10.0, pressure_bands(), &r.tags);
        res.bind(&r.sp, {"tank.supply", "tank.load", "tank.leak"});
        CHECK(r.tags.contains("tank.low"));  // authoring state tagged immediately

        r.ms.add_source(1);
        r.ms.add_modifier(1, rate("tank.supply", 120.0f));  // net +120/s -> +2/tick
        // 10 + 2n crosses strictly above 75 at step n=33 (76); n=32 -> 74.
        for (int n = 1; n <= 32; ++n)
            res.step();
        CHECK(res.value() == 74.0);
        CHECK(r.tags.contains("tank.low"));
        res.step();  // n=33
        CHECK(res.value() == 76.0);
        CHECK_FALSE(r.tags.contains("tank.low"));  // hysteresis-free
        CHECK_FALSE(r.tags.contains("tank"));      // no descendant live -> query dies
        for (int n = 1; n <= 9; ++n)
            res.step();  // 76 + 18 = 94
        CHECK(res.value() == 94.0);
        CHECK_FALSE(r.tags.contains("tank.full"));
        res.step();  // 96 >= 95
        CHECK(res.value() == 96.0);
        CHECK(r.tags.contains("tank.full"));
    }

    SUBCASE("net flows hit exact binary rates incl. leak: 15/s -> 0.25/tick") {
        Rig r;
        Reservoir res(10.0, {{5.0, "air.thin", band_dir::at_or_below}}, &r.tags);
        res.bind(&r.sp, {"tank.supply", "tank.load", "tank.leak"});
        r.ms.add_source(1);
        r.ms.add_modifier(1, rate("tank.load", 12.0f));
        r.ms.add_source(2);
        r.ms.add_modifier(2, rate("tank.leak", 3.0f));
        // net = 0 - 12 - 3 = -15/s -> -0.25/tick (binary-exact).

        for (int t = 1; t <= 19; ++t) {
            res.step();
            CHECK_FALSE(r.tags.contains("air.thin"));
        }
        res.step();  // t=20: 10 - 20*0.25 = 5.0 exactly -> at_or_below fires
        CHECK(res.value() == 5.0);
        CHECK(r.tags.contains("air.thin"));
    }

    SUBCASE("set_value (refill door) re-throws/un-throws immediately, no tick consumed") {
        Rig r;
        Reservoir res(0.0, pressure_bands(), &r.tags);
        // Construction evaluates bands: authoring state is tagged at tick 0.
        CHECK(r.tags.contains("tank.empty"));
        CHECK(r.tags.contains("tank.low"));
        res.step();
        CHECK(r.tags.contains("tank.empty"));
        res.set_value(100.0);
        CHECK_FALSE(r.tags.contains("tank.empty"));
        CHECK_FALSE(r.tags.contains("tank.low"));
        CHECK(res.tick() == 1);  // refill consumed no tick
        res.set_value(-50.0);    // refused below floor -> clamps to 0
        CHECK(res.value() == 0.0);
    }

    SUBCASE("ceiling caps refill; unbound by default") {
        Rig r;
        Reservoir res(90.0, {}, &r.tags);
        r.ms.add_source(1);
        r.ms.add_modifier(1, rate("tank.supply", 120.0f));
        res.bind(&r.sp, {"tank.supply", "tank.load", "tank.leak"});
        for (int t = 0; t < 10; ++t)
            res.step();
        CHECK(res.value() == 110.0);  // unbounded above
        res.set_ceiling(100.0);
        CHECK(res.value() == 100.0);  // clamped down immediately
        res.step();
        CHECK(res.value() == 100.0);  // stays capped
    }

    SUBCASE("unbound pipeline: tick clock runs, value holds") {
        Rig r;
        Reservoir res(50.0, {}, &r.tags);
        for (int t = 0; t < 5; ++t)
            res.step();
        CHECK(res.value() == 50.0);
        CHECK(res.tick() == 5);
    }
}
