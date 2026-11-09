#pragma once
// Phase 5 task 5.4 — the Phase 5 attribute-key set, declared where game
// concepts live (AGENTS rule 1): core/ treats these strings as opaque cooked
// data; only game-side code (and later cooked defs) names them.
//
// Convention (cooks and defs must match): keys are dot-separated segments,
// each [a-z][a-z0-9_]*, lowercase always. The full set is one ordered array
// (deterministic iteration for tooling; order is declaration order).
//
// Value UNITS in defs are human-authored with mandatory suffixes (#42) and
// cooked to SI; these constants only name the keys, never their units.
#include <array>
#include <string_view>

namespace ve::game {

inline constexpr std::string_view kAttr_thrust_force    = "thrust.force";
inline constexpr std::string_view kAttr_thrust_isp      = "thrust.isp";
inline constexpr std::string_view kAttr_power_supply    = "power.supply";
inline constexpr std::string_view kAttr_power_draw      = "power.draw";
inline constexpr std::string_view kAttr_mass            = "mass";
inline constexpr std::string_view kAttr_lift_coeff      = "lift.coeff";
inline constexpr std::string_view kAttr_shield_capacity = "shield.capacity";
inline constexpr std::string_view kAttr_ftl_speed_mult  = "ftl.speed_mult";   // #22d
inline constexpr std::string_view kAttr_ftl_spool_s     = "ftl.spool_s";      // #22d
inline constexpr std::string_view kAttr_ftl_flow        = "ftl.flow";         // #22d
inline constexpr std::string_view kAttr_sensor_range    = "sensor.range";
inline constexpr std::string_view kAttr_sensor_res      = "sensor.res_arcmin"; // #31b
inline constexpr std::string_view kAttr_cargo_capacity  = "cargo.capacity";   // #20a
inline constexpr std::string_view kAttr_interact_range  = "interact.range";   // #28a

// Every Phase 5 key, declaration order. Tooling iterates THIS for
// validation; adding a key means adding it here (duplicate/format gates in
// tests catch mistakes at build time).
inline constexpr std::array<std::string_view, 14> kPhase5_attr_keys = {
    kAttr_thrust_force,       kAttr_thrust_isp,       kAttr_power_supply,
    kAttr_power_draw,         kAttr_mass,             kAttr_lift_coeff,
    kAttr_shield_capacity,    kAttr_ftl_speed_mult,   kAttr_ftl_spool_s,
    kAttr_ftl_flow,           kAttr_sensor_range,     kAttr_sensor_res,
    kAttr_cargo_capacity,     kAttr_interact_range,
};

// Key grammar check: segments [a-z][a-z0-9_]* joined by single '.'.
constexpr bool is_valid_attr_key(std::string_view key) {
    if (key.empty()) return false;
    bool at_seg_start = true;
    for (char c : key) {
        if (c == '.') {
            if (at_seg_start) return false;  // empty segment (".x", "x..y")
            at_seg_start = true;
            continue;
        }
        const bool lower = c >= 'a' && c <= 'z';
        const bool alnum = (c >= '0' && c <= '9') || c == '_';
        if (at_seg_start) {
            if (!lower) return false;
            at_seg_start = false;
        } else if (!lower && !alnum) {
            return false;
        }
    }
    return !at_seg_start;  // no trailing '.'
}

}  // namespace ve::game
