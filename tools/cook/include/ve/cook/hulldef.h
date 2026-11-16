#pragma once
// Phase 5 task 5.8 — hull def TOML -> normalized binary DEF0 payload.
// A .toml def that declares a top-level `class_tag` is a HULL DEF and is
// compiled into the DEF0 v1 binary documented in ve/core/cooked.h; every
// unit-suffixed field is coerced to SI at cook time (#42), so the runtime
// never sees TOML or suffixes. A def WITHOUT class_tag keeps its raw source
// bytes (non-hull defs get their schema in a later phase). Refs are
// extracted either way; the caller resolves them to manifest deps.
// Phase 9 task 9.1 (#52a, user decision #55): a .toml def that declares a
// top-level `component_type` is a component TYPE def — the shared part
// identity (category + art refs + [attrs] preset) that hull [[component]]
// entries inherit and override key-by-key.
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ve/cook/cook.h"  // Bytes

namespace ve::cook {

// A parsed component TYPE def (#55): category code matches the entry
// `category` encoding (def_schema kCategory_names index); attrs preset in
// flat key order; refs are authored paths (caller resolves to deps; may be
// empty until the 9.2 strict art gate).
struct CompTypeDef {
    std::string type;                                     // type id (entry `type` matches)
    std::uint8_t category = 0;                            // category code
    std::vector<std::string> refs;                        // art refs, authored order
    std::vector<std::pair<std::string, double>> attrs;    // preset (#42 gate at this end too)
};

// True when the root table carries `component_type` (marker-detection
// doctrine mirrors toml_source_has_world_class; parse failure = false).
bool toml_source_has_component_type(const std::string& toml_text);

// Parse + schema-validate a component type def (#55 grammar: component_type +
// category required, refs optional string array, [attrs] preset table; a def
// carrying two of class_tag/world_class/component_type is cook-fail). No
// payload emission — the unit keeps its raw source bytes; the merge resolves
// into hull DEF0 payloads via cook_hulldef_unit below.
bool cook_comptype_source(const std::string& toml_text, CompTypeDef& out, std::string* err);

// World TRS of a socket/empty anchor (9.2 #52b): emitted per mounted
// component as the DEF0 v3 mount transform. The resolved model's keyed
// node carries the offset from it.
struct MountTRS {
    std::array<double, 3> t{{0, 0, 0}};
    std::array<double, 4> q{{0, 0, 0, 1}};
};

// One hull entry's art request (#52b): the driver resolves candidates to
// art asset content IDs (entry OWN refs against the hull's dir, else the
// type def's refs against the type def's dir) and hands cook_hulldef_unit
// the picked asset per component id. own_refs empty = inherit type def.
struct CompArtRequest {
    std::string id, type;
    std::vector<std::string> own_refs;
};

// Parse just enough of a hull def to list its component art requests
// (same grammar as cook_hulldef_unit; non-hull defs -> empty, true).
bool cook_hulldef_comp_requests(const std::string& toml_text,
                                std::vector<CompArtRequest>& out, std::string* err);

// Parse + schema-validate + normalize. On false, *err carries the reason.
// payload_out: DEF0 v1 binary (hull def) or raw source bytes (plain def).
// refs_out: top-level `refs = [...]` strings in authored order (may be empty).
// #52a/#55 resolution (all optional, defaults keep 5.8 behavior): when
// type_defs is non-null, an entry whose `type` has a type def gets its
// category cross-checked (entry category must equal the type def's — cook
// fail on mismatch, one truth per type) and its attrs REPLACED by the merged
// map (preset ∪ entry, the explicit entry key always wins; emitted in
// std::map key order like every attrs list). Entries whose type has no type
// def keep entry-only attrs. used_types_out collects the type ids of matched
// def collects the type ids of matched entries (authored order, may repeat);
// entry_refs_out collects each entry's OWN `refs` override paths (full-list
// replace, #55b) for the driver's manifest dep edges — inherited type-def
// refs ride the type def unit's own edges (authored against ITS directory),
// reached transitively via the hull -> type-def edge.
// 9.2 (#52b) DEF0 v3: art_by_comp maps component id -> the resolved art
// asset content ID whose model is keyed by that component's id/type
// (absent/'' = unresolved; validator gate G11 cooks it out); anchors maps
// anchor name -> world TRS (from the hull's cooked SOCK deps) supplying
// the per-component mount transform. Both optional; null emits art_ref ''
// and has_mount 0.
bool cook_hulldef_unit(const std::string& toml_text, Bytes& payload_out,
                       std::vector<std::string>& refs_out, std::string* err,
                       const std::map<std::string, CompTypeDef>* type_defs = nullptr,
                       std::vector<std::string>* used_types_out = nullptr,
                       std::vector<std::string>* entry_refs_out = nullptr,
                       const std::map<std::string, std::string>* art_by_comp = nullptr,
                       const std::map<std::string, MountTRS>* anchors = nullptr);

}  // namespace ve::cook
