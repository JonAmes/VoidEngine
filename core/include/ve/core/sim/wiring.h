#pragma once

// Phase 6 task 6.12 — wire networks (decision #26): power/signal flow is
// graph REACHABILITY over the hull def's authored wires (the DEF0 wire
// table, carried at runtime in loaded_hull::wires), never per-component
// wiring code. NO new persistent state: lit/dark is recomputed by BFS and
// recomputes run ONLY on topology-change events (mount/unmount, wire
// sever/repair, component kill/revive) — clean reads never re-walk (#19e
// "recompute only when inputs change").
//
// A component left unpowered or signal-starved goes DARK-NOT-DEAD exactly
// like 6.10's shed: ONE source per dark component carrying
// {function_attr ×0, draw_attr ×0} (empty attr string = skip that member).
// A cut wire or killed junction cascades its EXACT downstream dark set
// (done-when (e)); a repair-from-spare removes the source and the
// aggregates return to baseline bit-exactly.
//
// Concept-free: refs, attrs and wire kinds are opaque cooked/game strings —
// core never names "reactor"/"gun"/"junction". Roles are DATA: the
// is_power_source / is_signal_source flags are authored by the caller
// (game/cook side). Sources emit while mounted and alive; a source's own
// gating is game-side composition in Phase 6 (documented simplification).
//
// Determinism: node iteration is registration order (sockets in
// first-appearance order over the wire table, components in registry
// order); BFS scans each node's wires in wire registration order with a
// FIFO queue — the flood is a pure function of authored order, so the same
// event sequence always yields the same dark set.
//
// The two floods are INDEPENDENT: signal propagates regardless of power
// (power-gating signal relays is composed later by the game, not baked
// here).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/stats_pipeline.h"

namespace ve::core::sim {

// Wire flow type — matches the DEF0 u8 encoding exactly.
enum class wire_kind : std::uint8_t { power = 0, signal = 1, power_signal = 2 };

// DEF0 endpoint: kind 0 = socket (authored node, always live),
// kind 1 = component (a registry ref; in the graph only while mounted+alive).
struct wire_endpoint {
    std::uint8_t kind = 1;
    std::string ref;
    bool is_socket() const { return kind == 0; }
};

// One authored wire (DEF0 wire-table row, carried verbatim by the loader).
struct wire_entry {
    std::string id;
    wire_kind kind = wire_kind::power;
    std::optional<double> capacity;  // carried for future overload rules;
                                     // reachability itself ignores it
    wire_endpoint from, to;
};

// A component the caller cares about lighting (registry order is the
// deterministic iteration order). Relays (junctions, switchboards) simply
// carry no gates: gate_power/gate_signal false, function_attr empty.
struct wired_component {
    std::string ref;
    std::string function_attr;   // zeroed while dark ("" = never zero)
    std::string draw_attr;       // zeroed while dark ("" = never zero)
    bool gate_power = false;     // lit requires power-reachability
    bool gate_signal = false;    // lit requires signal-reachability
    bool is_power_source = false;
    bool is_signal_source = false;
};

class WiringReach {
public:
    // Reserved per-component dark-source range (one id per registry slot),
    // far from PowerPool's 0xF000… base — squatting it is a caller bug.
    static constexpr std::uint64_t kDefaultBase = 0xE000'0000'0000'0000ull;

    WiringReach(StatsPipeline& p, ModifierStack& stack,
                std::vector<wire_entry> wires,
                std::vector<wired_component> comps,
                std::uint64_t source_base = kDefaultBase);

    // ---- topology-change events; each triggers EXACTLY one recompute ----
    // Unknown ref / wire id, or a no-state-change op: refused, no recompute.
    bool mount(std::string_view ref);            // component enters the net
    bool unmount(std::string_view ref);          // loadout removal
    bool sever_wire(std::string_view id);        // damage: out of the graph
    bool repair_wire(std::string_view id);       // spare swapped in
    bool kill_component(std::string_view ref);   // destroyed: stops relaying
    bool revive_component(std::string_view ref);

    // ---- pure reads (never re-walk) ----
    [[nodiscard]] bool mounted(std::string_view ref) const;
    [[nodiscard]] bool lit(std::string_view ref) const;  // registry components
    [[nodiscard]] bool power_reach(std::string_view ref) const;
    [[nodiscard]] bool signal_reach(std::string_view ref) const;
    // Mounted registry components failing a gate, in REGISTRATION order —
    // the "exact dark set" of done-when (e).
    [[nodiscard]] std::vector<std::string> dark_set() const;
    [[nodiscard]] std::size_t recompute_count() const;

private:
    struct node_rt {  // unified node space: sockets first, then components
        std::string ref;
        bool is_socket = false;
        bool mounted = false;  // components only
        bool killed = false;   // components only
        bool reach_p = false, reach_s = false;
    };

    [[nodiscard]] std::size_t wire_index(std::string_view id) const;
    [[nodiscard]] std::size_t comp_index(std::string_view ref) const;
    [[nodiscard]] std::size_t comp_node(std::size_t ci) const { return m_comp_node[ci]; }
    [[nodiscard]] std::size_t endpoint_node(const wire_endpoint& e) const;
    [[nodiscard]] bool node_live(std::size_t n) const {
        return m_nodes[n].is_socket || (m_nodes[n].mounted && !m_nodes[n].killed);
    }
    void flood(bool power_flow);
    void apply_dark(std::size_t ci);
    void lift_dark(std::size_t ci);
    void recompute();

    StatsPipeline* m_pipe;
    ModifierStack* m_stack;
    std::uint64_t m_base;
    std::vector<wire_entry> m_wires;
    std::vector<char> m_wire_live;
    std::vector<node_rt> m_nodes;
    std::vector<wired_component> m_comps;
    std::vector<std::size_t> m_comp_node;   // registry slot -> node
    std::vector<std::size_t> m_comp_by_node;  // node -> registry slot (npos = socket)
    std::vector<char> m_dark;               // registry slot: source applied
    std::size_t m_recomputes = 0;
};

}  // namespace ve::core::sim
