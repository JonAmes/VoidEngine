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
        // Stack-free mirror: a StatsSet with no pipeline attached stays
        // self-consistent (derived == base). Once a StatsPipeline resolves
        // an attribute, its revision stamp makes it the authority — this
        // mirror only pre-seeds fresh entries.
        m_entries[i].value.derived = v;
    } else {
        m_entries.push_back({std::string(key), stat_value{v, v}});
    }
    ++m_revision;
    return true;
}

bool StatsSet::set_derived(std::string_view key, float v) {
    if (!std::isfinite(v))
        return false;  // the finiteness contract extends through the pipeline
    if (auto i = find_index(key); i != m_entries.size()) {
        m_entries[i].value.derived = v;
        return true;
    }
    return false;
}

bool StatsSet::remove(std::string_view key) {
    if (auto i = find_index(key); i != m_entries.size()) {
        m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
        ++m_revision;
        return true;
    }
    return false;
}

void StatsSet::clear() {
    if (!m_entries.empty())
        ++m_revision;
    m_entries.clear();
}

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
