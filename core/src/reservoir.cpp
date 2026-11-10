// Phase 6 task 6.9 — see reservoir.h for the contract.
#include "ve/core/sim/reservoir.h"

#include <cmath>

namespace ve::core::sim {

namespace {
constexpr double kFloor = 0.0;

float flow_or_zero(StatsPipeline* p, std::string_view attr) {
    const auto v = p->derived(attr);
    return v ? *v : 0.0f;  // missing attr = no flow (documented fail-open)
}
}  // namespace

Reservoir::Reservoir(double initial, std::vector<reservoir_band> bands, TagSet* tags)
    : m_value(initial < kFloor ? kFloor : initial), m_bands(std::move(bands)), m_tags(tags) {
    evaluate_bands();  // authoring state is tagged at construction (tick 0)
}

void Reservoir::bind(StatsPipeline* p, reservoir_attrs attrs) {
    m_pipeline = p;
    m_attrs = std::move(attrs);
}

void Reservoir::set_value(double v) {
    if (!std::isfinite(v))
        return;  // refuse, keep state (attr write door is atomic too)
    m_value = v < kFloor ? kFloor : v;
    if (m_ceiling >= 0.0 && m_value > m_ceiling)
        m_value = m_ceiling;
    evaluate_bands();  // authoring/refill changes tags immediately
}

void Reservoir::set_ceiling(double cap) {
    if (!std::isfinite(cap) || cap < kFloor)
        return;
    m_ceiling = cap;
    if (m_value > m_ceiling)
        m_value = m_ceiling;
    evaluate_bands();
}

void Reservoir::step() {
    ++m_tick;
    if (!m_pipeline)
        return;  // unbound reservoir just holds value (tick clock still runs)
    const double delta =
        (static_cast<double>(flow_or_zero(m_pipeline, m_attrs.supply_attr)) -
         static_cast<double>(flow_or_zero(m_pipeline, m_attrs.load_attr)) -
         static_cast<double>(flow_or_zero(m_pipeline, m_attrs.leak_attr))) *
        kSim_dt;
    m_value += delta;
    if (m_value < kFloor)
        m_value = kFloor;  // dry: never negative, never "debt"
    if (m_ceiling >= 0.0 && m_value > m_ceiling)
        m_value = m_ceiling;
    evaluate_bands();
}

void Reservoir::evaluate_bands() {
    if (!m_tags)
        return;
    // Hysteresis-free: re-evaluate EVERY band against the post-step value,
    // in registration order (deterministic TagSet insertion order).
    for (const auto& b : m_bands) {
        const bool live = (b.dir == band_dir::at_or_below) ? (m_value <= b.threshold)
                                                           : (m_value >= b.threshold);
        if (live)
            m_tags->add(b.tag);
        else
            m_tags->remove(b.tag);
    }
}

}  // namespace ve::core::sim
