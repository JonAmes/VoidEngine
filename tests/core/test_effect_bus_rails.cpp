// Phase 6 task 6.6 — #28b rails: seat attach/drop, door/airlock flips,
// wire/junction swaps as tick-stamped bus events with HAND-COMPUTED
// (tick, seq) streams, replay-checked: the same script run twice yields
// bit-identical event streams.
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/effect_bus.h"

using namespace ve::core;

TEST_CASE("effect_bus_rails") {
    SUBCASE("hand-computed scripted scenario: exact events, ticks, seqs") {
        // Script (hand-computed by the test author):
        //   tick 10: occupant 9001 sits seat 3 of ship 500  -> seat_attach
        //   tick 10: same tick, door 12 opens (state 1)     -> door_flip
        //   tick 25: airlock 12 cycles (state 1)            -> airlock_flip
        //   tick 40: occupant 9001 leaves seat 3            -> seat_drop
        //   tick 40: junction 77 replaced from spare 812    -> junction_swap
        EffectEventBus bus;
        bus.begin_tick(10);
        bus.emit(rail_kind::seat_attach, 9001, 3);
        bus.emit(rail_kind::door_flip, 12, 1);
        bus.begin_tick(25);
        bus.emit(rail_kind::airlock_flip, 12, 1);
        bus.begin_tick(40);
        bus.emit(rail_kind::seat_drop, 9001, 3);
        bus.emit(rail_kind::junction_swap, 77, 812);

        auto ev = bus.drain();
        REQUIRE(ev.size() == 5);
        struct want { sim::tick_t tick; std::uint64_t seq; rail_kind kind;
                      std::uint64_t a, b; };
        const want w[] = {
            {10, 0, rail_kind::seat_attach, 9001, 3},
            {10, 1, rail_kind::door_flip, 12, 1},
            {25, 2, rail_kind::airlock_flip, 12, 1},
            {40, 3, rail_kind::seat_drop, 9001, 3},
            {40, 4, rail_kind::junction_swap, 77, 812},
        };
        for (std::size_t i = 0; i < 5; ++i) {
            CHECK(ev[i].tick == w[i].tick);
            CHECK(ev[i].seq == w[i].seq);
            CHECK(ev[i].kind == w[i].kind);
            CHECK(ev[i].a == w[i].a);
            CHECK(ev[i].b == w[i].b);
        }
        CHECK(bus.pending() == 0);  // drain clears
    }

    SUBCASE("replay check: identical script -> identical stream (twice)") {
        auto run = [] {
            EffectEventBus bus;
            bus.begin_tick(3);
            bus.emit(rail_kind::wire_attach, 60, 61);
            bus.emit(rail_kind::wire_swap, 60, 999);
            bus.begin_tick(4);
            bus.emit(rail_kind::seat_drop, 1, 2);
            return bus.drain();
        };
        const auto a = run();
        const auto b = run();
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(a[i].tick == b[i].tick);
            CHECK(a[i].seq == b[i].seq);
            CHECK(a[i].kind == b[i].kind);
            CHECK(a[i].a == b[i].a);
            CHECK(a[i].b == b[i].b);
        }
    }

    SUBCASE("seq is bus-wide monotonic across undrained ticks") {
        EffectEventBus bus;
        bus.begin_tick(1);
        bus.emit(rail_kind::door_flip, 5, 0);
        bus.begin_tick(2);  // nothing drained between ticks
        bus.emit(rail_kind::door_flip, 5, 1);
        auto ev = bus.drain();
        REQUIRE(ev.size() == 2);
        CHECK(ev[0].seq == 0);
        CHECK(ev[0].b == 0);  // state carried in payload: scrub-safe
        CHECK(ev[1].seq == 1);
        CHECK(ev[1].b == 1);
    }

    SUBCASE("on-foot verb = same event shape as seated crew (#28)") {
        // The bus cannot tell a seated toggle from an on-foot one: payloads
        // are opaque ids, so the world-state delta per event is identical
        // (decision #28). The test scripts both and pins the shapes equal
        // modulo ids.
        EffectEventBus bus;
        bus.begin_tick(100);
        bus.emit(rail_kind::door_flip, 42, 1);   // toggled by seated pilot
        bus.emit(rail_kind::door_flip, 42, 0);   // toggled by on-foot walker
        auto ev = bus.drain();
        REQUIRE(ev.size() == 2);
        CHECK(ev[0].kind == ev[1].kind);
        CHECK(ev[0].tick == ev[1].tick);
        CHECK(ev[0].b == 1);
        CHECK(ev[1].b == 0);
    }
}
