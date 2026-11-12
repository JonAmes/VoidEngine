#pragma once
// Phase 6 task 6.1 — StatsSet: the single sparse stat storage shared by
// pilots, NPCs, ships, and components (PLAN #16 spine). "Sparse" = only the
// attributes that exist are stored; attribute keys are opaque dotted
// strings from cooked/game data (attr_keys.h lives in game/, AGENTS rule 1
// — core never interprets a key's meaning and never names one).
//
// Values are fp32 by decision: PLAN says "base + derived fp32" and AGENTS
// rule 4 keeps fp64 for sim positions/velocities only. Precision caveat
// honored where it bites: anything that must integrate hand-exactly
// (reservoirs, #19e, 6.9) keeps its STATE in fp64 and only reads/writes
// attributes through this fp32 surface.
//
// Determinism: entries live in an insertion-ordered vector with linear
// lookup — a crew or hull carries tens of stats, not millions, and the
// ordered container is what makes iteration-order anything (AGENTS rule 5).
// Never swap in an unordered map here for sim-visible reads.
//
// base  = the authored/mounted starting value.
// derived = what the modifier pipeline (6.3/6.4) resolves it to; until that
// pipeline exists, set_base keeps derived == base so every reader can go
// through one door.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ve::core {

struct stat_value {
    float base = 0.0f;
    float derived = 0.0f;
    friend bool operator==(const stat_value&, const stat_value&) = default;
};

class StatsSet {
public:
    // Store an attribute's base value; creates the entry at the end of
    // insertion order if new (existing entries keep their position).
    // Rejects non-finite values (returns false, leaves the set untouched):
    // NaN/inf in a stat silently poisons every downstream clamp and hash,
    // and the fp32 pipeline is only hand-verifiable on finite inputs.
    bool set_base(std::string_view key, float v);

    // Remove an attribute entirely (returns false if absent). Used by
    // mount/unmount paths; deterministic regardless of order.
    bool remove(std::string_view key);

    void clear();

    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] std::optional<float> base(std::string_view key) const;
    [[nodiscard]] std::optional<float> derived(std::string_view key) const;

    // Change clock (6.4): bumped on every successful mutation. The
    // StatsPipeline stamps each attribute's last-recompute tick against
    // max(this, ModifierStack::revision()) — recompute-on-read, never a
    // per-tick pass over everything.
    [[nodiscard]] std::uint64_t revision() const { return m_revision; }

    // Pipeline-only write door for derived (6.4). Same finiteness contract
    // as set_base: NaN/inf never reach consumers through StatsSet.
    // Direct derived() readers outside a pipeline see whatever the last
    // resolve left there — for a stack-free StatsSet that is set_base's
    // mirror, so it stays self-consistent.
    bool set_derived(std::string_view key, float v);

    [[nodiscard]] std::size_t size() const { return m_entries.size(); }
    [[nodiscard]] bool empty() const { return m_entries.empty(); }

    struct entry {
        std::string key;
        stat_value value;
    };

    // Iteration is insertion order — callers may rely on it (determinism);
    // do not mutate entries through this view.
    [[nodiscard]] const std::vector<entry>& entries() const { return m_entries; }

private:
    [[nodiscard]] std::size_t find_index(std::string_view key) const;

    std::vector<entry> m_entries;  // insertion order = the only iteration order
    std::uint64_t m_revision = 0;  // bumped on every successful mutation (6.4)
};

}  // namespace ve::core
