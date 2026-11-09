// Phase 6 task 6.2 — see tags.h for the contract.
#include "ve/core/sim/tags.h"

namespace ve::core {

bool TagSet::add(std::string_view tag) {
    if (tag.empty())
        return false;  // "" answers every prefix query; always a data bug
    for (const auto& t : m_tags)
        if (t == tag)
            return false;  // set semantics: no duplicates
    m_tags.emplace_back(tag);
    return true;
}

bool TagSet::remove(std::string_view tag) {
    for (std::size_t i = 0; i < m_tags.size(); ++i)
        if (m_tags[i] == tag) {
            m_tags.erase(m_tags.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    return false;
}

void TagSet::clear() { m_tags.clear(); }

bool TagSet::contains(std::string_view query) const {
    if (query.empty())
        return false;  // no tag answers the empty query (see add())
    for (const auto& t : m_tags) {
        if (t == query)
            return true;
        // Ancestor rule with dot-boundary strictness: "pilot." matches
        // "pilot.stunned" but NOT "pilotstunned" or "pilot.stun-thing".
        if (t.size() > query.size() && t.starts_with(query) && t[query.size()] == '.')
            return true;
    }
    return false;
}

bool TagSet::contains_exact(std::string_view tag) const {
    for (const auto& t : m_tags)
        if (t == tag)
            return true;
    return false;
}

std::size_t TagSet::count_matching(std::string_view query) const {
    if (query.empty())
        return 0;
    std::size_t n = 0;
    for (const auto& t : m_tags) {
        if (t == query)
            ++n;
        else if (t.size() > query.size() && t.starts_with(query) && t[query.size()] == '.')
            ++n;
    }
    return n;
}

}  // namespace ve::core
