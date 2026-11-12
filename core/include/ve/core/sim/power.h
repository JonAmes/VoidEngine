#pragma once

// Phase 6 task 6.10 — the power pool (decision #19e/#19f): the reservoir
// idea read as supply-vs-draw, plus the priority shed. NO new state:
// which systems are lit is DERIVED from the current aggregated attrs, so
// the pool never desyncs from stats — every call recomputes from the
// pipeline. Oversubscription drops systems by AUTHORED PRIORITY (highest
// priority kept; drops go lowest-first); restoration re-lights in reverse
// priority (highest first). Dropped systems go DARK-NOT-DEAD: exactly the
// #26 machinery — one source per system carrying {function.X *0, own
// draw.X *0} — the draw zeroing is what takes the load off the pool, the
// function zeroing is what the rest of the sim sees. Remove the source and
// the system's aggregates return to baseline bit-exactly (done-when (d)).
//
// Priorities, draws, functions and the supply attr are all opaque cooked
// strings ("power.supply", "laser.priority", ...) — core/ names nothing.
// Priority is an ATTRIBUTE, so damage/glitches can re-rank the shed live.
//
// Determinism: systems iterate in a stable order — priority DESC, ties by
// registration index (never insertion-order-of-unordered-container, never
// map order). Draws aggregate fp32 -> fp64 (pool comparisons in double,
// mirroring 6.9's accumulation rule).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/stats_pipeline.h"

namespace ve::core::sim {

struct power_system {
    std::string id;             // opaque; only used as audit key + test handle
    std::string draw_attr;      // aggregated fp32 draw (per second, authored)
    std::string function_attr;  // the attr zeroed while dark (dark-not-dead)
    std::string priority_attr;  // higher number = kept longer under shortage
};

class PowerPool {
public:
    // source_base: start of a caller-reserved ModifierStack id range of
    // size systems.size(); kDefaultBase sits far from SeatLink's reserved
    // ranges — collision-safe unless a caller squats that range (then
    // add_source refuses, deterministic).
    static constexpr std::uint64_t kDefaultBase = 0xF000'0000'0000'0000ull;
    PowerPool(StatsPipeline& p, ModifierStack& stack, std::string supply_attr,
              std::vector<power_system> systems,
              std::uint64_t source_base = kDefaultBase);

    // Re-shed/re-light against the CURRENT pipeline values. Call every
    // tick (cheap: linear in systems, tens at most) or on demand — it is
    // idempotent, so call-after-mutation is the contract.
    void rebalance();

    [[nodiscard]] bool lit(std::string_view id) const;
    [[nodiscard]] std::optional<float> draw_of(std::string_view id) const;
    [[nodiscard]] double supply() const;
    [[nodiscard]] double total_draw() const;  // live systems only

private:
    struct sys_rt {
        power_system sys;
        std::size_t reg_index = 0;
        bool dark = false;
    };
    void apply_dark(sys_rt& s);
    void lift_dark(sys_rt& s);
    std::optional<float> attr(std::string_view key) const;
    std::size_t index_of(std::string_view id) const;

    StatsPipeline* m_pipe;
    ModifierStack* m_stack;
    std::string m_supply_attr;
    std::vector<sys_rt> m_sys;  // registration order
    std::uint64_t m_base = kDefaultBase;
};

}  // namespace ve::core::sim
