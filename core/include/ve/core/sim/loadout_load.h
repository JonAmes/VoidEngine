#pragma once
// Phase 5 task 5.10 — the runtime load path: cooked hull units
// (MESH + SOCK + DEF0; MAT/TEX ride the manifest deps of the MESH) into a
// validated Loadout. The loader is concept-FREE (AGENTS rule 1): it decodes
// ids, category bytes, attribute keys and numbers exactly as cooked — it
// never interprets a type id, an attr key, a section name, or a facet
// action. Cook-time validation (5.9b) is the author-time line of defense;
// Loadout::assemble is the runtime one, so corrupt or hand-massaged units
// still cannot produce a broken loadout.
//
// Every named anchor (SOCK v2, kind 0 hardpoints AND kind 1..5 interior
// empties) becomes a loadout socket: def components mount onto anchors
// uniformly, which is what makes the #46a name pairing total. Seams are
// decoded for structural soundness but not yet carried (station docking
// later). WIRES (6.12) are carried in loaded_hull::wires so the WiringReach
// BFS runs over the DEF0 network verbatim. Since 9.13 the section->wires
// index (#51b, cooked by 9.3's route gates) and the #28a facet rows are
// carried too: they are the damage/interaction query data — a hull hit
// severs exactly the wires cooked into the hit section's row, and a
// component's facets gate which repairs are legal at it. Still concept-FREE:
// section names, facet actions and targets stay opaque strings.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "ve/core/cooked.h"
#include "ve/core/sim/cargo.h"
#include "ve/core/sim/loadout.h"
#include "ve/core/sim/wiring.h"

namespace ve::core::sim {

// #28a facet row exactly as cooked (action/range/manual/target stay opaque;
// cook validated target ids exist — 5.9b).
struct facet_desc {
    std::string action;
    double range_m = 0.0;
    bool manual = false;
    std::string target;  // '' unless the facet declares a target
};

// Facets of one def component, authored order.
struct component_facets {
    std::string instance_id;
    std::vector<facet_desc> facets;
};

// Cook-derived #51b damage index: section name -> wire-table indices
// (ascending) of every wire whose routed cells pass through the section.
// Rows are in authored-section order, one row per section (empty row =
// nothing routed there).
struct section_wires_row {
    std::string section;
    std::vector<std::uint32_t> wire_indices;
};

// One hull's decoded runtime data.
struct loaded_hull {
    Loadout loadout;
    std::string mesh_unit_id;   // SOCK's content-ID link to the MESH unit
    std::vector<wire_entry> wires;  // authored DEF0 wire network (6.12 BFS)
    std::vector<section_wires_row> section_wires;  // #51b sever query (9.13)
    std::vector<component_facets> facets;          // #28a interaction data
};

// Decode a DEF0-v2 + SOCK-v2 unit pair and assemble the loadout.
// mesh_content_id: manifest content id of the MESH unit (the caller
// resolves refs -> ids; the loader never opens files or the manifest).
// Errors are deterministic strings naming the first violation: a payload
// corruption ("DEF0 payload: ...", "SOCK payload: ...") or the assemble
// verdict ("loadout: <first loadout_error kind>").
std::expected<loaded_hull, std::string> load_hull_loadout(
    const ve::core::cooked::Unit& def0,
    const ve::core::cooked::Unit& sock,
    std::uint64_t mesh_content_id);

// ---- #51b damage tie-in (9.13): the cooked section index drives the cut.
// Concept-FREE glue: "hit this section row" -> sever exactly the wires
// cooked there via the shipped WiringReach::sever_wire; downstream dark
// cascade is 6.12 machinery, zero new net logic.

// Wire-table indices routed through a section ('' if the section is
// unknown). Returned pointer is into the hull's own rows.
const std::vector<std::uint32_t>* wires_in_section(
    const loaded_hull& hull, std::string_view section);

// Sever every wire in the section's row (indices in row order = ascending
// wire-table order, deterministic). Returns the ids ACTUALLY cut (a wire
// already severed or dangling counts as not-cut). Unknown section -> empty.
std::vector<std::string> sever_wires_in_section(
    WiringReach& net, const loaded_hull& hull, std::string_view section);

// Repair at a facet: legal only when host_component carries a facet with
// action == facet_action naming wire_id as target (the cook gate 5.9b
// proved the target is a real wire id at author time), and consumes one
// spare from the manifest. A repair with nothing severed, an unknown wire,
// an absent facet, or an unknown spare consumes NOTHING (checked before
// the net mutates; the spare removal then cannot fail).
enum class wire_repair_error {
    no_facet,       // host does not declare that facet naming the wire
    unknown_wire,   // not in the wire table, or currently not severed
    unknown_spare,  // spare instance not in the manifest
};
std::expected<void, wire_repair_error> repair_wire_at_facet(
    WiringReach& net, CargoManifest& cargo, const loaded_hull& hull,
    std::string_view facet_action, std::string_view host_component,
    std::string_view wire_id, std::string_view spare_instance_id);

}  // namespace ve::core::sim
