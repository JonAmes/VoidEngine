// Phase 6 task 6.10 — see power.h for the contract.
#include "ve/core/sim/power.h"

#include <algorithm>

namespace ve::core::sim {

PowerPool::PowerPool(StatsPipeline& p, ModifierStack& stack, std::string supply_attr,
                     std::vector<power_system> systems, std::uint64_t source_base)
    : m_pipe(&p), m_stack(&stack), m_supply_attr(std::move(supply_attr)), m_base(source_base) {
    m_sys.reserve(systems.size());
    for (std::size_t i = 0; i < systems.size(); ++i)
        m_sys.push_back(sys_rt{std::move(systems[i]), i, false});
}

std::optional<float> PowerPool::attr(std::string_view key) const {
    const auto v = m_pipe->derived(key);
    return v;  // nullopt when nobody authored it
}

std::size_t PowerPool::index_of(std::string_view id) const {
    for (std::size_t i = 0; i < m_sys.size(); ++i)
        if (m_sys[i].sys.id == id)
            return i;
    return m_sys.size();
}

bool PowerPool::lit(std::string_view id) const {
    const auto i = index_of(id);
    return i != m_sys.size() && !m_sys[i].dark;
}

std::optional<float> PowerPool::draw_of(std::string_view id) const {
    const auto i = index_of(id);
    if (i == m_sys.size())
        return std::nullopt;
    return attr(m_sys[i].sys.draw_attr);  // DERIVED: dark systems read 0
}

double PowerPool::supply() const {
    const auto v = attr(m_supply_attr);
    return v ? static_cast<double>(*v) : 0.0;
}

double PowerPool::total_draw() const {
    double total = 0.0;
    for (const auto& s : m_sys) {
        if (s.dark)
            continue;
        const auto d = attr(s.sys.draw_attr);
        if (d)
            total += static_cast<double>(*d);
    }
    return total;
}

void PowerPool::apply_dark(sys_rt& s) {
    s.dark = true;
    // One source, two zeroing modifiers (6.6 effect shape; here hand-baked
    // because the bundle is fixed and needs no condition/expiry): the draw
    // zero takes load OFF the pool, the function zero is what the sim sees.
    const auto sid = m_base + s.reg_index;  // caller-reserved range
    m_stack->add_source(sid);
    auto zero = [&](std::string_view a) {
        auto m = modifier_desc{};
        m.target_attr = a;
        m.stack_id = "power.shed." + s.sys.id;
        m.op = mod_op::multiplier;
        m.value = 0.0f;
        m.policy = stack_policy::exclusive;
        m_stack->add_modifier(sid, m);
    };
    zero(s.sys.draw_attr);
    zero(s.sys.function_attr);
}

void PowerPool::lift_dark(sys_rt& s) {
    s.dark = false;
    m_stack->remove_source(m_base + s.reg_index);
}

void PowerPool::rebalance() {
    // Stable, fully deterministic iteration order: priority DESC, ties by
    // registration index. (Copy sorted; m_sys stays in registration order.)
    std::vector<sys_rt*> order;
    order.reserve(m_sys.size());
    for (auto& s : m_sys)
        order.push_back(&s);
    std::stable_sort(order.begin(), order.end(), [&](const sys_rt* a, const sys_rt* b) {
        const float pa = attr(a->sys.priority_attr).value_or(0.0f);
        const float pb = attr(b->sys.priority_attr).value_or(0.0f);
        if (pa != pb)
            return pa > pb;  // higher priority first
        return a->reg_index < b->reg_index;  // deterministic tie-break
    });

    // Shed: while oversubscribed, drop the LOWEST-priority live system
    // (scan the order tail) until the budget closes or nothing is droppable.
    while (total_draw() > supply()) {
        sys_rt* cand = nullptr;
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            if (!(*it)->dark) {
                cand = *it;
                break;
            }
        if (!cand)
            break;  // everything dark and still short: brownout ceiling
        apply_dark(*cand);
    }

    // Restore: re-light in REVERSE of the shed order = highest priority
    // first (top of `order`). TRIAL-LIFT semantics: a dark system's own
    // draw reads ZEROED (its ×0 modifier is live), so the budget test
    // cannot read the draw in advance — it must LIFT for real, measure,
    // and re-dark if the pool went over. Supply never moves inside one
    // rebalance and lifts only consume headroom, so a single pass is the
    // fixed point (greedy highest-first; skipped systems stay dark until
    // conditions change next call — no backfill past a richer system).
    for (auto* s : order) {
        if (!s->dark)
            continue;
        lift_dark(*s);
        if (total_draw() > supply())
            apply_dark(*s);  // trial failed: re-dark, budget unchanged
    }
}

}  // namespace ve::core::sim
