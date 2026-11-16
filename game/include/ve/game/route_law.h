#pragma once
// Phase 9 task 9.5a — shared wire-route legality (#51a). ONE implementation
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

} // namespace ve::game
