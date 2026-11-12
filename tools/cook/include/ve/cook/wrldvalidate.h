#pragma once
// Phase 8 task 8.8 — world-def cross-field / cross-file validation gates.
// The cooklet runs these after the 5.9b hull cross-validation pass, over
// touched WRLD units in manifest (content-ID) order. Gate order is fixed
// per class — identical inputs produce identical first-error text
// (cook determinism, like the manifest graph and cook_hull_validate):
//   body:    rail parent chain bottoms at the root anchor, no cycle (#45c);
//            POI stamp linear radius >= footprint (#29c);
//            pad POIs ride a full-weight flatten/mesa stamp, which makes
//            the pad plane parallel to the well normal by proof, not
//            tolerance (full-weight interior is bit-constant height,
//            terrain.h derivation).
//   system:  members resolve and cooked as world defs; member rail parents
//            are members of this system (#39); owner tags name defined
//            factions (#25a); stance tags name defined factions, not self
//            (#25b); market anchors resolve to a member or a member POI
//            (#45d/#29c); lane endpoints name market nodes, flow > 0, and
//            the lane's commodity is produced at its from-market (#20b);
//            waypoint bodies are members, nav-edge endpoints name market
//            or waypoint nodes (#27a); job-step targets name a node, lane
//            or member station (#33a).
//   station: parent chain (#45c); hull def cooked as a hull (#24a); root
//            socket is a hardpoint on it; build-plan modules cooked as
//            hulls, their clamp seam pairs the step socket with THIS
//            station hull (#24b — the #46d seam gates in hullvalidate.cpp
//            already proved both-sides hardpoint + lattice + symmetry,
//            this gate binds the PLAN onto those pairs), steps ordered at
//            time > 0, no two steps on one socket.
// The airlock-pairing rules themselves (#46c facing, #46d seam rules) are
// per-hull gates that already run on every cooked DEF0 — including station
// hulls and plan modules — so 8.8 composes with them instead of re-checking.
#include <functional>
#include <map>
#include <string>

#include "ve/cook/hullvalidate.h"   // HullInputs (decoded DEF0 + SOCK)
#include "ve/game/world_load.h"     // WorldDef

namespace ve::cook {

// Resolve an authored ref string from `from_id` to a content id. Same
// contract as cook.cpp's resolve_ref ("" out = unresolvable).
using RefResolve = std::function<bool(const std::string& from_id,
                                      const std::string& ref,
                                      std::string& id_out)>;

// self_id: content ID of the WRLD unit being gated. wrld_deps: every world
// def reachable from self (members, parent chains) already decoded; hulls:
// every hull def reachable from self (station hull + plan modules) already
// loaded. Missing deps are gates, not crashes: they produce the first
// failure message. On failure *err carries "world '<id>': ...".
bool cook_wrld_validate(const std::string& self_id,
                        const ve::game::WorldDef& self,
                        const std::map<std::string, ve::game::WorldDef>& wrld_deps,
                        const std::map<std::string, HullInputs>& hulls,
                        const RefResolve& resolve,
                        std::string* err);

} // namespace ve::cook
