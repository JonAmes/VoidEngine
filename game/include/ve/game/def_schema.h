#pragma once
// Phase 8 task 8.1 — def-schema registry (#43 mode-C forms / #48 editor kit).
//
// What every def kind's fields ARE, as data: field path, value kind, arity,
// requiredness, unit dim (unit_str fields), and enum set (enumeration
// fields). The 8.2 forms panel walks this to render typed fields; the 8.1
// entry gate (#42 on entry) reads each field's dim. This is deliberately
// NOT a second source of truth: the closed value sets (categories, faces,
// wire types, facet actions, attr keys) live here game-side and the
// cooklet's hulldef validation indexes INTO these tables for its cooked
// encodings — one spelling set, two consumers. Field lists mirror
// tools/cook/src/hulldef.cpp's parser exactly; keep the two in lock-step
// (a hull def parsed by the cooklet must validate through the same schema
// the forms use).
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ve/game/attr_keys.h"       // attr-key registry (candidate picker)
#include "ve/game/facet_actions.h"   // facet-action registry
#include "ve/game/units.h"           // unit_dim

namespace ve::game {

// ---- closed enum sets (index == cooked u8 encoding) ----
// Socket categories: order matches core `socket_category` (main=0, rcs=1,
// gun=2, utility=3) so the loader casts through (5.10).
inline constexpr std::array<std::string_view, 4> kCategory_names = {
    "main", "rcs", "gun", "utility"};
// #46c1 axis spelling -> face code = index + 1 (0 = absent). Engine axes,
// #34 RH Y-up -Z fwd.
inline constexpr std::array<std::string_view, 6> kFace_axis_names = {
    "+x", "-x", "+y", "-y", "+z", "-z"};
// Wire type codes: 0 power, 1 signal, 2 power+signal.
inline constexpr std::array<std::string_view, 3> kWire_type_names = {
    "power", "signal", "power+signal"};

// Enumeration fields name their set; empty span = unknown set (forms bug).
constexpr std::span<const std::string_view> enum_options(std::string_view set) {
    if (set == "category") return kCategory_names;
    if (set == "face") return kFace_axis_names;
    if (set == "wire_type") return kWire_type_names;
    if (set == "facet_action") return kPhase5_facet_actions;
    return {};
}

// ---- schema shape ----
enum class field_kind : std::uint8_t {
    text,        // non-empty free string (component.type)
    id,          // non-empty identifier string (unique within the def)
    tag,         // dotted-grammar string ("class.<name>", component tags)
    unit_str,    // "NUMBER SUFFIX" string (#42); `dim` gates at entry
    number_si,   // bare TOML number, SI or dimensionless (#42 scope)
    integer,     // bare integer
    boolean,     // TOML bool
    enumeration, // one of enum_options(enum_set)
    endpoint,    // "socket.<id>" | "component.<id>" (#26a) — picker only
    name_ref,    // id inside THIS def (seam.socket, facet.target) — picker only
    def_path,    // other-def relative path (seam.partner, refs[]) — picker only
    tag_list,    // inline array of tag strings (component.tags)
    path_list,   // inline array of def paths (top-level refs)
    int_boxes,   // flat [lo0,lo1,lo2,hi0,hi1,hi2] integer box (section.cells)
    attr_map,    // registry-key -> number map ([component.attrs])
};

struct def_field {
    std::string_view path;                // relative to the def/row root
    field_kind kind;
    std::uint8_t arity = 1;               // fixed tuple width (hull_com/offset = 3); 0 = variable
    bool required = true;
    std::optional<unit_dim> dim;          // unit_str only
    std::string_view enum_set;            // enumeration only
};
// Row collections ([[section]], [[component]], nested [[component.facet]] as
// "component.facet", [[wire]], [[seam]]). Rows with a dotted name nest under
// the parent row's tables.
struct def_row { std::string_view row; std::span<const def_field> fields; };
struct def_schema {
    std::string_view def_kind;
    std::span<const def_field> scalars;
    std::span<const def_row> rows;
};

// ---- HULL def schema (mirrors tools/cook/src/hulldef.cpp exactly) ----
inline constexpr std::array kHull_scalars = {
    def_field{"class_tag", field_kind::tag, 1, true, std::nullopt, {}},
    def_field{"hull_mass", field_kind::number_si, 1, false, std::nullopt, {}},
    def_field{"hull_com", field_kind::unit_str, 3, false, unit_dim::length, {}},
    def_field{"hull_inertia_diag", field_kind::number_si, 3, false, std::nullopt, {}},
    def_field{"grid.cell", field_kind::unit_str, 1, true, unit_dim::length, {}},
    def_field{"grid.cell_height", field_kind::unit_str, 1, true, unit_dim::length, {}},
    def_field{"grid.tolerance", field_kind::unit_str, 1, true, unit_dim::length, {}},
    def_field{"grid.subcell_ratio", field_kind::integer, 1, true, std::nullopt, {}},
    def_field{"refs", field_kind::path_list, 0, false, std::nullopt, {}},
};
inline constexpr std::array kSection_fields = {
    def_field{"name", field_kind::id, 1, true, std::nullopt, {}},
    def_field{"cells", field_kind::int_boxes, 0, true, std::nullopt, {}},
};
inline constexpr std::array kComponent_fields = {
    def_field{"id", field_kind::id, 1, true, std::nullopt, {}},
    def_field{"type", field_kind::text, 1, true, std::nullopt, {}},
    def_field{"category", field_kind::enumeration, 1, true, std::nullopt, "category"},
    def_field{"socket", field_kind::name_ref, 1, true, std::nullopt, {}},
    def_field{"face", field_kind::enumeration, 1, false, std::nullopt, "face"},
    def_field{"tags", field_kind::tag_list, 0, false, std::nullopt, {}},
    def_field{"attrs", field_kind::attr_map, 0, false, std::nullopt, {}},
    // facet fields ride the nested "component.facet" row below.
};
inline constexpr std::array kFacet_fields = {
    def_field{"action", field_kind::enumeration, 1, true, std::nullopt, "facet_action"},
    def_field{"range", field_kind::unit_str, 1, true, unit_dim::length, {}},
    def_field{"manual", field_kind::boolean, 1, false, std::nullopt, {}},
    def_field{"target", field_kind::name_ref, 1, false, std::nullopt, {}},
};
inline constexpr std::array kWire_fields = {
    def_field{"id", field_kind::id, 1, true, std::nullopt, {}},
    def_field{"type", field_kind::enumeration, 1, true, std::nullopt, "wire_type"},
    def_field{"capacity", field_kind::number_si, 1, false, std::nullopt, {}},
    def_field{"from", field_kind::endpoint, 1, true, std::nullopt, {}},
    def_field{"to", field_kind::endpoint, 1, true, std::nullopt, {}},
};
inline constexpr std::array kSeam_fields = {
    def_field{"partner", field_kind::def_path, 1, true, std::nullopt, {}},
    def_field{"socket", field_kind::name_ref, 1, true, std::nullopt, {}},
    def_field{"offset", field_kind::unit_str, 3, false, unit_dim::length, {}},
};
inline constexpr std::array kHull_rows = {
    def_row{"section", kSection_fields},
    def_row{"component", kComponent_fields},
    def_row{"component.facet", kFacet_fields},
    def_row{"wire", kWire_fields},
    def_row{"seam", kSeam_fields},
};
inline constexpr def_schema kHull_def_schema{"hull", kHull_scalars, kHull_rows};

}  // namespace ve::game
