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
// uniformly, which is what makes the #46a name pairing total. Section
// boxes, facets and seams are decoded for structural soundness but not yet
// carried in the loadout model (interior graph -> 5.11+, seams -> station
// docking later). WIRES (6.12) are now carried in loaded_hull::wires so the
// WiringReach BFS runs over the DEF0 network verbatim.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "ve/core/cooked.h"
#include "ve/core/sim/loadout.h"
#include "ve/core/sim/wiring.h"

namespace ve::core::sim {

// One hull's decoded runtime data.
struct loaded_hull {
    Loadout loadout;
    std::string mesh_unit_id;   // SOCK's content-ID link to the MESH unit
    std::vector<wire_entry> wires;  // authored DEF0 wire network (6.12 BFS)
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

}  // namespace ve::core::sim
