// Docked service gates + walk-over junction validity (checklist 7.11):
// one tag predicate + one graph-edge pressure rule, pure functions, no
// stored state — replay hashes ride every gate flip exactly like the
// clamp itself.

#include "ve/core/sim/docked.h"

#include <algorithm>

namespace ve::core::sim {

namespace {

bool has_tag(const std::vector<std::string>& tags, std::string_view tag) {
    return std::ranges::find(tags, tag) != tags.end();
}

const section_def* find_section(const section_graph& graph,
                                std::string_view id) {
    const auto it = std::ranges::find(graph.sections, id, &section_def::id);
    return it == graph.sections.end() ? nullptr : &*it;
}

} // namespace

service_check station_service_check(std::optional<clamped_pair> pair,
                                    const Loadout& station,
                                    std::string_view service_facet,
                                    std::string_view dark_tag) {
    if (!pair.has_value()) return {.available = false, .reason = "undocked", .counter_id = {}};

    // Deterministic: loadout mount order (ordered container, sim rule 5).
    // First facet-bearing counter wins — authored data, not a hash map.
    for (const auto& mc : station.mounts()) {
        if (!has_tag(mc.tags, service_facet)) continue;
        if (has_tag(mc.tags, dark_tag)) {
            return {.available = false, .reason = "counter dark",
                    .counter_id = mc.instance_id};
        }
        return {.available = true, .reason = "", .counter_id = mc.instance_id};
    }
    return {.available = false, .reason = "no counter", .counter_id = {}};
}

std::optional<walk_junction> mated_junction(
    const Loadout& ship, const Loadout& /*station*/,
    const section_graph& ship_graph, const section_graph& station_graph,
    std::string_view airlock_section, std::string_view port_section) {
    // Both ends must exist: the ship must actually carry the airlock
    // facet, and BOTH named sections must be authored in their hull's
    // walk graph. An unauthored side = no junction (the clamp still
    // holds; there is just nothing to walk through).
    bool has_airlock = false;
    for (const auto& mc : ship.mounts())
        if (has_tag(mc.tags, "airlock")) { has_airlock = true; break; }
    if (!has_airlock) return std::nullopt;
    if (!find_section(ship_graph, airlock_section)) return std::nullopt;
    if (!find_section(station_graph, port_section)) return std::nullopt;
    return walk_junction{.ship_section = airlock_section,
                         .station_section = port_section};
}

bool junction_walkable(const walk_junction& junction,
                       std::span<const section_state> ship_sections,
                       std::span<const section_state> station_sections) {
    if (junction.ship_section.empty() || junction.station_section.empty())
        return false;

    const auto holds = [](std::span<const section_state> secs,
                          std::string_view id) {
        const auto it = std::ranges::find(secs, id, &section_state::section_id);
        // Fail closed: no air report, no green light.
        if (it == secs.end()) return false;
        return !it->venting && !it->empty;
    };
    return holds(ship_sections, junction.ship_section) &&
           holds(station_sections, junction.station_section);
}

} // namespace ve::core::sim
