#pragma once
// Phase 6 task 6.6 — the #28b interaction-event rails. Every "use" in the
// game runs on one of these: seat attach/drop (the #18 stack linkage),
// door/airlock state flips (#19a), wire/junction swap (#26d). They exist
// here as TICK-STAMPED, deterministic-ordered bus events — the resolution
// layer that *selects* a target is game-side and later, but the event
// SHAPES are engine data now, and the scripted tests hand-compute their
// exact (tick, seq) streams, which is what makes replays of these rails
// checkable.
//
// Same ordering contract as the stats bus (6.5): begin_tick tags every
// emit; seq is bus-wide monotonic; drain() hands back in (tick, seq)
// order and clears. Payloads are two opaque u64s — which entity, which
// seat/section/part is the caller's namespace; core never interprets
// them (AGENTS rule 1). A flip event carries the NEW state in `b`
// (0/1) because "what state is the door in now" must be readable from
// the log alone for scrubbing.
#include <cstdint>
#include <vector>

#include "ve/core/sim/time.h"

namespace ve::core {

enum class rail_kind : std::uint8_t {
    seat_attach,     // a: occupant entity, b: seat id          (#18)
    seat_drop,       // a: occupant entity, b: seat id          (#18)
    door_flip,       // a: hull/section id, b: new state 0/1    (#19a)
    airlock_flip,    // a: hull/section id, b: new state 0/1    (#19a)
    wire_attach,     // a: junction/run id, b: endpoint id      (#26d)
    wire_swap,       // a: junction/run id, b: spare part id    (#26d)
    junction_swap,   // a: junction id, b: spare part id        (#26d)
};

struct rail_event {
    sim::tick_t tick;
    std::uint64_t seq;
    rail_kind kind;
    std::uint64_t a;
    std::uint64_t b;
};

class EffectEventBus {
public:
    void begin_tick(sim::tick_t t) { m_tick = t; }
    [[nodiscard]] sim::tick_t current_tick() const { return m_tick; }

    void emit(rail_kind kind, std::uint64_t a, std::uint64_t b = 0) {
        m_events.push_back(rail_event{m_tick, m_seq++, kind, a, b});
    }

    std::vector<rail_event> drain();
    [[nodiscard]] std::size_t pending() const { return m_events.size(); }

private:
    std::vector<rail_event> m_events;
    sim::tick_t m_tick = 0;
    std::uint64_t m_seq = 0;
};

}  // namespace ve::core
