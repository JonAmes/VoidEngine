#pragma once
// Phase 6 task 6.2 — hierarchical gameplay tags (PLAN #28a: "the universal
// glue for effects, conditions, AI queries, and UI"). Tags are opaque
// dot-separated strings from cooked/game data — `core/` never names one
// (AGENTS rule 1); the vocabulary lives in game/ headers (facet_actions.h,
// role_tags.h, cargo_tags.h are existing examples).
//
// Hierarchy = pure string ancestry: tag T answers query Q iff T == Q or T
// starts with Q + '.' (ancestor chain). "pilot.stunned" answers queries
// "pilot" and "pilot.stunned". Dot-boundary strictness is the load-bearing
// detail: query "pilot.stun" must NOT answer for tag "pilot.stunned" — a
// prefix match that ignores boundaries would silently make half-typed tags
// match everything below them.
//
// Determinism: insertion-ordered, unique entries; iteration order is
// stable and total (AGENTS rule 5). This is the query layer conditions
// (6.6), threshold crossers (6.9), and dark-sets (6.10-6.12) all ride.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ve::core {

class TagSet {
public:
    // Add a tag; returns false if the exact tag is already present (set
    // semantics) or the tag is empty ("" would answer every prefix query
    // and is always a data bug). Insertion order = iteration order.
    bool add(std::string_view tag);

    // Remove the exact tag only; descendants of a removed tag stay (they
    // are independent tags, not children of storage).
    bool remove(std::string_view tag);

    void clear();

    // Ancestor-inclusive query: true if the set holds `query` itself or
    // any tag under it ("pilot" is answered by "pilot.stunned").
    [[nodiscard]] bool contains(std::string_view query) const;

    // Literal query: the exact tag must be present.
    [[nodiscard]] bool contains_exact(std::string_view tag) const;

    // How many held tags answer `query` (ancestor-inclusive). Conditions
    // and gates read this to distinguish "one pilot stunned" from "the
    // whole crew is".
    [[nodiscard]] std::size_t count_matching(std::string_view query) const;

    [[nodiscard]] std::size_t size() const { return m_tags.size(); }
    [[nodiscard]] bool empty() const { return m_tags.empty(); }

    // Insertion order — callers may rely on it (determinism).
    [[nodiscard]] const std::vector<std::string>& tags() const { return m_tags; }

private:
    std::vector<std::string> m_tags;  // unique, insertion order
};

}  // namespace ve::core
