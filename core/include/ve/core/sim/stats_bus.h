#pragma once
// Phase 6 task 6.5 — attribute-change event bus. When the 6.4 pipeline
// resolves an attribute and the derived BITS changed (raw-bit policy, same
// contract as the replay hash: -0.0 -> +0.0 IS a change), the change is
// recorded here in (tick, insertion-seq) order — consumers (HUD scrubber
// 6.15, conditions/effects 6.6, UI) drain a deterministic stream instead
// of polling every attribute every tick.
//
// Ordering contract: events carry the sim tick they were emitted on
// (begin_tick tags it) and a bus-wide monotonic seq; drain() returns them
// in exactly that order. Emission itself happens on pipeline recompute —
// so "when did a consumer see it" is "which tick's drain", not "when the
// stack mutated" (mutations are invisible until a resolve re-dirties; the
// PLAN's own model: recompute-on-read).
//
// Values are fp32 (stats surface); sim::tick_t is the sim clock (sim/time.h).
// Concept-free: the bus knows attribute KEYS, never what they mean.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/time.h"

namespace ve::core {

struct stat_change {
    std::string attr;
    float old_value;
    float new_value;
    sim::tick_t tick;             // tick tagged at emit time
    std::uint64_t seq;       // bus-wide monotonic insertion counter
};

class StatsEventBus {
public:
    // Tag subsequent emits with this sim tick. Call at the top of each
    // tick's stats-visible window (before any pipeline reads).
    void begin_tick(sim::tick_t t) { m_tick = t; }
    [[nodiscard]] sim::tick_t current_tick() const { return m_tick; }

    // Return pending changes in (tick, seq) order and clear the queue.
    // Order within a tick = emission order = resolve order that tick,
    // which is deterministic for a deterministic sim (6.4 walk order).
    std::vector<stat_change> drain();

    [[nodiscard]] std::size_t pending() const { return m_events.size(); }

private:
    friend class StatsPipeline;  // the pipeline is the only emitter
    void record(std::string_view attr, float old_v, float new_v);

    std::vector<stat_change> m_events;
    sim::tick_t m_tick = 0;
    std::uint64_t m_seq = 0;
};

}  // namespace ve::core
