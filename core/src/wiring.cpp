// Phase 6 task 6.12 — see wiring.h for the contract.
#include "ve/core/sim/wiring.h"

#include <limits>

namespace ve::core::sim {

namespace {
constexpr std::size_t knpos = std::numeric_limits<std::size_t>::max();

bool carries(wire_kind k, bool power_flow) {
    if (power_flow)
        return k == wire_kind::power || k == wire_kind::power_signal;
    return k == wire_kind::signal || k == wire_kind::power_signal;
}
}  // namespace

WiringReach::WiringReach(StatsPipeline& p, ModifierStack& stack,
                         std::vector<wire_entry> wires,
                         std::vector<wired_component> comps,
                         std::uint64_t source_base)
    : m_pipe(&p), m_stack(&stack), m_base(source_base), m_wires(std::move(wires)) {
    m_wire_live.assign(m_wires.size(), 1);

    // Node registration order is load-bearing (determinism): sockets in
    // first-appearance order over the wire table, then the component
    // registry in order.
    for (const auto& w : m_wires)
        for (const wire_endpoint* e : {&w.from, &w.to})
            if (e->is_socket()) {
                bool seen = false;
                for (const auto& n : m_nodes)
                    if (n.is_socket && n.ref == e->ref)
                        seen = true;
                if (!seen)
                    m_nodes.push_back(node_rt{e->ref, true, true, false, false, false});
            }

    m_comps = std::move(comps);
    m_comp_node.resize(m_comps.size(), knpos);
    for (std::size_t ci = 0; ci < m_comps.size(); ++ci) {
        m_comp_node[ci] = m_nodes.size();
        m_nodes.push_back(node_rt{m_comps[ci].ref, false, false, false, false, false});
    }
    m_dark.assign(m_comps.size(), 0);
    recompute();  // authored state (everything unmounted) is evaluated at once
}

std::size_t WiringReach::wire_index(std::string_view id) const {
    for (std::size_t i = 0; i < m_wires.size(); ++i)
        if (m_wires[i].id == id)
            return i;
    return knpos;
}

std::size_t WiringReach::comp_index(std::string_view ref) const {
    for (std::size_t i = 0; i < m_comps.size(); ++i)
        if (m_comps[i].ref == ref)
            return i;
    return knpos;
}

std::size_t WiringReach::endpoint_node(const wire_endpoint& e) const {
    if (e.is_socket()) {
        for (std::size_t i = 0; i < m_nodes.size(); ++i)
            if (m_nodes[i].is_socket && m_nodes[i].ref == e.ref)
                return i;
        return knpos;  // unknown socket: wire is inert (cooklet #26a normally
    }                  // cross-refs reject this authored-side)
    const auto ci = comp_index(e.ref);
    return ci == knpos ? knpos : m_comp_node[ci];
}

// ---- topology events: each valid one triggers EXACTLY one recompute ----

bool WiringReach::mount(std::string_view ref) {
    const auto ci = comp_index(ref);
    if (ci == knpos || m_nodes[m_comp_node[ci]].mounted)
        return false;
    m_nodes[m_comp_node[ci]].mounted = true;
    recompute();
    return true;
}

bool WiringReach::unmount(std::string_view ref) {
    const auto ci = comp_index(ref);
    if (ci == knpos || !m_nodes[m_comp_node[ci]].mounted)
        return false;
    m_nodes[m_comp_node[ci]].mounted = false;
    recompute();  // recompute also lifts any dark source (unmounted = absent)
    return true;
}

bool WiringReach::sever_wire(std::string_view id) {
    const auto wi = wire_index(id);
    if (wi == knpos || !m_wire_live[wi])
        return false;
    m_wire_live[wi] = 0;
    recompute();
    return true;
}

bool WiringReach::repair_wire(std::string_view id) {
    const auto wi = wire_index(id);
    if (wi == knpos || m_wire_live[wi])
        return false;
    m_wire_live[wi] = 1;
    recompute();
    return true;
}

bool WiringReach::kill_component(std::string_view ref) {
    const auto ci = comp_index(ref);
    if (ci == knpos)
        return false;
    auto& n = m_nodes[m_comp_node[ci]];
    if (!n.mounted || n.killed)
        return false;
    n.killed = true;
    recompute();
    return true;
}

bool WiringReach::revive_component(std::string_view ref) {
    const auto ci = comp_index(ref);
    if (ci == knpos)
        return false;
    auto& n = m_nodes[m_comp_node[ci]];
    if (!n.mounted || !n.killed)
        return false;
    n.killed = false;
    recompute();
    return true;
}

// ---- flood: FIFO BFS, wires scanned in registration order ----

void WiringReach::flood(bool power_flow) {
    for (auto& n : m_nodes)
        if (power_flow)
            n.reach_p = false;
        else
            n.reach_s = false;

    std::vector<std::size_t> q;
    q.reserve(m_nodes.size());
    std::size_t head = 0;
    auto seed = [&](std::size_t n) {
        auto& nd = m_nodes[n];
        if (power_flow)
            nd.reach_p = true;
        else
            nd.reach_s = true;
        q.push_back(n);
    };
    // Sources seed in component registration order (deterministic).
    for (std::size_t ci = 0; ci < m_comps.size(); ++ci) {
        const bool src = power_flow ? m_comps[ci].is_power_source : m_comps[ci].is_signal_source;
        const auto n = m_comp_node[ci];
        if (src && node_live(n))
            seed(n);
    }

    while (head < q.size()) {
        const auto n = q[head++];
        for (std::size_t wi = 0; wi < m_wires.size(); ++wi) {
            if (!m_wire_live[wi] || !carries(m_wires[wi].kind, power_flow))
                continue;
            const auto a = endpoint_node(m_wires[wi].from);
            const auto b = endpoint_node(m_wires[wi].to);
            if (a == knpos || b == knpos)
                continue;  // dangling endpoint: wire inert
            std::size_t other = knpos;
            if (a == n)
                other = b;
            else if (b == n)
                other = a;
            else
                continue;
            if (node_live(other)) {
                auto& nd = m_nodes[other];
                const bool seen = power_flow ? nd.reach_p : nd.reach_s;
                if (!seen)
                    seed(other);
            }
        }
    }
}

void WiringReach::apply_dark(std::size_t ci) {
    m_dark[ci] = 1;
    // One source per dark component (6.10 bundle shape): function zero is
    // what the sim sees, draw zero takes the dead load off the pool.
    const auto sid = m_base + ci;
    m_stack->add_source(sid);
    auto zero = [&](std::string_view a) {
        if (a.empty())
            return;
        auto m = modifier_desc{};
        m.target_attr = a;
        m.stack_id = "wiring.dark." + m_comps[ci].ref;
        m.op = mod_op::multiplier;
        m.value = 0.0f;
        m.policy = stack_policy::exclusive;
        m_stack->add_modifier(sid, m);
    };
    zero(m_comps[ci].function_attr);
    zero(m_comps[ci].draw_attr);
}

void WiringReach::lift_dark(std::size_t ci) {
    m_dark[ci] = 0;
    m_stack->remove_source(m_base + ci);
}

void WiringReach::recompute() {
    ++m_recomputes;
    flood(true);   // power
    flood(false);  // signal (independent)

    // Diff-apply dark sources in registry order; only transitions touch the
    // stack, so a repair round-trip removes exactly what the cut added and
    // aggregates return to baseline bit-exactly.
    for (std::size_t ci = 0; ci < m_comps.size(); ++ci) {
        const auto& n = m_nodes[m_comp_node[ci]];
        if (!n.mounted) {
            if (m_dark[ci])
                lift_dark(ci);  // absent component holds no source
            continue;
        }
        const bool want =
            (m_comps[ci].gate_power && !n.reach_p) ||
            (m_comps[ci].gate_signal && !n.reach_s);
        if (want && !m_dark[ci])
            apply_dark(ci);
        else if (!want && m_dark[ci])
            lift_dark(ci);
    }
}

// ---- pure reads ----

bool WiringReach::mounted(std::string_view ref) const {
    const auto ci = comp_index(ref);
    return ci != knpos && m_nodes[m_comp_node[ci]].mounted;
}

bool WiringReach::lit(std::string_view ref) const {
    const auto ci = comp_index(ref);
    return ci != knpos && m_nodes[m_comp_node[ci]].mounted && !m_dark[ci];
}

bool WiringReach::power_reach(std::string_view ref) const {
    const auto ci = comp_index(ref);
    return ci != knpos && m_nodes[m_comp_node[ci]].reach_p;
}

bool WiringReach::signal_reach(std::string_view ref) const {
    const auto ci = comp_index(ref);
    return ci != knpos && m_nodes[m_comp_node[ci]].reach_s;
}

std::vector<std::string> WiringReach::dark_set() const {
    std::vector<std::string> out;
    for (std::size_t ci = 0; ci < m_comps.size(); ++ci)
        if (m_nodes[m_comp_node[ci]].mounted && m_dark[ci])
            out.push_back(m_comps[ci].ref);
    return out;
}

std::size_t WiringReach::recompute_count() const { return m_recomputes; }

}  // namespace ve::core::sim
