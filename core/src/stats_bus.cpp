// Phase 6 task 6.5 — see stats_bus.h for the contract.
#include "ve/core/sim/stats_bus.h"

namespace ve::core {

void StatsEventBus::record(std::string_view attr, float old_v, float new_v) {
    m_events.push_back(stat_change{std::string(attr), old_v, new_v, m_tick, m_seq++});
}

std::vector<stat_change> StatsEventBus::drain() {
    std::vector<stat_change> out;
    out.swap(m_events);  // keep the queue's capacity for next tick
    return out;
}

}  // namespace ve::core
