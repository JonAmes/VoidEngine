#include "ve/core/sim/well_blend.h"

#include <algorithm>
#include <cmath>

namespace ve::core::sim {

using ve::core::math::dvec3;

namespace {
inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
// smoothstep(0,1): s(0)=s'(0)=s(1)=s'(1)=0 — the crossfade shape.
inline double smoothstep01(double u) { return u * u * (3.0 - 2.0 * u); }
} // namespace

dvec3 total_accel(const rail_world& world, const dvec3& pos, tick_t t, double dominance_band) {
    const std::int32_t n = static_cast<std::int32_t>(world.body_count());
    dvec3 acc(0, 0, 0);
    const double k = dominance_band;
    // Three ordered passes (max -> weight sum -> accumulate). Weights are a
    // pure function of (A_i, g_max, band), so nothing needs storing: no heap,
    // no capacity cliff, body count unbounded. well_accel is cheap analytic
    // math; re-evaluating beats caching for Phase 4 body counts and keeps a
    // single codepath (determinism #5).
    double g_max = 0.0;
    for (std::int32_t i = 0; i < n; ++i)
        g_max = std::max(g_max, length(well_accel(world, i, pos, t)));
    if (g_max == 0.0) return acc;  // exact zero outside every well
    auto weight = [&](std::int32_t i) {
        const double a = length(well_accel(world, i, pos, t));
        return a == 0.0 ? 0.0 : smoothstep01(clamp01((a / g_max - (1.0 - k)) / k));
    };
    double w_sum = 0.0;
    for (std::int32_t i = 0; i < n; ++i) w_sum += weight(i);
    // w_sum >= 1.0 always: the max well maps to smoothstep(1) = 1 exactly.
    for (std::int32_t i = 0; i < n; ++i) {
        const double w = weight(i);
        if (w == 0.0) continue;
        acc = acc + well_accel(world, i, pos, t) * (w / w_sum);
    }
    return acc;
}

std::optional<std::int32_t> home_well_tracker::update(const rail_world& world, const dvec3& pos, tick_t t) {
    std::int32_t best = -1;
    double best_a = 0.0;
    for (std::int32_t i = 0, n = static_cast<std::int32_t>(world.body_count()); i < n; ++i) {
        const double a = length(well_accel(world, i, pos, t));
        if (a > best_a) { best_a = a; best = i; }  // strict >: ties keep the lower id
    }
    if (best < 0) return m_home;  // outside all wells: framing persists
    if (!m_home) { m_home = best; return m_home; }
    const double home_a = length(well_accel(world, *m_home, pos, t));
    // Hysteresis band: only a challenger STRICTLY beating home by (1+hyst)
    // takes the crown; equal-strength regions cannot chatter.
    if (best_a > home_a * (1.0 + m_hyst)) m_home = best;
    return m_home;
}

} // namespace ve::core::sim
