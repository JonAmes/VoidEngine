// Phase 6 task 6.3 — see modifiers.h for the contract.
#include "ve/core/sim/modifiers.h"

#include <cmath>

namespace ve::core {

ModifierStack::source* ModifierStack::find_source(std::uint64_t id) {
    for (auto& s : m_sources)
        if (s.id == id)
            return &s;
    return nullptr;
}

const ModifierStack::source* ModifierStack::find_source(std::uint64_t id) const {
    for (const auto& s : m_sources)
        if (s.id == id)
            return &s;
    return nullptr;
}

bool ModifierStack::add_source(std::uint64_t id, std::vector<std::string> tags) {
    if (find_source(id))
        return false;  // ids are unique; two sources with one id would break
                       // deterministic attribution of every modifier
    m_sources.push_back({id, TagSet{}, {}});
    for (const auto& t : tags)
        m_sources.back().tags.add(t);
    touch();
    return true;
}

bool ModifierStack::remove_source(std::uint64_t id) {
    for (std::size_t i = 0; i < m_sources.size(); ++i)
        if (m_sources[i].id == id) {
            m_sources.erase(m_sources.begin() + static_cast<std::ptrdiff_t>(i));
            touch();
            return true;
        }
    return false;
}

bool ModifierStack::has_source(std::uint64_t id) const { return find_source(id) != nullptr; }

std::size_t ModifierStack::count_identity(std::string_view attr, std::string_view stack_id) const {
    std::size_t n = 0;
    for (const auto& s : m_sources)
        for (const auto& m : s.mods)
            if (m.target_attr == attr && m.stack_id == stack_id)
                ++n;
    return n;
}

std::size_t ModifierStack::drop_identity(std::string_view attr, std::string_view stack_id) {
    std::size_t dropped = 0;
    for (auto& s : m_sources) {
        const std::size_t before = s.mods.size();
        std::erase_if(s.mods, [&](const modifier& m) {
            return m.target_attr == attr && m.stack_id == stack_id;
        });
        dropped += before - s.mods.size();
    }
    return dropped;
}

mod_add_result ModifierStack::add_modifier(std::uint64_t source_id, const modifier_desc& md) {
    source* src = find_source(source_id);
    if (!src)
        return mod_add_result::source_unknown;
    if (md.target_attr.empty() || md.stack_id.empty() || !std::isfinite(md.value) ||
        (md.policy == stack_policy::stack_n && md.stack_cap == 0))
        return mod_add_result::invalid;

    const std::size_t colliding = count_identity(md.target_attr, md.stack_id);

    switch (md.policy) {
    case stack_policy::exclusive:
        if (colliding > 0)
            return mod_add_result::rejected_exclusive;
        break;
    case stack_policy::stack_n:
        if (colliding >= md.stack_cap)
            return mod_add_result::rejected_cap;
        break;
    case stack_policy::refresh:
        if (colliding > 0) {
            // Keep existing values; stamp the new expiry onto every
            // same-identity modifier (nullopt = make permanent). Values and
            // insertion positions do not move — only the deadline does.
            for (auto& s : m_sources)
                for (auto& m : s.mods)
                    if (m.target_attr == md.target_attr && m.stack_id == md.stack_id)
                        m.expire_tick = md.expire_tick;
            touch();
            return mod_add_result::refreshed;
        }
        break;  // no collision: falls through to plain insert
    case stack_policy::replace:
        if (colliding > 0)
            drop_identity(md.target_attr, md.stack_id);
        break;
    }

    src->mods.push_back(modifier{
        md.target_attr, md.stack_id, md.op, md.value, md.policy, md.stack_cap,
        md.expire_tick, TagSet{}});
    for (const auto& t : md.tags)
        src->mods.back().tags.add(t);
    touch();
    return colliding > 0 && md.policy == stack_policy::replace ? mod_add_result::replaced
                                                               : mod_add_result::added;
}

std::size_t ModifierStack::expire_due(std::uint64_t tick) {
    // Live while tick < expire_tick: a modifier expiring at T is active for
    // every tick strictly before T and gone when T arrives. Dropped in the
    // total ordered-view order; counts are order-independent but the drop
    // must be deterministic for the replay hash either way.
    std::size_t dropped = 0;
    for (auto& s : m_sources) {
        const std::size_t before = s.mods.size();
        std::erase_if(s.mods, [&](const modifier& m) {
            return m.expire_tick.has_value() && tick >= *m.expire_tick;
        });
        dropped += before - s.mods.size();
    }
    if (dropped > 0)
        touch();
    return dropped;
}

std::size_t ModifierStack::count_modifiers_tagged(std::string_view query) const {
    std::size_t n = 0;
    for (const auto& s : m_sources)
        for (const auto& m : s.mods)
            if (m.tags.contains(query))
                ++n;
    return n;
}

std::size_t ModifierStack::remove_modifiers_tagged(std::string_view query) {
    std::size_t dropped = 0;
    for (auto& s : m_sources) {
        const std::size_t before = s.mods.size();
        std::erase_if(s.mods, [&](const modifier& m) { return m.tags.contains(query); });
        dropped += before - s.mods.size();
    }
    if (dropped > 0)
        touch();
    return dropped;
}

bool ModifierStack::source_tagged(std::uint64_t id, std::string_view query) const {
    const source* s = find_source(id);
    return s && s->tags.contains(query);
}

std::size_t ModifierStack::modifier_count() const {
    std::size_t n = 0;
    for (const auto& s : m_sources)
        n += s.mods.size();
    return n;
}

}  // namespace ve::core
