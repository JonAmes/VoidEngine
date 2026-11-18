#pragma once

// Phase 9 task 9.7 — instant-hit weapons (lasers/beams). A laser is a static
// segment: origin + unit dir * range. The hit test REUSES the Phase 7
// oriented-component-bounds machinery (swept.h slab clip): sweeping a point
// at unit velocity over dt = range metres is exactly segment-vs-OBB, so the
// laser reports the same entry hit (t metres from origin) + face normal as
// the time-domain sweeps — one geometry kernel, deterministic fp64, replay
// hashes ride it for free.
//
// Range itself is authored data (`weapon.range` attr, 9.6): the caller reads
// it off the stats spine and passes it here; this layer owns no state.
// Ties/iteration: candidates are scanned in container order and a hit
// replaces the incumbent only on STRICTLY smaller distance — with ordered
// candidate containers (sim determinism rule) the pick is total.

#include <cstdint>
#include <optional>
#include <span>

#include "ve/core/math/vec.h"
#include "ve/core/sim/swept.h"

namespace ve::core::sim {

// One candidate: a component's oriented world bounds plus the id the hit
// report must name. `component` is opaque here (loadout component row index
// in practice) — this layer never interprets it.
struct comp_bounds {
    std::uint32_t component = 0;
    obb bounds;
};

struct laser_hit {
    std::uint32_t component = 0;  // the struck component (named, not just "hull")
    double dist = 0.0;            // metres from origin to the entry point
    math::dvec3 pos{};            // world entry point
    math::dvec3 normal{};         // face normal pointing back at the shooter
};

// Instant-hit raycast: nearest entry among `targets` within [0, range].
// `dir` must be unit (approx_equal, same tolerance as swept.h) so t is
// metres; degenerate inputs (non-finite, range < 0, non-unit dir) yield
// nullopt — core is exception-free. A start-inside candidate hits at
// dist = 0 (first candidate in order wins the 0-tie).
std::optional<laser_hit> laser_raycast(
    math::dvec3 origin, math::dvec3 dir, double range,
    std::span<const comp_bounds> targets);

} // namespace ve::core::sim
