#pragma once
// Phase 8 task 8.7 (decision #45d): the world-def TOML schema, cook side.
// A .toml source whose root table carries `world_class = "system" | "body" |
// "station"` cooks to a WORLD unit (WRLD payload, layout in core/cooked.h);
// every other .toml keeps the 5.8 hull-def path. Schema gates run in FIXED
// ORDER and report the first failure only (worlddef_gate_order pins that
// order) — same doctrine as the hull-def validator.
//
// Schema summary (all length/time/angle scalars are #42 "NUMBER SUFFIX"
// strings coerced to SI; triples and rates are bare SI numbers, the hull
// attr rule):
//   shared: world_class, name
//   body:    parent (def ref, absent = root anchor #45c), radius, well_*,
//            atm_*; orbit_period/orbit_radius/orbit_phase/spin_*;
//            [terrain] seed/relief/base_frequency/octaves/lacunarity/gain;
//            [[stamp]] type flatten|crater|mesa + center + radius/level/blend;
//            [[poi]] name + kind pad|outpost|landmark + stamp fields +
//            footprint; [[biome]] lo/hi/color/roughness/scatter; tags.
//   station: hull_def (def ref, #24a), parent + orbit fields, root_socket,
//            [[build_step]] plan/module/socket/at (#24b; flat rows — a step
//            is a row keyed by its plan id, no nested AOTs); tags.
//   system:  seed, members (ordered def refs), [[faction]] name/power/stance,
//            [[market]] id/anchor, [[production]] market/commodity/rate,
//            [[lane]] id/from/to/commodity/flow, [[waypoint]] id/kind/body/at,
//            [[edge]] kind/from/to (#27a), [[job]] id + [[job_step]]
//            job/verb/target/amount (#33a).
// Manifest edges cover STRUCTURAL containment only (members, parent,
// hull_def, build-plan modules); name cross-refs (market anchors, lane and
// edge endpoints, waypoint bodies, owner tags) are 8.8's resolution gates.
#include <cstdint>
#include <string>
#include <vector>

#include <ve/core/cooked.h>

namespace ve::cook {

// Cheap routing sniff: parses and reports whether the root table declares
// world_class. A TOML parse failure returns false (the hull-def path then
// reports the historical parse error — routing never swallows it).
bool toml_source_has_world_class(const std::string& toml_text);

// Parse + schema-gate one world-def source into the WRLD payload.
// refs_out receives the def refs that become manifest dep edges. False +
// actionable first-error message otherwise.
bool cook_worlddef_source(const std::string& toml_text, core::cooked::Bytes& payload_out,
                          std::vector<std::string>& refs_out, std::string* err);

}  // namespace ve::cook
