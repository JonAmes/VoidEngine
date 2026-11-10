#pragma once
// Phase 6 task 6.13 — the threshold-band machinery EXTRACTED from the 6.9
// reservoir, so reputation flips use the SAME primitive (PLAN #20c:
// "threshold crossings flip faction tags ... using the same threshold
// machinery as reservoirs"). The reservoir evaluates these against its own
// fp64 state; anything else (reputation, any glued number) calls
// apply_bands with the current value every tick.
//
// Full contract lives with the reservoir (reservoir.h): HYSTERESIS-FREE —
// every band is re-evaluated against the value EVERY evaluation, in
// registration order; tag added when the condition holds, removed when it
// does not. Crossings therefore land at hand-computable ticks and reverse
// crossings un-throw the tag. Tags ride 6.2 TagSet semantics.
//
// struct/enum names keep their 6.9 spelling: it is one type everywhere.
#include <cstdint>
#include <string>
#include <vector>

#include "ve/core/sim/tags.h"

namespace ve::core::sim {

enum class band_dir : std::uint8_t {
    at_or_below,  // tag live while value <= threshold (scarcity, low air)
    at_or_above,  // tag live while value >= threshold (overload, fullness)
};

struct reservoir_band {
    double threshold = 0.0;
    std::string tag;  // opaque ancestor-queryable tag (6.2 semantics)
    band_dir dir = band_dir::at_or_below;
};

// Re-evaluate every band against `value`, in registration order: add the
// tag where the condition holds, remove it where it does not. Callers run
// this every fixed tick — never on edges.
void apply_bands(double value, const std::vector<reservoir_band>& bands, TagSet& tags);

}  // namespace ve::core::sim
