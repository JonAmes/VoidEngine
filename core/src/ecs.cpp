// 8.2.5 — ecs_world non-template half. Template accessors live in the
// header; storage mutation lives here.
#include "ve/core/ecs.h"

#include "ve/core/assert.h"

namespace ve::core {

std::uint32_t ecs_world::register_component_type(std::string_view name) {
    for (std::size_t i = 0; i < m_names.size(); ++i)
        if (m_names[i] == name) return static_cast<std::uint32_t>(i);
    m_names.emplace_back(name);
    // Existing slots need room for the new column (emplace/get index
    // comps[type] unconditionally on alive entities).
    for (auto& s : m_slots) s.comps.resize(m_names.size());
    return static_cast<std::uint32_t>(m_names.size() - 1);
}

entity_id ecs_world::create() {
    entity_id e;
    if (!m_free.empty()) {
        e.slot = m_free.back();
        m_free.pop_back();
        // Bump on REUSE: ids minted before this slot freed stay provably
        // dead (generation mismatch in alive()), and the bump point is a
        // create, so its order is deterministic.
        auto& s = m_slots[static_cast<std::size_t>(e.slot)];
        s.generation = s.generation + 1u;
        e.generation = s.generation;
    } else {
        e.slot = static_cast<std::int32_t>(m_slots.size());
        m_slots.push_back({});
        m_slots.back().comps.resize(m_names.size());
        e.generation = m_slots.back().generation;  // 0 on first life
    }
    auto& s = m_slots[static_cast<std::size_t>(e.slot)];
    VE_ASSERT(!s.alive);
    s.alive = true;
    ++m_live;
    return e;
}

bool ecs_world::alive(entity_id e) const {
    if (e.slot < 0 || static_cast<std::size_t>(e.slot) >= m_slots.size())
        return false;
    const auto& s = m_slots[static_cast<std::size_t>(e.slot)];
    return s.alive && s.generation == e.generation;
}

bool ecs_world::destroy(entity_id e) {
    if (!alive(e)) return false;
    auto& s = m_slots[static_cast<std::size_t>(e.slot)];
    s.alive = false;
    for (auto& c : s.comps) c.reset();  // components die with the entity
    // Bump on REUSE (below), not here: ids minted before stay provably dead
    // either way, and the bump point is then deterministic w.r.t. creates.
    m_free.push_back(e.slot);
    --m_live;
    return true;
}

bool ecs_world::remove(entity_id e, std::uint32_t type) {
    if (!alive(e) || type >= m_names.size()) return false;
    auto& blob = m_slots[static_cast<std::size_t>(e.slot)].comps[type];
    if (!blob) return false;
    blob.reset();
    return true;
}

bool ecs_world::has(entity_id e, std::uint32_t type) const {
    if (!alive(e) || type >= m_names.size()) return false;
    return static_cast<bool>(
        m_slots[static_cast<std::size_t>(e.slot)].comps[type]);
}

std::vector<ecs_world::entity_view> ecs_world::entities() const {
    std::vector<entity_view> out;
    out.reserve(m_live);
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        const auto& s = m_slots[i];
        if (!s.alive) continue;
        entity_view v;
        v.id = entity_id{static_cast<std::int32_t>(i), s.generation};
        for (std::size_t t = 0; t < s.comps.size(); ++t)
            if (s.comps[t]) v.types.push_back(static_cast<std::uint32_t>(t));
        out.push_back(std::move(v));
    }
    return out;
}

}  // namespace ve::core
