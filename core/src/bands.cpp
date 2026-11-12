// Phase 6 task 6.13 — see bands.h for the contract.
#include "ve/core/sim/bands.h"

namespace ve::core::sim {

void apply_bands(double value, const std::vector<reservoir_band>& bands, TagSet& tags) {
    // Hysteresis-free: re-evaluate EVERY band against the current value,
    // in registration order (deterministic TagSet insertion order).
    for (const auto& b : bands) {
        const bool live = (b.dir == band_dir::at_or_below) ? (value <= b.threshold)
                                                           : (value >= b.threshold);
        if (live)
            tags.add(b.tag);
        else
            tags.remove(b.tag);
    }
}

}  // namespace ve::core::sim
