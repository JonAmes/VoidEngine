#pragma once
// core/sim home-well selection + overlap crossfade (decision #17b): nested
// wells — a moon inside its planet's well — must keep "stand on the moon,
// down = the moon, not the giant in its sky" true as a test, not a hope.
//
// PHYSICS (pure, stateless): total accel = soft-dominance crossfade. At a
// point, each well i has magnitude A_i; wells within `dominance_band` of the
// strongest participate with weight smoothstep((A_i/A_max - (1-band))/band),
// and the accel is the weight-normalized blend of the well accel vectors.
// Consequences: deep in one well the blend is that well ALONE (a 4x-stronger
// neighbor contributes nothing at the moon's surface); in an overlap shell
// where strengths are comparable the pull vectors crossfade smoothly; the
// max well always has weight exactly 1, so the blend denominator can never
// vanish. Continuity: A_i are C1 (4.4), weights are continuous functions of
// them, and the blend -> 0 as all A_i -> 0 — no force step at any shell
// boundary. Hysteresis must NOT ride in the force path: forces stay a pure
// function of (pos, tick) or ship path-history would alter physics.
//
// FRAMING (stateful, per ship): home_well_tracker carries the one dominant
// home well with a deterministic switch rule + hysteresis band — assist
// framing / "ground" choice reads it, never the integrator.

#include <cstdint>
#include <optional>
#include <vector>

#include "ve/core/math/vec.h"
#include "ve/core/sim/gravity.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// Blend accel at world pos across every active well (ordered vector scan,
// body-id order — #5). band = dominance fraction (authored; 0.25 default).
// Exact zero dvec3 when no well is active at pos.
math::dvec3 total_accel(const rail_world& world, const math::dvec3& pos, tick_t t,
                        double dominance_band = 0.25);

// Per-ship home well: argmax of well magnitude at the queried point, with a
// challenger only taking over if it exceeds the CURRENT home by
// (1 + hysteresis) — deterministic given the same (pos, tick) stream; ties
// resolve to the lower body id. Home persists when a query falls outside all
// wells (coasting between worlds keeps its last framing).
class home_well_tracker {
public:
    explicit home_well_tracker(double hysteresis = 0.1) : m_hyst(hysteresis) {}

    std::optional<std::int32_t> home() const { return m_home; }

    // Feed the ship's current position at the current tick; returns the
    // (possibly updated) home body id.
    std::optional<std::int32_t> update(const rail_world& world, const math::dvec3& pos, tick_t t);

private:
    std::optional<std::int32_t> m_home;
    double m_hyst;
};

} // namespace ve::core::sim
