#pragma once
// Cargo tag vocabulary (#20a) — game-side, mirrors role_tags.h/facet_actions.h:
// these names are cooked/rostered data; the core cargo machinery is handed
// them and never spells them itself.
//
//   hold_tag     marks a mounted component as a cargo hold;
//   gate_prefix  prefixes the mount tags that gate commodity types — the tag
//                suffix IS the commodity id (cooked market data, #20b);
//   capacity     comes from the kAttr_cargo_capacity attr key (attr_keys.h).

#include <string_view>

namespace ve::game {

inline constexpr std::string_view kTag_cargo_hold = "cargo.hold";
inline constexpr std::string_view kPrefix_commodity_gate = "commodity.";

// Gate tags follow the dotted-identifier grammar like every other tag.
inline constexpr bool is_commodity_gate_tag(std::string_view tag) {
    return tag.size() > kPrefix_commodity_gate.size() &&
           tag.compare(0, kPrefix_commodity_gate.size(), kPrefix_commodity_gate) == 0;
}

}  // namespace ve::game
