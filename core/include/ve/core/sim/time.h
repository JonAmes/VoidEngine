#pragma once
// core/sim time (decision #45a): the canonical fixed tick is 1/60 s and sim
// time is an INTEGER tick count — never float-seconds state, never wall
// clock (determinism rule #5: replay scrubber, ghost sim, and time
// compression all index off this same integer).

#include <cstdint>

namespace ve::core::sim {

using tick_t = std::int64_t;

inline constexpr double kSim_tick_hz = 60.0;
inline constexpr double kSim_tick_dt = 1.0 / kSim_tick_hz;  // seconds per tick

// Divide by 60.0 rather than multiply by the non-representable 1/60: a
// division is correctly rounded, so tick-aligned durations (600 ticks =
// exactly 10.0 s) evaluate exactly — hand-computed test pins stay clean and
// rail periods hit their pins at exact tick multiples.
constexpr double tick_to_seconds(tick_t t) { return static_cast<double>(t) / kSim_tick_hz; }

} // namespace ve::core::sim
