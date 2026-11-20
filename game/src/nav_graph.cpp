// Phase 9 task 9.22 — deterministic A* + closed-form convoy schedule.
#include "ve/game/nav_graph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace ve::game {

void nav_graph::add_node(const nav_node& n) {
    const auto it = std::ranges::lower_bound(m_nodes, n.id, {}, &nav_node::id);
    if (it != m_nodes.end() && it->id == n.id) return;  // first node wins
    const auto idx = static_cast<std::ptrdiff_t>(it - m_nodes.begin());
    m_nodes.insert(it, n);
    // NOTE: must spell the element type — insert(pos, {}) on a
    // vector<vector<T>> binds to the wrong overload set (compiles, then
    // inserts garbage / crashes).
    m_adj.insert(m_adj.begin() + idx, std::vector<adj>{});
}

void nav_graph::add_edge(const nav_edge& e) {
    const int a = index_of(e.from), b = index_of(e.to);
    if (a < 0 || b < 0 || a == b) return;
    auto link = [&](std::vector<adj>& row, std::uint32_t to, double cost) {
        const auto it = std::ranges::lower_bound(row, to, {}, &adj::to);
        if (it != row.end() && it->to == to) { it->cost = std::min(it->cost, cost); return; }
        row.insert(it, {to, cost});
    };
    link(m_adj[static_cast<std::size_t>(a)], e.to, e.cost);
    link(m_adj[static_cast<std::size_t>(b)], e.from, e.cost);
}

int nav_graph::index_of(const std::uint32_t id) const {
    const auto it = std::ranges::lower_bound(m_nodes, id, {}, &nav_node::id);
    return (it != m_nodes.end() && it->id == id)
               ? static_cast<int>(it - m_nodes.begin()) : -1;
}

const core::math::dvec3* nav_graph::node_pos(const std::uint32_t id) const {
    const int i = index_of(id);
    return i < 0 ? nullptr : &m_nodes[static_cast<std::size_t>(i)].pos;
}

double nav_graph::edge_cost(const std::uint32_t a, const std::uint32_t b) const {
    const int i = index_of(a);
    if (i < 0) return -1.0;
    const auto& row = m_adj[static_cast<std::size_t>(i)];
    const auto it = std::ranges::lower_bound(row, b, {}, &adj::to);
    return (it != row.end() && it->to == b) ? it->cost : -1.0;
}

std::optional<std::vector<std::uint32_t>>
nav_graph::astar(const std::uint32_t start, const std::uint32_t goal,
                 const std::span<const std::uint32_t> blocked,
                 const double heuristic_scale) const {
    const int si = index_of(start), gi = index_of(goal);
    if (si < 0 || gi < 0) return std::nullopt;
    auto is_blocked = [&](const std::uint32_t id) {
        return std::binary_search(blocked.begin(), blocked.end(), id);
    };
    if (is_blocked(start) || is_blocked(goal)) return std::nullopt;

    const std::size_t n = m_nodes.size();
    std::vector<double> g(n, std::numeric_limits<double>::infinity());
    std::vector<int> prev(n, -1);
    g[static_cast<std::size_t>(si)] = 0.0;

    // (f, id) total order — never ambiguous, never unordered (#5).
    std::set<std::pair<double, std::uint32_t>> open;
    auto h = [&](const int i) {
        return heuristic_scale *
               core::math::length(m_nodes[static_cast<std::size_t>(i)].pos -
                            m_nodes[static_cast<std::size_t>(gi)].pos);
    };
    open.insert({h(si), start});

    while (!open.empty()) {
        const auto [f, id] = *open.begin();
        open.erase(open.begin());
        const int u = index_of(id);
        if (u < 0) continue;
        if (id == goal) {
            std::vector<std::uint32_t> path;
            for (int c = gi; c >= 0; c = prev[static_cast<std::size_t>(c)])
                path.push_back(m_nodes[static_cast<std::size_t>(c)].id);
            std::ranges::reverse(path);
            return path;
        }
        const double gu = g[static_cast<std::size_t>(u)];
        // neighbours strictly ascending by id: tie paths are reproducible.
        for (const auto& e : m_adj[static_cast<std::size_t>(u)]) {
            if (is_blocked(e.to)) continue;
            const int v = index_of(e.to);
            const double cand = gu + e.cost;
            if (cand < g[static_cast<std::size_t>(v)] - 1e-12) {
                const std::size_t vi = static_cast<std::size_t>(v);
                const auto old_f = g[vi] + h(v);
                if (g[vi] != std::numeric_limits<double>::infinity())
                    open.erase({old_f, e.to});
                g[vi] = cand;
                prev[vi] = u;
                open.insert({cand + h(v), e.to});
            }
        }
        (void)f;
    }
    return std::nullopt;
}

double convoy_route::total_length(const nav_graph& g) const {
    double len = 0.0;
    for (std::size_t i = 1; i < path.size(); ++i) {
        const auto* a = g.node_pos(path[i - 1]);
        const auto* b = g.node_pos(path[i]);
        if (!a || !b) continue;
        len += core::math::length(*b - *a);
    }
    return len;
}

std::optional<double> convoy_route::travel_length(const nav_graph& g) const {
    // Sum of the graph's MIN edge costs (what it physically costs to
    // traverse at authored edge costs, incl. their geometry); nullopt =
    // some consecutive pair has no edge.
    double len = 0.0;
    for (std::size_t i = 1; i < path.size(); ++i) {
        const double c = g.edge_cost(path[i - 1], path[i]);
        if (c < 0.0) return std::nullopt;
        len += c;
    }
    return len;
}

std::vector<std::uint64_t>
convoy_route::arrival_ticks(const nav_graph& g, const double tick_s) const {
    std::vector<std::uint64_t> ticks;
    double cum = 0.0;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i > 0) {
            const auto* a = g.node_pos(path[i - 1]);
            const auto* b = g.node_pos(path[i]);
            if (a && b) cum += core::math::length(*b - *a);
        }
        ticks.push_back(static_cast<std::uint64_t>(
            std::ceil(cum / (speed_m_s * tick_s) - 1e-9)));
    }
    return ticks;
}

}  // namespace ve::game
