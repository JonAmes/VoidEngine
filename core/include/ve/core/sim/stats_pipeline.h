#pragma once
// Phase 6 task 6.4 — the aggregation pipeline (PLAN #16 "modifier stack
// resolution"): the single door that turns base + the ordered modifier
// view into derived values.
//
// Per-attribute resolution order is FIXED (done-when (a) "clamp order"):
//   v  = base
//   v += sum(flat_add values)                 // stage 1
//   v *= 1 + sum(percent values)/100          // stage 2 (percent-POINTS)
//   v *= product(multiplier values)           // stage 3
//   v  = clamp(v, lo, hi) iff a range is      // stage 4 — clamp ALWAYS last
//        registered for this attribute
// Sums/products accumulate in the stack's total ordered view (source order,
// then modifier order) so fp32 accumulation order is deterministic —
// associativity is not free in fp32 and the replay hash must not care
// about it (AGENTS rule 5).
//
// Dirty flags (PLAN: "recompute on read ... never a full recompute every
// tick"): each attribute carries the last (max of StatsSet::revision(),
// ModifierStack::revision()) it resolved at; derived() re-walks only when
// that stamp is stale. recompute_count() is the test-visible counter that
// pins "never per-tick".
//
// Finiteness: base and every stored modifier value are finite by
// construction (6.1/6.3), so NaN/inf can only come from fp32 overflow in
// the arithmetic; such results saturate to +/-FLT_MAX instead of leaking
// poisoned values into consumers, clamps, or the replay hash.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/stats.h"

namespace ve::core {

class StatsEventBus;  // 6.5; declared here to keep the dependency one-way
                      // (bus does not know the pipeline exists)

struct clamp_range {
    float lo = 0.0f;
    float hi = 0.0f;
    friend bool operator==(const clamp_range&, const clamp_range&) = default;
};

class StatsPipeline {
public:
    // Borrows both sides: StatsSet owns the values, ModifierStack owns the
    // rules. Neither is copied; neither may outlive the pipeline's use.
    StatsPipeline(StatsSet& stats, ModifierStack& mods) : m_stats(stats), m_mods(mods) {}

    // Per-attribute clamp ranges. Validation is total: finite bounds and
    // lo <= hi, else rejected unchanged. No range registered = unbounded
    // stage 4. Keyed by attribute, independent of whether an entry exists
    // yet (authored before the stat is mounted).
    bool set_clamp(std::string_view attr, float lo, float hi);
    bool clear_clamp(std::string_view attr);
    [[nodiscard]] std::optional<clamp_range> clamp_for(std::string_view attr) const;

    // The read path. nullopt = no such attribute. Recomputes exactly when
    // the attribute is stale (some stats/stack mutation happened since the
    // last resolve) — reads on clean data are pure lookups.
    [[nodiscard]] std::optional<float> derived(std::string_view attr);

    // Force re-walk regardless of stamps: one attribute, or every entry in
    // insertion order. Used by the full-vs-incremental property test and
    // by anything that needs all values warm at once (save/state-hash).
    void recompute(std::string_view attr);
    void recompute_all();

    // Total per-attribute recomputes performed (every re-walk counts 1).
    [[nodiscard]] std::uint64_t recompute_count() const { return m_count; }

    // 6.5 event bus: when attached, every recompute whose derived BITS
    // changed records a stat_change on the bus (raw-bit policy: -0.0 ->
    // +0.0 counts, matching the state hash exactly). Null detaches.
    void attach_bus(StatsEventBus* bus) { m_bus = bus; }

private:
    [[nodiscard]] float resolve(std::string_view attr, float base) const;
    [[nodiscard]] const clamp_range* find_clamp(std::string_view attr) const;
    [[nodiscard]] std::uint64_t current_clock() const;
    [[nodiscard]] const std::uint64_t* find_stamp(std::string_view attr) const;
    std::uint64_t* find_stamp(std::string_view attr);
    void set_stamp(std::string_view attr, std::uint64_t stamp);

    StatsSet& m_stats;
    ModifierStack& m_mods;
    StatsEventBus* m_bus = nullptr;  // 6.5; not owned
    std::vector<std::pair<std::string, clamp_range>> m_clamps;   // insertion order
    std::vector<std::pair<std::string, std::uint64_t>> m_stamps;  // insertion order
    std::uint64_t m_count = 0;
};

}  // namespace ve::core
