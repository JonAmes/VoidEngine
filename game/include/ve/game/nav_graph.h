#pragma once
// Phase 9 task 9.22 — nav-graph pathing, game-side (#22).
//
// Generic lane/region graph: nodes are waypoints (lane midpoints, well
// mouths, docks), edges are authored lanes with costs. A* here is fully
// DETERMINISTIC: adjacency iterates in ascending neighbour-id order, the
// open set orders by (f-cost, node-id) — no unordered containers anywhere
// in the search (determinism rule #5), so a replayed block schedule
// retasks to bit-identical paths.
//
// "Blocked" is live world state (raider activity, debris): the caller
// passes the blocked node-id set at solve time; the graph itself stores
// no mutable traffic — convoys/pilots re-solve when the set changes and
// retask at the exact tick they see it.
//
// Game-side: nodes/edges are authored content (regions, lanes), not core
// concepts (AGENTS rule 1).
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "ve/core/math/vec.h"

namespace ve::game {

struct nav_node {
    std::uint32_t id = 0;
    core::math::dvec3 pos{};
};

struct nav_edge {
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    double cost = 0.0;  // authored traversal cost (>= 0)
};

class nav_graph {
public:
    // Construction sorts/dedupes adjacency by neighbour id — the solve
    // order never depends on insertion order.
    void add_node(const nav_node& n);
    void add_edge(const nav_edge& e);  // treated as bidirectional

    [[nodiscard]] std::size_t node_count() const { return m_nodes.size(); }
    [[nodiscard]] const core::math::dvec3* node_pos(std::uint32_t id) const;

    // Min edge cost between two nodes (both directions); -1 = no edge.
    [[nodiscard]] double edge_cost(std::uint32_t a, std::uint32_t b) const;

    // A* over live `blocked` (ascending-order ids; start/goal never
    // excused from the block check). nullopt = no path exists.
    // Euclidean*heuristic_scale admissible heuristic on node positions.
    [[nodiscard]] std::optional<std::vector<std::uint32_t>>
    astar(std::uint32_t start, std::uint32_t goal,
          std::span<const std::uint32_t> blocked, double heuristic_scale = 1.0) const;

private:
    struct adj { std::uint32_t to; double cost; };
    std::vector<nav_node> m_nodes;                 // sorted by id
    std::vector<std::vector<adj>> m_adj;           // per node: sorted by `to`
    [[nodiscard]] int index_of(std::uint32_t id) const;
};

// Kinematic convoy advance along a solved path: constant speed, exact
// polyline distance (no per-tick Euler drift — the schedule is closed
// form, so arrival ticks are exact integers under tick*dt division).
struct convoy_route {
    std::vector<std::uint32_t> path;
    double speed_m_s = 0.0;
    // Geometric length along the path (node positions).
    [[nodiscard]] double total_length(const nav_graph& g) const;
    // Sum of authored MIN edge costs along the path; nullopt = a hop has
    // no edge. Discriminates edge dedup (total_length cannot).
    [[nodiscard]] std::optional<double> travel_length(const nav_graph& g) const;
    // tick at which the convoy reaches waypoint index w (0-based along
    // path); ceil((segment_dist/speed)/tick_s), accumulated exactly.
    [[nodiscard]] std::vector<std::uint64_t>
    arrival_ticks(const nav_graph& g, double tick_s) const;
};

}  // namespace ve::game
