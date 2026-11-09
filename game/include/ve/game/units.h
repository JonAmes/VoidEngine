#pragma once
// Phase 5 task 5.4 — decision #42 unit-suffix -> SI coercion.
// Authoring units are human (km / AU, days / seconds, degrees, deg/day spin);
// a value is ALWAYS paired with its mandatory suffix. The cooked payload
// carries SI only. This header is the single conversion table; the cooklet
// (5.7+) calls it at parse time, and a bare or unknown suffix is a HARD cook
// error — drift is never silent (#42).
//
// NOTE: unit math lives HERE and in the cooklet only (via this table) —
// never in runtime/core code paths (#42).
#include <expected>
#include <string_view>

#include "ve/core/math/base.h"

namespace ve::game {

enum class unit_error { bare_suffix,        // empty suffix: mandatory per #42
                        unknown_suffix };   // not in the ratified table

// Exact constants: AU is the IAU-2012 nominal astronomical unit (m);
// day = 86400 s; deg = pi/180 rad via the core kPi_d width.
inline constexpr double kMeters_per_AU  = 149597870700.0;
inline constexpr double kSeconds_per_day = 86400.0;

// value + suffix -> SI (meters, seconds, radians, rad/s). Suffixes are
// case-sensitive exactly as ratified in #42. Unknown/bare -> error.
constexpr std::expected<double, unit_error> coerce_to_si(double value,
                                                         std::string_view suffix) {
    if (suffix.empty()) return std::unexpected(unit_error::bare_suffix);
    if (suffix == "m")  return value;
    if (suffix == "km") return value * 1000.0;
    if (suffix == "AU") return value * kMeters_per_AU;
    if (suffix == "s")  return value;
    if (suffix == "day") return value * kSeconds_per_day;
    if (suffix == "rad") return value;
    if (suffix == "deg") return value * (ve::core::math::kPi_d / 180.0);
    if (suffix == "deg/day") return value * (ve::core::math::kPi_d / 180.0) / kSeconds_per_day;
    return std::unexpected(unit_error::unknown_suffix);
}

}  // namespace ve::game
