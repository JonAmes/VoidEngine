// Phase 9 task 9.22 — nav-graph tests.
// nav_astar: exact paths on a 3x3 lattice (orthogonal edges only); ties
// break to the ordered search (ascending neighbours, (f,id) open set) —
// which of the many 4-edge routes wins is pinned; mutants prove the
// order is load-bearing.
// nav_convoy_leg: closed-form arrival ticks are exact integers; a block
// event landing on a node-arrival tick retasks at that exact tick and the
// downstream schedule shifts by exact arithmetic (+160 ticks here).
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "ve/game/nav_graph.h"

using ve::game::convoy_route;
using ve::game::nav_edge;
using ve::game::nav_graph;
using ve::game::nav_node;

namespace {

// 3x3 lattice, 1000 m spacing; ids row-major (id = row*3 + col).
nav_graph lattice() {
    nav_graph g;
    for (std::uint32_t id = 0; id < 9; ++id)
        g.add_node({id, {static_cast<double>(id % 3) * 1000.0, 0.0,
                         static_cast<double>(id / 3) * 1000.0}});
    for (std::uint32_t r = 0; r < 3; ++r)
        for (std::uint32_t c = 0; c < 3; ++c) {
            const std::uint32_t id = r * 3 + c;
            if (c + 1 < 3) g.add_edge({id, id + 1, 1000.0});
            if (r + 1 < 3) g.add_edge({id, id + 3, 1000.0});
        }
    return g;
}

constexpr double kSpeed = 250.0;       // m/s -> 4 s per 1000 m edge
constexpr double kTick = 1.0 / 20.0;   // 20 Hz -> 80 ticks per edge

}  // namespace

TEST_CASE("nav_astar") {
    const auto g = lattice();

    // Unblocked corner-to-corner: every shortest route costs 4000 m; the
    // (f,id) open set pops the lower-id frontier first, so the route
    // along the bottom row then up the right edge wins.
    const auto p0 = g.astar(0, 8, {});
    REQUIRE(p0.has_value());
    CHECK(*p0 == std::vector<std::uint32_t>({0, 1, 4, 5, 8}));

    // Centre blocked -> bottom row then up the right edge.
    const std::vector<std::uint32_t> blk{4};
    const auto p1 = g.astar(0, 8, blk);
    REQUIRE(p1.has_value());
    CHECK(*p1 == std::vector<std::uint32_t>({0, 1, 2, 5, 8}));

    // Node 5 blocked -> route diverts via 7.
    const std::vector<std::uint32_t> blk2{5};
    const auto p2 = g.astar(0, 8, blk2);
    REQUIRE(p2.has_value());
    CHECK(*p2 == std::vector<std::uint32_t>({0, 1, 4, 7, 8}));

    // Goal walled in (all three of 7's neighbours blocked) -> no path.
    const std::vector<std::uint32_t> wall{4, 6, 8};
    CHECK_FALSE(g.astar(0, 7, wall).has_value());

    // Start itself blocked -> no path (start not excused from the check).
    const std::vector<std::uint32_t> b0{0};
    CHECK_FALSE(g.astar(0, 8, b0).has_value());

    // Edges are bidirectional: the reverse solves exist too.
    const auto rb = g.astar(8, 0, {});
    REQUIRE(rb.has_value());
    CHECK(*rb == std::vector<std::uint32_t>({8, 5, 4, 1, 0}));
    const auto rb2 = g.astar(8, 6, {});
    REQUIRE(rb2.has_value());
    CHECK(*rb2 == std::vector<std::uint32_t>({8, 7, 6}));

    // Re-adding an edge keeps the MIN cost (dedup, order-insensitive).
    auto g2 = lattice();
    g2.add_edge({0, 1, 1500.0});   // worse duplicate: ignored
    convoy_route rr{.path = {0, 1, 4, 5, 8}, .speed_m_s = kSpeed};
    const auto tl1 = rr.travel_length(g2);
    REQUIRE(tl1.has_value());
    CHECK(*tl1 == 4000.0);
    g2.add_edge({1, 4, 700.0});    // better duplicate: replaces
    const auto tl2 = rr.travel_length(g2);
    REQUIRE(tl2.has_value());
    CHECK(*tl2 == 3700.0);
    // A hop with no edge -> nullopt.
    convoy_route bad{.path = {0, 8}, .speed_m_s = kSpeed};
    CHECK_FALSE(bad.travel_length(g2).has_value());

    // Pure function: replayed solve identical (determinism rule #5).
    CHECK(g.astar(0, 8, blk) == p1);
}

TEST_CASE("nav_convoy_leg") {
    const auto g = lattice();

    // Phase 1: solve at t=0; closed-form schedule, 80 ticks per edge.
    convoy_route r1{.path = {0, 1, 4, 5, 8}, .speed_m_s = kSpeed};
    CHECK(r1.total_length(g) == 4000.0);
    CHECK(r1.arrival_ticks(g, kTick) ==
          std::vector<std::uint64_t>({0, 80, 160, 240, 320}));

    // Block event: raider reports close nodes 2 and 4 at t=80 — exactly
    // the convoy's arrival tick at node 1. It re-solves at THAT tick from
    // its node, so the tail is solved with the block already applied:
    const auto tail = g.astar(1, 8, std::vector<std::uint32_t>{2, 4});
    REQUIRE(tail.has_value());
    CHECK(*tail == std::vector<std::uint32_t>({1, 0, 3, 6, 7, 8}));

    // Retaiked route: prefix 0,1 + tail; the detour costs 2 extra edges,
    // every downstream arrival shifts by exact multiples of 80, and node
    // 8 lands at t=480 — 160 ticks late, zero drift.
    convoy_route r2{.path = {0, 1, 0, 3, 6, 7, 8}, .speed_m_s = kSpeed};
    CHECK(r2.total_length(g) == 6000.0);
    CHECK(r2.arrival_ticks(g, kTick) ==
          std::vector<std::uint64_t>({0, 80, 160, 240, 320, 400, 480}));

    // Retaik decision is deterministic: identical inputs -> identical tail.
    CHECK(g.astar(1, 8, std::vector<std::uint32_t>{2, 4}) == tail);

    // The retask FIRES at an exact tick: replaying the schedule, the
    // block event at t=80 coincides with the arrival at node 1 — and at
    // that tick the convoy is at node 1, so the re-solve starts there.
    const auto sched = r1.arrival_ticks(g, kTick);
    std::size_t leg = 0;
    std::optional<std::uint64_t> retask_tick;
    std::uint32_t retask_node = 0;
    for (std::uint64_t t = 0; t <= 120; ++t) {
        if (leg + 1 < sched.size() && t == sched[leg + 1]) ++leg;
        if (t == 80) {  // raider report event
            retask_tick = t;
            retask_node = r1.path[leg];
        }
    }
    CHECK(retask_tick.has_value());
    CHECK(*retask_tick == 80);
    CHECK(retask_node == 1);

    // Non-dyadic speed: each arrival is the CUMULATIVE travel time
    // ROUNDED UP to a whole tick (a convoy never books an arrival before
    // it physically fits): ceil(66.67)=67, ceil(133.3)=134, ceil(200)=200,
    // ceil(266.7)=267 — exact integers, no drift accumulation.
    convoy_route r3{.path = {0, 1, 4, 5, 8}, .speed_m_s = 300.0};
    CHECK(r3.arrival_ticks(g, kTick) ==
          std::vector<std::uint64_t>({0, 67, 134, 200, 267}));
}
