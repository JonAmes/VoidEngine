// Phase 6 task 6.1 — StatsSet: sparse keyed storage, fp32, insertion-order
// iteration (the determinism spine of everything later in this phase).
#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include "ve/core/sim/stats.h"

using ve::core::StatsSet;
using ve::core::stat_value;

TEST_CASE("stats_set") {
    SUBCASE("set/get round trip, unknown keys are nullopt") {
        StatsSet s;
        CHECK(s.empty());
        CHECK_FALSE(s.has("pilot.reflex"));
        CHECK_FALSE(s.base("pilot.reflex").has_value());
        CHECK_FALSE(s.derived("pilot.reflex").has_value());

        CHECK(s.set_base("pilot.reflex", 12.5f));
        CHECK(s.has("pilot.reflex"));
        CHECK(s.base("pilot.reflex") == 12.5f);
        CHECK(s.derived("pilot.reflex") == 12.5f);  // pre-6.4: derived == base
        CHECK(s.size() == 1);

        // Overwrite in place; fp32 bit-exact round trip on representable values.
        CHECK(s.set_base("pilot.reflex", -0.25f));
        CHECK(s.base("pilot.reflex") == -0.25f);
        CHECK(s.size() == 1);  // no duplicate entry created
    }

    SUBCASE("non-finite and empty-key bases are rejected, set untouched") {
        StatsSet s;
        CHECK_FALSE(s.set_base("shield.capacity", INFINITY));
        CHECK_FALSE(s.set_base("shield.capacity", -INFINITY));
        CHECK_FALSE(s.set_base("shield.capacity", NAN));
        CHECK_FALSE(s.set_base("", 5.0f));
        CHECK(s.empty());
        CHECK(s.set_base("shield.capacity", 100.0f));
        CHECK_FALSE(s.set_base("shield.capacity", NAN));  // existing key too
        CHECK(s.base("shield.capacity") == 100.0f);       // unchanged
        CHECK(s.size() == 1);
    }

    SUBCASE("iteration order is insertion order, overwrite keeps position") {
        // The whole phase's determinism rests on this order being stable and
        // total; pin it explicitly.
        StatsSet s;
        s.set_base("b.second", 1.0f);
        s.set_base("a.first", 2.0f);
        s.set_base("c.third", 3.0f);
        s.set_base("a.first", 9.0f);  // must NOT move to the back

        std::vector<std::string> order;
        for (const auto& e : s.entries())
            order.push_back(e.key);
        CHECK(order == std::vector<std::string>{"b.second", "a.first", "c.third"});
        CHECK(s.entries()[1].value.base == 9.0f);
    }

    SUBCASE("remove drops the entry; re-insert appends at the end") {
        StatsSet s;
        s.set_base("x.one", 1.0f);
        s.set_base("y.two", 2.0f);
        CHECK(s.remove("x.one"));
        CHECK_FALSE(s.remove("x.one"));
        CHECK(s.size() == 1);
        CHECK_FALSE(s.has("x.one"));
        s.set_base("x.one", 3.0f);
        // Deterministic order after a remove/re-insert round trip:
        CHECK(s.entries()[0].key == "y.two");
        CHECK(s.entries()[1].key == "x.one");
        CHECK(s.entries()[1].value.base == 3.0f);
    }

    SUBCASE("keys are opaque strings — core stores anything dotted, undotted, unicode") {
        // AGENTS rule 1 check: StatsSet must never validate or interpret key
        // grammar (the dotted-identifier grammar gate lives game-side and in
        // the cooklet). Whatever bytes come in are data.
        StatsSet s;
        CHECK(s.set_base("no_dots_at_all", 1.0f));
        CHECK(s.set_base("weird..key", 2.0f));
        CHECK(s.set_base("\xc3\xa9.fraction", 3.0f));
        CHECK(s.size() == 3);
        CHECK(s.base("\xc3\xa9.fraction") == 3.0f);
    }

    SUBCASE("stat_value equality is memberwise") {
        CHECK(stat_value{1.5f, 2.5f} == stat_value{1.5f, 2.5f});
        CHECK_FALSE(stat_value{1.5f, 2.5f} == stat_value{1.5f, 2.6f});
    }
}
