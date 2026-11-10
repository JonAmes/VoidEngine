// Phase 6 task 6.6 — see effect_bus.h for the contract.
#include "ve/core/sim/effect_bus.h"

namespace ve::core {

std::vector<rail_event> EffectEventBus::drain() {
    std::vector<rail_event> out;
    out.swap(m_events);  // keep capacity; next tick appends into fresh space
    return out;
}

}  // namespace ve::core
