#pragma once
// Phase 5 task 5.4 — decision #42 unit-suffix -> SI coercion.
// Authoring units are human (km / AU, days / seconds, degrees, deg/day spin);
// a value is ALWAYS paired with its mandatory suffix. The cooked payload
// carries SI only. This header is the single conversion table; the cooklet
// (5.7+) calls it at parse time, and a bare or unknown suffix is a HARD cook
// error — drift is never silent (#42).
//
// NOTE: unit math lives HERE and in the cooklet only (as a wrapper over
// this header) — never in runtime/core code paths (#42).
//
// 8.1: the suffix table became data (kUnits) carrying each suffix's dim,
// and the "NUMBER SUFFIX" string grammar moved here from the cooklet's TOML
// parser so the def-forms entry gate (#42 enforced ON ENTRY, task 8.1) and
// the cook parser share ONE implementation. ve::cook::si_from_string is a
// wrapper over ve::game::si_from_string — error strings stay byte-identical
// (tests/cook/test_toml.cpp pins their substrings).
#include <array>
#include <cstdint>  // uint8_t discriminants below
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "ve/core/math/base.h"  // ve::core::math::kPi_d

namespace ve::game {

enum class unit_error { bare_suffix,        // empty suffix: mandatory per #42
                        unknown_suffix };   // not in the ratified table

// Dimensional family of a ratified #42 suffix. The 8.1 def-forms gate
// suffixes by the field's dim — stricter at ENTRY than cook by design: the
// cooklet never dim-checks; the def schema says what each field's dim is.
enum class unit_dim : std::uint8_t { length, time, angle, spin };

constexpr std::string_view dim_name(unit_dim d) {
    switch (d) {
        case unit_dim::length: return "length";
        case unit_dim::time:   return "time";
        case unit_dim::angle:  return "angle";
        case unit_dim::spin:   return "spin";
    }
    return "?";
}

// Exact constants: AU is the IAU-2012 nominal astronomical unit (m);
// day = 86400 s; deg = pi/180 rad via the core kPi_d width.
inline constexpr double kMeters_per_AU  = 149597870700.0;
inline constexpr double kSeconds_per_day = 86400.0;

struct ratified_unit {
    std::string_view suffix;
    double factor;  // multiply an authored value by this to get SI
    unit_dim dim;
};

// The ratified #42 suffix table (single source: cook coercion and form
// entry read it). Suffixes are case-sensitive exactly as ratified in #42.
// deg/day's factor groups as (pi/180)/86400 — the tests compare with
// Approx and no cooked golden carries a deg/day value (world-def spin
// authored in 8.7+ converts through the same table).
inline constexpr std::array<ratified_unit, 8> kUnits = {{
    {"m",       1.0,                                 unit_dim::length},
    {"km",      1000.0,                              unit_dim::length},
    {"AU",      kMeters_per_AU,                      unit_dim::length},
    {"s",       1.0,                                 unit_dim::time},
    {"day",     kSeconds_per_day,                    unit_dim::time},
    {"rad",     1.0,                                 unit_dim::angle},
    {"deg",     ve::core::math::kPi_d / 180.0,       unit_dim::angle},
    {"deg/day", (ve::core::math::kPi_d / 180.0) / kSeconds_per_day, unit_dim::spin},
}};

// value + suffix -> SI (meters, seconds, radians, rad/s). Unknown/bare ->
// error. Identical values to the old if-chain (factor multiply by 1.0 is
// exact, so "m"/"s"/"rad" round-trip bitwise).
constexpr std::expected<double, unit_error> coerce_to_si(double value,
                                                         std::string_view suffix) {
    if (suffix.empty()) return std::unexpected(unit_error::bare_suffix);
    for (const auto& u : kUnits)
        if (u.suffix == suffix) return value * u.factor;
    return std::unexpected(unit_error::unknown_suffix);
}

constexpr std::optional<unit_dim> dim_of_suffix(std::string_view suffix) {
    for (const auto& u : kUnits)
        if (u.suffix == suffix) return u.dim;
    return std::nullopt;
}

// ---- shared "NUMBER SUFFIX" string grammar (def-authored fields) ----
// One whitespace token splits number from suffix; std::from_chars keeps the
// historical strictness (rejects inf/nan/leading '+'); the suffix must be
// ratified and — when `want` is set — of that dimension (the entry gate;
// cook passes nullopt). Typed failure kinds so forms can show *why*; the
// cooklet maps them back to the historical message strings via
// entry_error_message.
enum class entry_error : std::uint8_t {
    too_many_tokens, bad_number, bare_suffix, unknown_suffix, wrong_dim,
};
struct entry_failure {
    entry_error kind = entry_error::bad_number;
    std::string token;             // offending token (full text for too_many_tokens)
    unit_dim want = unit_dim::length;  // wrong_dim only: the field's dim
};

std::expected<double, entry_failure> parse_unit_entry(std::string_view text,
                                                      std::optional<unit_dim> want);

// Human-readable failure (shared so forms and cook show the same words).
std::string entry_error_message(const entry_failure& f);

// Cook-compatible coercion: grammar identical, no dim gate. The cooklet's
// ve::cook::si_from_string forwards here.
std::expected<double, std::string> si_from_string(std::string_view text);

}  // namespace ve::game
