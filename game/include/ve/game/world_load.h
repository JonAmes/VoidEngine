#pragma once
// Phase 8 task 8.7 (decision #45d): the WORLD0 cooked payload codec — the
// slice map's authored data as normalized binary (decision #13: the runtime
// never parses TOML). Lives in ve::game, not core: the payload speaks game
// concepts (market, faction, lane, build plan — AGENTS.md rule 1), while the
// rail/terrain/well/atmosphere numbers map 1:1 onto core::sim structs
// (rail_world::body_def, terrain_def, terrain_stamp), so later Phase 8 boxes
// can assemble a rail_world straight out of a decoded body def.
//
// ONE codec shared by cook and runtime (the DEF0 precedent): the cooklet
// (tools/cook/src/worlddef.cpp) parses the TOML schema into these structs,
// calls encode_world(), and the manifest edges come from the ref strings;
// the runtime calls decode_world(). worlddef_cook pins re-encode == encode
// byte-for-byte, so the two directions can never drift silently.
//
// Units (#42): every length/time/angle SCALAR in the TOML schema is a
// "NUMBER SUFFIX" string coerced to SI at cook time — the structs and the
// payload only ever hold SI (m, s, rad, and rad/s spin). Position triples
// and rates (flow kg/s, power/s, scatter density/km^2, price-curve inputs)
// stay bare SI numbers, same rule as hull attrs.
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/cooked.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/terrain.h"

namespace ve::game {

enum class WorldClass : std::uint8_t { system = 0, body = 1, station = 2 };

// POI kinds (#29c): the authored named surface features of the slice.
enum class PoiKind : std::uint8_t { pad = 0, outpost = 1, landmark = 2 };

// Nav-graph node/edge kinds (#27a).
enum class NavKind : std::uint8_t { lane = 0, patrol = 1, local = 2 };

// An authored stamp riding a POI, plus the POI's own footprint so the cook
// can prove the stamp covers it (8.8 gate).
struct WorldPoi {
    std::string name;
    PoiKind kind = PoiKind::pad;
    core::sim::terrain_stamp stamp;   // center_dir + angular radius etc.
    double footprint_m = 0.0;         // structure radius the stamp must cover
};

struct WorldBiomeBand {
    double lo_m = 0.0;                // height above mean radius, meters
    double hi_m = 0.0;
    double color[3] = {1.0, 1.0, 1.0};
    double roughness = 0.8;
    double scatter_density = 0.0;     // bare rate: scatter items per km^2
};

// A build-plan step (#24b): a prefab module hull def flown to the station
// and clamped onto target_socket at the authored scheduled time.
struct WorldBuildStep {
    std::string module_def_ref;       // -> hull def (manifest dep)
    std::string target_socket;        // station hull's socket id
    double at_time_s = 0.0;           // authored timeline ("0.5 day" -> s)
};

struct WorldBuildPlan {
    std::string id;
    std::vector<WorldBuildStep> steps;
};

struct WorldFaction {
    std::string name;
    double power_production = 0.0;    // bare rate: faction.power supply /s (#25c)
    std::vector<std::string> tags;    // stance.hostile.<faction> etc. (#25b)
};

struct WorldMarketCommodity {
    std::string commodity;            // commodity id (cooked tags)
    double production = 0.0;          // bare rate: supply kg/s (#20b)
};

struct WorldMarketNode {
    std::string id;                   // node id (lane/nav endpoints key on this)
    std::string anchor_ref;           // body/station def ref or "poi:<name>"
    std::vector<WorldMarketCommodity> commodities;
};

struct WorldLane {                         // #20b authored trade lane
    std::string id;
    std::string from, to;                  // node ids (8.8 resolves)
    std::string commodity;
    double flow_rate = 0.0;                // bare rate: baseline NPC flow kg/s
};

struct WorldNavNode {                      // #27a waypoint/lane/patrol anchor
    std::string id;
    NavKind kind = NavKind::lane;
    std::string body_ref;                  // '' = fixed point in system frame
    double offset[3] = {0.0, 0.0, 0.0};    // bare SI m (rel. body center)
};

struct WorldNavEdge {
    NavKind kind = NavKind::lane;
    std::string from, to;                  // nav node ids
};

struct WorldJobStep {                      // #33a director template step
    std::string verb;                      // undock/cargo_run/dock/purchase...
    std::string target_ref;                // node/station id ('' = none)
    double amount = 0.0;                   // bare payload (kg, credits...)
};

struct WorldJobTemplate {
    std::string id;
    std::vector<WorldJobStep> steps;
};

// One decoded world def: the class byte says which sections are populated
// (decode rejects cross-class sections, so a re-encode reproduces the exact
// bytes — the round-trip pin in tests/cook/test_worlddef.cpp).
struct WorldDef {
    WorldClass world_class = WorldClass::system;
    std::string name;

    // ---- body (class body): rail + well + atmosphere + terrain ----
    // rails/well/atmos ride as the sim's own struct — the payload maps 1:1
    // onto rail_world::body_def (parent_ref resolves to an index only at
    // world assembly; here it stays a def-ref string).
    std::string parent_ref;                // '' = static root anchor (#45c)
    core::sim::body_def rails;             // well + atmos + orbit + spin, SI
    // Safe bubble impact magnitude (#50c, 8.13): the FLAT damage points the
    // glue applies (negative flat_add on hull, damage-as-data) once per
    // bubble-edge crossing, at the first contact tick. Game-side value —
    // the core body_def carries the wall (radius + safe_speed) but never
    // a damage number. 0 = authored "no impact damage on crossing".
    double safe_impact = 0.0;
    core::sim::terrain_def terrain;
    std::vector<core::sim::terrain_stamp> stamps;   // unnamed art stamps
    std::vector<WorldPoi> pois;
    std::vector<WorldBiomeBand> biomes;
    std::vector<std::string> tags;         // owner.<faction> (#25a)

    // ---- station (class station): loadout entity pinned to a rail ----
    std::string hull_def_ref;              // -> hull def (manifest dep, #24a)
    std::string root_socket;               // '' = identity pin
    std::vector<WorldBuildPlan> build_plans;   // #24b

    // ---- system (class system): the map root ----
    std::uint64_t seed = 0;
    std::vector<std::string> members;      // ordered body/station def refs
    std::vector<WorldFaction> factions;
    std::vector<WorldMarketNode> markets;
    std::vector<WorldLane> lanes;
    std::vector<WorldNavNode> waypoints;
    std::vector<WorldNavEdge> nav_edges;
    std::vector<WorldJobTemplate> job_templates;

    // Ref strings that must become manifest dep edges (members, parents,
    // hull defs, build-plan modules). The cook resolves them with the same
    // resolve_ref loop the DEF0 path uses.
    std::vector<std::string> collect_refs() const;
};

// WorldClass <-> the TOML world_class spelling.
std::expected<WorldClass, std::string> world_class_of(std::string_view text);
const char* world_class_name(WorldClass c);

// ---- WORLD0 payload codec (layout documented in core/cooked.h) ----
core::cooked::Bytes encode_world(const WorldDef& def);
std::expected<WorldDef, std::string> decode_world(const core::cooked::Bytes& payload);

// Convenience for world assembly (used from 8.12 on): the decoded body's
// rail_world::body_def, with parent left unresolved (-1; assembly patches
// the index once def refs become body ids).
core::sim::body_def sim_body_def(const WorldDef& body);

}  // namespace ve::game
