#pragma once
// Phase 9 tasks 9.5a/9.5 — shared wire-route legality (#51). ONE implementation
// of the route TEXT GRAMMAR used by every route front-end (def-forms panel,
// loadout editor route mode), so a draft that commits in one front-end has
// the same shape legality and the same error strings everywhere. (9.5 adds
// the spatial law here too — the cook G12 gate becomes a wrapper over it,
// byte-identical strings, per the repo precedent for shared grammar.)
//
// Grammar (same flat-integer form the 8.2 forms use for composite fields):
// a route draft is comma-joined integers forming whole [x,y,z] triples.
// TOML bracket decoration ("[[0, 0, 0], [1, 0, 0]]") is tolerated so the
// field can be prefilled straight from the authored line.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ve::game {

using route_cell = std::array<std::int32_t, 3>;

// true = well-formed draft. On false, msg carries the shared error string:
//   "cell coord '<tok>' is not an integer"
//   "route needs whole [x,y,z] cells (3 coords each)"
bool route_text_ok(std::string_view text, std::string& msg);

// Parse a well-formed draft into cells (coords are int32; out-of-int32
// values are a grammar failure with the '<tok> is not an integer' message).
bool parse_route_text(std::string_view text, std::vector<route_cell>& route, std::string& msg);

// Canonical flat-text form for a route (what the editor prefills).
std::string route_text_of(const std::vector<route_cell>& route);

// ---- spatial law (#51 G12a-d) --------------------------------------------
// ONE implementation, consumed by the cook validator gate AND the loadout
// editor route mode (9.5): a route legal in the editor is exactly a route
// that passes cook, and the refuse reason is byte-identical in both places.
// (Precedent: the same "one gate fn" doctrine as 5.9b loadout assemble and
// the #42 unit table shared with cook.) Views mirror the DEF0 shapes so the
// validator maps them mechanically.
struct route_box { std::array<std::int32_t, 6> bounds; };  // lo3 then hi3, DEF0 order
struct route_section { std::string name; std::vector<route_box> boxes; };
struct route_door_seam { route_cell a{}, b{}; };           // (cell, cell+face) pair
struct route_wire {
    std::string id;
    route_cell from_cell{}, to_cell{};  // endpoint ANCHOR cells (resolver's job)
    std::vector<route_cell> route;
};

// "" = legal. Otherwise the G12 failure message, byte-identical to the cook
// gate's (#51 G12a endpoints, G12b adjacency, G12c lattice membership,
// G12d door seams — checked in that order per wire, pinned by tests).
std::string route_law_error(const std::vector<route_section>& sections,
                            const std::vector<route_door_seam>& seams,
                            const route_wire& wire);

} // namespace ve::game
