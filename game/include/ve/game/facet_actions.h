#pragma once
// Phase 5 task 5.9b (#28a, ratified at the 5.9b checkpoint) — the closed
// Phase-5 facet-action tag set. The cooklet REJECTS action tags outside this
// registry; the interaction system (#28b, Phase 6+) only ever dispatches
// actions from the same set. Like kPhase5_attr_keys this is game-side
// declaration (AGENTS rule 1): core/ and the runtime treat the cooked action
// strings as opaque. Adding an action means adding it here + its test.
//
// Grammar is the same dotted-identifier shape as attr keys (segments
// [a-z][a-z0-9_]* joined by single '.').
#include <array>
#include <string_view>

#include "ve/game/attr_keys.h"  // is_valid_attr_key doubles as the grammar check

namespace ve::game {

// Every ratified Phase-5 facet action. Fixture + PLAN examples covered;
// "use.door" pairs with door.* components, "airlock.cycle" with airlock.*.
inline constexpr std::array<std::string_view, 12> kPhase5_facet_actions = {
    "airlock.cycle",   "door.toggle",     "mount.socket",   "operate.station",
    "port.undock",     "repair.target",   "trade.counter",  "use.berth",
    "use.counter",     "use.door",        "use.panel",      "use.seat",
};

inline constexpr bool is_valid_facet_action(std::string_view action) {
    if (!is_valid_attr_key(action)) return false;  // grammar first
    for (const auto a : kPhase5_facet_actions)
        if (a == action) return true;
    return false;
}

} // namespace ve::game
