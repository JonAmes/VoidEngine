// Phase 6 task 6.1 — see stats.h for the contract.
#include "ve/core/sim/stats.h"

#include <cmath>

namespace ve::core {

std::size_t StatsSet::find_index(std::string_view key) const {
    // Linear scan by design: tens of stats per entity, and insertion order
    // is load-bearing (determinism). n is tiny; do not "optimize" this into
    // an unordered map — that would silently break ordered iteration.
    for (std::size_t i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].key == key)
            return i;
    return m_entries.size();
}

bool StatsSet::set_base(std::string_view key, float v) {
    if (key.empty() || !std::isfinite(v))
        return false;  // empty key or NaN/inf: programming error, store nothing
    if (auto i = find_index(key); i != m_entries.size()) {
        m_entries[i].value.base = v;
        // Until the modifier pipeline lands (6.4), derived tracks base so
        // every reader goes through one door. 6.4 replaces this with the
        // dirty-flag recompute.
        m_entries[i].value.derived = v;
    } else {
        m_entries.push_back({std::string(key), stat_value{v, v}});
    }
    return true;
}

bool StatsSet::remove(std::string_view key) {
    if (auto i = find_index(key); i != m_entries.size()) {
        m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

void StatsSet::clear() { m_entries.clear(); }

bool StatsSet::has(std::string_view key) const {
    return find_index(key) != m_entries.size();
}

std::optional<float> StatsSet::base(std::string_view key) const {
    if (auto i = find_index(key); i != m_entries.size())
        return m_entries[i].value.base;
    return std::nullopt;
}

std::optional<float> StatsSet::derived(std::string_view key) const {
    if (auto i = find_index(key); i != m_entries.size())
        return m_entries[i].value.derived;
    return std::nullopt;
}

}  // namespace ve::core
