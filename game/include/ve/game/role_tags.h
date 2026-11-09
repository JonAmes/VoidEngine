#pragma once
// Phase 5 task 5.11 — seat role tags, decision #18c: the slice crew roster
// is exactly five role stations. Game-side (AGENTS rule 1): core/ occupancy
// machinery never names a role; the game feeds this ratified registry (or a
// cooked superset) to `Occupancy`, and a mount is a seat exactly when it
// carries one of these tags. What a role AFFORDS (which modifier stack) is
// cooked data + Phase 6 — never engine code.
//
// Grammar note: role tags are dot-separated like attr keys; the ratified
// set uses the `station.` prefix (#18). Grammar membership and registry
// membership are separate checks (see the game_role_tags test).
#include <algorithm>
#include <array>
#include <string_view>

namespace ve::game {

inline constexpr std::string_view kRole_station_pilot     = "station.pilot";
inline constexpr std::string_view kRole_station_gunner    = "station.gunner";
inline constexpr std::string_view kRole_station_engineer  = "station.engineer";
inline constexpr std::string_view kRole_station_navigator = "station.navigator";
inline constexpr std::string_view kRole_station_trader    = "station.trader";

// The slice roster (#18c), declaration order.
inline constexpr std::array<std::string_view, 5> kPhase5_role_tags = {
    kRole_station_pilot, kRole_station_gunner, kRole_station_engineer,
    kRole_station_navigator, kRole_station_trader,
};

// Registry membership only (grammar is checked by the cooklet, same rules
// as attr keys).
constexpr bool is_valid_role_tag(std::string_view tag) {
    for (const auto t : kPhase5_role_tags)
        if (t == tag) return true;
    return false;
}

}  // namespace ve::game
