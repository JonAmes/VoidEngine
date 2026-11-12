// Phase 6 task 6.5 — event bus: derived-bit changes recorded by the
// pipeline in (tick, insertion-seq) order; drains are FIFO and clear; no
// bus attached = pipeline behaves exactly as in 6.4.
#include <bit>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_bus.h"
#include "ve/core/sim/stats_pipeline.h"

using namespace ve::core;
using namespace ve::core::sim;

namespace {
modifier_desc mk(std::string_view attr, std::string_view id, mod_op op, float v) {
    auto m = modifier_desc{};
    m.target_attr = std::string(attr);
    m.stack_id = std::string(id);
    m.op = op;
    m.value = v;
    m.policy = stack_policy::stack_n;
    m.stack_cap = 16;
    return m;
}
}  // namespace

TEST_CASE("stats_event_bus") {
    SUBCASE("first resolve emits base->derived; re-reads emit nothing") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        StatsEventBus bus;
        p.attach_bus(&bus);

        st.set_base("hull.speed", 10.0f);
        ms.add_source(1);
        ms.add_modifier(1, mk("hull.speed", "f", mod_op::flat_add, 5.0f));

        CHECK(*p.derived("hull.speed") == 15.0f);
        REQUIRE(bus.pending() == 1);
        (void)p.derived("hull.speed");  // clean read: no recompute, no event
        CHECK(bus.pending() == 1);

        auto ev = bus.drain();
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].attr == "hull.speed");
        CHECK(ev[0].old_value == 10.0f);  // base mirror was the old derived
        CHECK(ev[0].new_value == 15.0f);
        CHECK(ev[0].tick == 0);
        CHECK(ev[0].seq == 0);
        CHECK(bus.pending() == 0);  // drain clears
    }

    SUBCASE("(tick, seq) ordering across a multi-tick frame") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        StatsEventBus bus;
        p.attach_bus(&bus);

        st.set_base("a.hp", 100.0f);
        st.set_base("b.shield", 50.0f);
        bus.begin_tick(41);
        (void)p.derived("a.hp");   // warm, no change (stack-free mirror)
        (void)p.derived("b.shield");
        CHECK(bus.pending() == 0);

        ms.add_source(1);
        ms.add_source(2);
        ms.add_modifier(1, mk("b.shield", "s", mod_op::multiplier, 0.5f));
        ms.add_modifier(2, mk("a.hp", "h", mod_op::flat_add, -20.0f));
        bus.begin_tick(42);
        (void)p.derived("a.hp");       // 100 -> 80
        (void)p.derived("b.shield");   // 50 -> 25
        auto ev = bus.drain();
        REQUIRE(ev.size() == 2);
        CHECK(ev[0].attr == "a.hp");       // resolve order within a tick
        CHECK(ev[0].tick == 42);
        CHECK(ev[0].seq == 0);
        CHECK(ev[0].old_value == 100.0f);
        CHECK(ev[0].new_value == 80.0f);
        CHECK(ev[1].attr == "b.shield");
        CHECK(ev[1].tick == 42);
        CHECK(ev[1].seq == 1);
    }

    SUBCASE("seq keeps climbing across ticks; undrained events accumulate") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        StatsEventBus bus;
        p.attach_bus(&bus);
        st.set_base("x.y", 1.0f);
        ms.add_source(1);
        ms.add_modifier(1, mk("x.y", "m", mod_op::flat_add, 1.0f));
        bus.begin_tick(7);
        (void)p.derived("x.y");          // 1 -> 2, seq 0
        ms.add_modifier(1, mk("x.y", "m2", mod_op::flat_add, 1.0f));  // same source
        bus.begin_tick(8);
        (void)p.derived("x.y");          // 2 -> 3, seq 1 (never drained 0)
        auto ev = bus.drain();
        REQUIRE(ev.size() == 2);
        CHECK(ev[0].tick == 7);
        CHECK(ev[0].seq == 0);
        CHECK(ev[1].tick == 8);
        CHECK(ev[1].seq == 1);
    }

    SUBCASE("-0.0 -> +0.0 emits (raw-bit policy, matches the state hash)") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        StatsEventBus bus;
        p.attach_bus(&bus);
        st.set_base("z.v", -5.0f);
        ms.add_source(1);
        ms.add_modifier(1, mk("z.v", "neg", mod_op::multiplier, -1.0f));
        CHECK(*p.derived("z.v") == 5.0f);
        bus.drain();
        // swap to x0 on a negative base: derived becomes -0.0 from +5.0
        ms.remove_source(1);
        ms.add_source(2);
        ms.add_modifier(2, mk("z.v", "pos_then_zero", mod_op::multiplier, 0.0f));
        // base -5 * 0 = +0.0? IEEE: -5 * +0 = -0.0. Bit-change 5.0 -> -0.0.
        float d = *p.derived("z.v");
        CHECK(std::bit_cast<std::uint32_t>(d) == std::bit_cast<std::uint32_t>(-0.0f));
        auto ev = bus.drain();
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].old_value == 5.0f);
        CHECK(std::bit_cast<std::uint32_t>(ev[0].new_value) ==
              std::bit_cast<std::uint32_t>(-0.0f));
    }

    SUBCASE("no bus attached: pipeline identical to 6.4 behavior") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        st.set_base("a.b", 10.0f);
        ms.add_source(1);
        ms.add_modifier(1, mk("a.b", "f", mod_op::flat_add, 5.0f));
        CHECK(*p.derived("a.b") == 15.0f);
        CHECK(p.recompute_count() == 1);
    }
}
