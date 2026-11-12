#pragma once

// Docked service gates + walk-over check (PLAN #23b/c, checklist 7.11):
// "Docked" is ONE physics state — the #23 clamp from 7.10 — that opens the
// whole suite (dock repair, propellant refill, trade, crew/loadout) via
// pure TAG checks on loadout components. No menu mode, no new state kind:
// a service is available exactly when (a) the ship is clamped at a port
// whose host hull hosts a counter component carrying the service facet,
// and (b) the counter itself is up — dark-not-dead (Phase 6): a destroyed
// counter or a component shedding its facet closes the door, and repair
// re-opens it by restoring the facet.
//
// Walk-over (#23c): the ship's `airlock` component mates into the station
// section beside the clamping port. The junction is an EDGE of the walk
// graph over the authored section maps (#19a/#27c: sections are nodes,
// doors/airlocks are edges), and pressure is the section's reservoir
// (Phase 6 primitive): the junction holds while BOTH sides' adjacent
// sections hold pressure (not empty, not venting). A breached section
// closes the edge just like a closed door — one rule, one primitive.
// Actual walking is Phase 10; this task asserts edge presence + validity
// under pressure rules, nothing else.
//
// Same discipline as dock.h: pure functions of (loadouts, clamped-pair
// snapshot, sections), no stored state, so replay hashes ride every
// gate flip. Service/facet/section names are opaque strings — the concept
// vocabulary (trade.repair, atmo.venting…) is cooked game data (#19:
// "oxygen" is never core's).

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ve/core/sim/loadout.h"
#include "ve/core/sim/tags.h"

namespace ve::core::sim {

// One authored section of a hull's section map (#19a). `door_edges` are
// neighbor section indices (the graph edges the doors form); airlock
// components name their own section via their mount (see walk_junction).
struct section_def {
    std::string id;                       // opaque section name
    std::vector<std::uint32_t> door_edges; // neighbor sections (graph edges)
};

// Authored walk graph of one hull: sections + their door edges (#27c).
struct section_graph {
    std::vector<section_def> sections;
};

// The clamped pair snapshot at one tick: WHICH port of the station the
// ship's clamp engaged (indices into the respective loadouts' mount
// lists — deterministic identity, like every other loadout ref). A free
// ship has no pair; callers pass no snapshot then.
struct clamped_pair {
    std::uint32_t ship_port_index = 0;    // ship's dock.gear mount index
    std::uint32_t station_port_index = 0; // station port mount index
};

// A docked service request resolved against the clamped pair + authored
// hulls (availability only — the verb EXECUTES as a fixed-tick bus event
// through the Phase 6 machinery: repair = wiring restore, refuel =
// reservoir refill, trade = market leg; 7.11 owns the gate, not the verb).
struct service_check {
    bool available = false;
    std::string_view reason;     // opaque; "" when available
    std::string_view counter_id; // counter component instance, if found
};

// The gate is ONE tag predicate on the STATION side — "the whole suite
// keys off the clamped constraint" (#23b) means the caller supplies the
// clamp fact (or nothing, for a free ship); the predicate itself is
// station loadout + facet tag:
//   no clamp snapshot  -> "undocked"
//   no component on the station carrying `service_facet` -> "no counter"
//   facet-bearing component also carries the dark tag      -> "counter dark"
//   facet-bearing component present + not dark             -> available
// Dark-not-dead is literally a dropped facet (Phase 6 idiom), so the
// dark tag rides the SAME tags vector the facet lives in — no new state.
service_check station_service_check(std::optional<clamped_pair> pair,
                                    const Loadout& station,
                                                                      std::string_view service_facet,
                                    std::string_view dark_tag);

// --- walk-over ------------------------------------------------------------

// Junction pressure facts for one authored section (from the Phase 6
// reservoir + band tags): `venting` = section carries the venting band
// tag (#19c), `empty` = reservoir crossed the vacuum threshold.
struct section_state {
    std::string_view section_id;
    bool venting = false;
    bool empty = false;
};

// The mated airlock junction (#23c): the ship's airlock component opens
// onto `ship_section`; the station section `station_section` sits beside
// the port the ship clamped at. The walk graph edge EXISTS iff both
// hulls authored those sections; it is WALKABLE iff the junction holds
// pressure — both adjacent sections not empty and not venting (the
// junction is itself a section boundary: if either side breathes out,
// the junction breathes out — the same venting rule covers a breached
// junction, no special case).
struct walk_junction {
    std::string_view ship_section;    // ship airlock's section, "" if unauthored
    std::string_view station_section; // station section beside the port
};

// The authored junction pair for a clamped ship: which section the
// ship's airlock pressurizes (`airlock_section`) and which section the
// station port sits in (`port_section`). Those pairings are authored
// data — this phase the test/caller supplies them directly, Phase 8's
// cooklet emits the same pairs from section geometry (#19a: "how the
// sections are connected" is cooked data). nullopt when the ship has
// no airlock-facet component or either named section is unauthored in
// its hull's graph — no junction to assert (the clamp still holds;
// you just can't walk over).
std::optional<walk_junction> mated_junction(
    const Loadout& ship, const Loadout& station,
    const section_graph& ship_graph, const section_graph& station_graph,
    std::string_view airlock_section, std::string_view port_section);

// Is the junction walkable right now? Compares the authored junction
// against the live section states of BOTH hulls (looked up by id;
// missing state = the section holds pressure = walkable-by-default is
// NOT the rule — a named section with no state reported fails closed:
// no air data, no green light).
bool junction_walkable(const walk_junction& junction,
                       std::span<const section_state> ship_sections,
                       std::span<const section_state> station_sections);

} // namespace ve::core::sim
