#pragma once

// Phase 6 task 6.9 — the reservoir primitive (decision #19e): the ONE new
// engine state for this phase. A scalar fp64 integrated per fixed sim tick
// from aggregated attributes:
//     value += (supply - load - leak) * dt          (dt = fixed sim tick)
// "Deterministic by construction: deltas are pure functions of aggregated
// attributes at the tick." fp64 state is deliberate (checklist Notes):
// propellant must hit force*t/Isp hand-exactly over 36k ticks; fp32 attr
// VALUES feed it, but accumulation never rounds in fp32.
//
// Threshold bands are pure data: {threshold, tag, direction}. Evaluation is
// HYSTERESIS-FREE — every band is re-evaluated against the post-step value
// EVERY tick in registration order, tag added when the condition holds,
// removed when it does not. So crossings land at hand-computable ticks and
// reverse crossings un-throw the tag (a refill under an "empty" band does
// not permanently scar the tag set). Tags ride 6.2 TagSet semantics
// (ancestor queries: `propellant.low` answers `propellant`).
//
// Everything downstream is an instance: cabin air per hull section, ship
// propellant (load = thrust/Isp written by the flight glue as a modifier,
// #21c), market stock (flows = lane traffic, price is a SEPARATE derived
// attr over this value — 6.13), faction power (#25c). Core never names any
// of them: attr keys and band tags are opaque cooked/game strings.
//
// Floor: values clamp at kFloor (0.0) by default — an empty tank stays
// exactly 0.0 under a nonzero load (a dry run draws nothing, it never goes
// negative); `Reservoir::set_ceiling` bounds the top (tank capacity).
#include <cstdint>
#include <string>
#include <vector>

#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/tags.h"
#include "ve/core/sim/time.h"  // tick_t

namespace ve::core::sim {

// Fixed sim tick length (AGENTS: fixed timestep; replay uses the same 1/60).
inline constexpr double kSim_dt = 1.0 / 60.0;

enum class band_dir : std::uint8_t {
    at_or_below,  // tag live while value <= threshold (scarcity, low air)
    at_or_above,  // tag live while value >= threshold (overload, fullness)
};

struct reservoir_band {
    double threshold = 0.0;
    std::string tag;  // opaque ancestor-queryable tag (6.2 semantics)
    band_dir dir = band_dir::at_or_below;
};

// Which aggregated attrs feed this reservoir. Missing attributes read as 0
// (a stat nobody wrote contributes nothing — fail-open for flows, the
// band/tag machinery is the fail-SAFE layer).
struct reservoir_attrs {
    std::string supply_attr;
    std::string load_attr;
    std::string leak_attr;
};

class Reservoir {
public:
    Reservoir(double initial, std::vector<reservoir_band> bands, TagSet* tags);

    // Wiring: the pipeline whose derived values feed the flows. Bands must
    // have been validated (nonempty tags; strictly-ordered thresholds is
    // NOT required — bands are independent conditions, registration order
    // is the evaluation order).
    void bind(StatsPipeline* p, reservoir_attrs attrs);

    // Advance one fixed sim tick (call at most once per tick_t; tick() is
    // the internal counter used for crossing bookkeeping). Pure function of
    // the pipeline's CURRENT derived values — callers that mutate stats
    // mid-tick must do it BEFORE stepping.
    void step();

    [[nodiscard]] double value() const { return m_value; }
    [[nodiscard]] tick_t tick() const { return m_tick; }
    void set_value(double v);              // refill/authoring door (finite, >= floor)
    void set_ceiling(double cap);          // capacity clamp for step() (finite, >= 0)

private:
    void evaluate_bands();  // hysteresis-free re-eval in registration order

    double m_value;
    double m_ceiling = -1.0;  // <0 = unbounded above
    std::vector<reservoir_band> m_bands;
    TagSet* m_tags;
    StatsPipeline* m_pipeline = nullptr;  // non-const: derived() is recompute-on-read
    reservoir_attrs m_attrs;
    tick_t m_tick = 0;
};

}  // namespace ve::core::sim
