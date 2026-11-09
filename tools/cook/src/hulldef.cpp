// Phase 5 task 5.8 — hull def schema parse + DEF0 v1 payload emission.
// Schema (PLAN Phase 5; decisions #14/#18/#19a/#21/#26a/#28a/#31a/#42/#44):
//   class_tag          required for a hull def; "class.<x>" tag (#31a).
//   hull_mass          plain number (kg)                -> optional override (#14)
//   hull_com           3 unit strings ("0.4 m")         -> optional
//   hull_inertia_diag  3 plain numbers (kg*m^2)         -> optional
//   [grid]             cell/cell_height/tolerance unit strings, subcell_ratio
//                      integer >= 1 (#44: every hull declares its lattice).
//   [[section]]        name + cells = flat [lo.x lo.y lo.z hi.x hi.y hi.z]
//                      boxes, CELL coords (#19a).
//   [[component]]      id/type/category/socket; optional tags, [component.attrs]
//                      (registry keys, #42 coercion), [[component.facet]]
//                      (action + range + optional manual) (#28a).
//   [[wire]]           id/type power|signal|power+signal, optional capacity,
//                      from/to = "socket.<id>" | "component.<id>" (#26a).
//   [[seam]]           partner def ref + shared socket + optional offset
//                      (#46d; partner joins the manifest dep graph).
//   facet.target       optional str — repair.target cross-ref (#28a).
//   refs               top-level string array (manifest deps, existing doctrine).
// Determinism: every emitted sequence is ordered — components/wires/refs in
// authored array order, attrs in std::map key order, catalog first-seen.
#include "ve/cook/hulldef.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ve/game/attr_keys.h>

#include "ve/cook/toml.h"

namespace ve::cook {
namespace {

constexpr std::uint32_t kPayloadVersion = 2;  // DEF0 v2: facet target + seams (cooked.h layout)

// LE byte writer — same byte doctrine as the SOCK/MESH payloads (cooked.h).
struct Writer {
    Bytes b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f64(double v) {
        unsigned char t[8];
        std::memcpy(t, &v, 8);
        for (int i = 0; i < 8; ++i) b.push_back(t[i]);
    }
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
};

const TomlValue* at(const TomlTable& t, std::string_view key) {
    const auto it = t.find(std::string(key));
    return it == t.end() ? nullptr : &it->second;
}

bool fail(std::string& err, const std::string& msg) { err = msg; return false; }

bool req_str(const TomlTable& t, std::string_view key, const std::string& where,
             std::string& outv, std::string& err) {
    const TomlValue* v = at(t, key);
    if (!v) return fail(err, "missing '" + where + std::string(key) + "'");
    if (!v->is_string()) return fail(err, "'" + where + std::string(key) + "' must be a string");
    if (v->str.empty()) return fail(err, "'" + where + std::string(key) + "' must not be empty");
    outv = v->str;
    return true;
}

// Plain SI number (kg, N, W, dimensionless): a bare TOML number, never a
// suffixed string (#42 scope: only length/time/angle fields take suffixes).
bool req_num(const TomlValue& v, const std::string& path, double& outv, std::string& err) {
    if (!v.is_number()) return fail(err, "'" + path + "' must be a plain SI number (kg/N/W/dimensionless)");
    outv = v.num();
    if (!std::isfinite(outv)) return fail(err, "'" + path + "' must be finite");
    return true;
}

// Unit-suffixed string coerced to SI at cook time (#42).
bool req_unit(const TomlValue& v, const std::string& path, double& outv, std::string& err) {
    if (!v.is_string())
        return fail(err, "'" + path + "' must be a unit-suffixed string (\"5 m\")");
    const auto si = si_from_string(v.str);
    if (!si) return fail(err, "'" + path + "': " + si.error());
    outv = *si;
    return true;
}

// Ratified Phase-5 categories; encoding matches core `socket_category`
// (main=0, rcs=1, gun=2, utility=3) so the 5.10 loader can cast through.
bool category_u8(const std::string& s, std::uint8_t& outv) {
    if (s == "main") outv = 0;
    else if (s == "rcs") outv = 1;
    else if (s == "gun") outv = 2;
    else if (s == "utility") outv = 3;
    else return false;
    return true;
}

bool req_string_array(const TomlValue& v, const std::string& path,
                      std::vector<std::string>& outv, std::string& err) {
    if (v.kind != TomlValue::kArray) return fail(err, "'" + path + "' must be an array of strings");
    for (std::size_t i = 0; i < v.array.size(); ++i) {
        if (!v.array[i].is_string() || v.array[i].str.empty())
            return fail(err, "'" + path + "[" + std::to_string(i) + "]' must be a non-empty string");
        outv.push_back(v.array[i].str);
    }
    return true;
}

bool parse_refs(const TomlTable& rt, std::vector<std::string>& refs_out, std::string& err) {
    if (const TomlValue* r = at(rt, "refs"))
        return req_string_array(*r, "refs", refs_out, err);
    return true;
}

// One [[section]] row: name + cells = [[lo,hi],...] (6 ints per box, lo<=hi).
bool parse_section(const TomlValue& row, std::string& name_out,
                   std::vector<std::array<std::int32_t, 6>>& boxes_out, std::string& err) {
    if (!row.is_table()) return fail(err, "section row must be a table");
    const auto& t = row.table;
    if (!req_str(t, "name", "section.", name_out, err)) return false;
    const TomlValue* cells = at(t, "cells");
    if (!cells) return fail(err, "section '" + name_out + "' missing 'cells'");
    if (cells->kind != TomlValue::kArray || cells->array.empty())
        return fail(err, "section '" + name_out + "' cells must be a non-empty array of [lo,hi] boxes");
    for (const auto& box : cells->array) {
        if (box.kind != TomlValue::kArray || box.array.size() != 6)
            return fail(err, "section '" + name_out + "' box must be [lo3, hi3] of 6 integers");
        std::array<std::int32_t, 6> b{};
        for (std::size_t i = 0; i < 6; ++i) {
            if (box.array[i].kind != TomlValue::kInteger)
                return fail(err, "section '" + name_out + "' box coords must be integers (cell coords)");
            b[i] = static_cast<std::int32_t>(box.array[i].integer);
        }
        for (int ax = 0; ax < 3; ++ax)
            if (b[ax] > b[ax + 3])
                return fail(err, "section '" + name_out + "' box has lo > hi on axis " + std::to_string(ax));
        boxes_out.push_back(b);
    }
    return true;
}

struct Facet {
    std::string action;
    double range_m = 0.0;
    std::uint8_t manual = 0;
    // #28a cross-ref target: a `repair.target` facet names a real wire /
    // junction id. Existence is cross-validated at 5.9b; the string rides
    // here (DEF0 payload v2).
    std::uint8_t has_target = 0;
    std::string target;
};
struct Attr {
    std::string key;
    double value = 0.0;
};
struct CompRow {
    std::string id, type, socket;
    std::uint8_t category = 0;
    std::vector<std::string> tags;
    std::vector<Attr> attrs;   // std::map key order (deterministic)
    std::vector<Facet> facets;
};

bool parse_component(const TomlValue& row, const std::string& where, CompRow& c, std::string& err) {
    if (!row.is_table()) return fail(err, where + " must be a table");
    const auto& t = row.table;
    if (!req_str(t, "id", where + ".", c.id, err)) return false;
    if (!req_str(t, "type", where + ".", c.type, err)) return false;
    std::string cat;
    if (!req_str(t, "category", where + ".", cat, err)) return false;
    if (!category_u8(cat, c.category))
        return fail(err, where + ".category '" + cat + "' not one of main/rcs/gun/utility");
    if (!req_str(t, "socket", where + ".", c.socket, err)) return false;
    if (const TomlValue* tags = at(t, "tags"))
        if (!req_string_array(*tags, where + ".tags", c.tags, err)) return false;

    if (const TomlValue* attrs = at(t, "attrs")) {
        if (!attrs->is_table()) return fail(err, where + ".attrs must be a table");
        // TOML dotted keys nest ([component.attrs] + `power.draw = 1` creates
        // a `power` sub-table). The attr registry uses flat dotted names, so
        // flatten nested tables to dot-joined keys before validating. std::map
        // over the joined names keeps emission order deterministic (key order).
        std::vector<std::pair<std::string, const TomlValue*>> flat;
        std::function<void(const TomlTable&, const std::string&)> walk =
            [&](const TomlTable& tbl, const std::string& prefix) {
                for (const auto& [k, v] : tbl) {
                    const std::string key = prefix.empty() ? k : prefix + "." + k;
                    if (v.is_table()) walk(v.table, key);
                    else flat.emplace_back(key, &v);
                }
            };
        walk(attrs->table, "");
        for (const auto& [key, val] : flat) {
            if (!ve::game::is_valid_attr_key(key))
                return fail(err, where + ".attrs key '" + key + "' fails grammar");
            const bool known = std::any_of(ve::game::kPhase5_attr_keys.begin(),
                                           ve::game::kPhase5_attr_keys.end(),
                                           [&](std::string_view k) { return k == key; });
            if (!known)
                return fail(err, where + ".attrs key '" + key + "' not in the ratified attr-key registry");
            const std::string path = where + ".attrs." + key;
            double v = 0.0;
            if (val->is_number()) {
                if (!req_num(*val, path, v, err)) return false;
            } else if (!req_unit(*val, path, v, err)) {  // string form: unit-suffixed only (#42)
                return false;
            }
            c.attrs.push_back({key, v});
        }
    }
    if (const TomlValue* facets = at(t, "facet")) {
        if (facets->kind != TomlValue::kArray)
            return fail(err, where + ".facet must be an array ([[component.facet]])");
        for (std::size_t i = 0; i < facets->array.size(); ++i) {
            const auto& f = facets->array[i];
            const std::string fpath = where + ".facet[" + std::to_string(i) + "]";
            if (!f.is_table()) return fail(err, fpath + " must be a table");
            Facet fac;
            if (!req_str(f.table, "action", fpath + ".", fac.action, err)) return false;
            const TomlValue* range = at(f.table, "range");
            if (!range) return fail(err, fpath + " missing 'range'");
            if (!req_unit(*range, fpath + ".range", fac.range_m, err)) return false;
            if (const TomlValue* man = at(f.table, "manual")) {
                if (man->kind != TomlValue::kBoolean)
                    return fail(err, fpath + ".manual must be a boolean");
                fac.manual = man->boolean ? 1 : 0;
            }
            if (const TomlValue* tgt = at(f.table, "target")) {
                if (!tgt->is_string())
                    return fail(err, fpath + ".target must be a string (wire/junction id, #28a)");
                fac.has_target = 1;
                fac.target = tgt->str;
            }
            c.facets.push_back(std::move(fac));
        }
    }
    return true;
}

struct SeamRow {
    std::string partner;  // partner def ref (also pushed into refs)
    std::string socket;   // own shared socket name
    std::uint8_t has_offset = 0;
    std::array<double, 3> off{0, 0, 0};  // module transform offset (SI m, #42)
};
struct WireRow {
    std::string id;
    std::uint8_t type = 0;  // 0 power, 1 signal, 2 power+signal
    bool has_capacity = false;
    double capacity = 0.0;
    std::uint8_t from_kind = 0, to_kind = 0;  // 0 socket, 1 component
    std::string from_ref, to_ref;
};

// "socket.<id>" / "component.<id>" endpoint grammar (#26a).
bool parse_endpoint(const TomlTable& t, std::string_view key, const std::string& where,
                    std::uint8_t& kind_out, std::string& ref_out, std::string& err) {
    std::string s;
    if (!req_str(t, key, where, s, err)) return false;
    constexpr std::string_view kSock = "socket.";
    constexpr std::string_view kComp = "component.";
    if (s.rfind(kSock, 0) == 0 && s.size() > kSock.size()) {
        kind_out = 0;
        ref_out = s.substr(kSock.size());
    } else if (s.rfind(kComp, 0) == 0 && s.size() > kComp.size()) {
        kind_out = 1;
        ref_out = s.substr(kComp.size());
    } else {
        return fail(err, std::string(where) + std::string(key) +
                             " '" + s + "' must be \"socket.<id>\" or \"component.<id>\"");
    }
    return true;
}

bool parse_wire(const TomlValue& row, const std::string& where, WireRow& w, std::string& err) {
    if (!row.is_table()) return fail(err, where + " must be a table");
    const auto& t = row.table;
    if (!req_str(t, "id", where + ".", w.id, err)) return false;
    std::string type;
    if (!req_str(t, "type", where + ".", type, err)) return false;
    if (type == "power") w.type = 0;
    else if (type == "signal") w.type = 1;
    else if (type == "power+signal") w.type = 2;
    else return fail(err, where + ".type '" + type + "' not one of power/signal/power+signal");
    if (const TomlValue* cap = at(t, "capacity")) {
        if (!req_num(*cap, where + ".capacity", w.capacity, err)) return false;
        if (w.capacity <= 0.0) return fail(err, where + ".capacity must be > 0");
        w.has_capacity = true;
    }
    if (!parse_endpoint(t, "from", where + ".", w.from_kind, w.from_ref, err)) return false;
    if (!parse_endpoint(t, "to", where + ".", w.to_kind, w.to_ref, err)) return false;
    return true;
}

}  // namespace

bool cook_hulldef_unit(const std::string& toml_text, Bytes& payload_out,
                       std::vector<std::string>& refs_out, std::string* err) {
    std::string local;
    std::string& e = err ? *err : local;
    TomlValue root;
    if (!parse_toml(toml_text, root, &e)) return false;
    const TomlTable& rt = root.table;
    refs_out.clear();
    if (!parse_refs(rt, refs_out, e)) return false;

    const TomlValue* tag = at(rt, "class_tag");
    if (!tag) {  // not a hull def: raw passthrough (non-hull defs keep their bytes)
        payload_out = Bytes(toml_text.begin(), toml_text.end());
        return true;
    }

    // ---- hull-def schema ----
    std::string class_tag;
    if (!req_str(rt, "class_tag", "", class_tag, e)) return false;
    if (class_tag.rfind("class.", 0) != 0 || class_tag.size() <= 6)
        return fail(e, "class_tag '" + class_tag + "' must be \"class.<name>\"");

    double hull_mass = 0.0;
    std::uint8_t has_mass = 0;
    if (const TomlValue* m = at(rt, "hull_mass")) {
        if (!req_num(*m, "hull_mass", hull_mass, e)) return false;
        if (hull_mass <= 0.0) return fail(e, "hull_mass must be > 0");
        has_mass = 1;
    }
    std::array<double, 3> com{0, 0, 0};
    std::uint8_t has_com = 0;
    if (const TomlValue* c = at(rt, "hull_com")) {
        if (c->kind != TomlValue::kArray || c->array.size() != 3)
            return fail(e, "hull_com must be an array of 3 unit strings");
        for (int i = 0; i < 3; ++i)
            if (!req_unit(c->array[i], "hull_com[" + std::to_string(i) + "]", com[i], e)) return false;
        has_com = 1;
    }
    std::array<double, 3> inertia{0, 0, 0};
    std::uint8_t has_inertia = 0;
    if (const TomlValue* iv = at(rt, "hull_inertia_diag")) {
        if (iv->kind != TomlValue::kArray || iv->array.size() != 3)
            return fail(e, "hull_inertia_diag must be an array of 3 numbers (kg*m^2)");
        for (int i = 0; i < 3; ++i)
            if (!req_num(iv->array[i], "hull_inertia_diag[" + std::to_string(i) + "]", inertia[i], e)) return false;
        has_inertia = 1;
    }

    const TomlValue* grid = at(rt, "grid");
    if (!grid || !grid->is_table()) return fail(e, "hull def requires a [grid] block (#44)");
    const TomlValue* cell = at(grid->table, "cell");
    const TomlValue* ratio = at(grid->table, "subcell_ratio");
    const TomlValue* height = at(grid->table, "cell_height");
    const TomlValue* tol = at(grid->table, "tolerance");
    if (!cell) return fail(e, "grid.cell missing");
    if (!ratio) return fail(e, "grid.subcell_ratio missing");
    if (!height) return fail(e, "grid.cell_height missing");
    if (!tol) return fail(e, "grid.tolerance missing");
    double cell_m = 0.0, height_m = 0.0, tol_m = 0.0;
    if (!req_unit(*cell, "grid.cell", cell_m, e)) return false;
    if (!req_unit(*height, "grid.cell_height", height_m, e)) return false;
    if (!req_unit(*tol, "grid.tolerance", tol_m, e)) return false;
    if (cell_m <= 0.0 || height_m <= 0.0 || tol_m < 0.0)
        return fail(e, "grid.cell/cell_height must be > 0, tolerance >= 0");
    if (ratio->kind != TomlValue::kInteger || ratio->integer < 1)
        return fail(e, "grid.subcell_ratio must be an integer >= 1");
    const auto ratio_u = static_cast<std::uint32_t>(ratio->integer);

    // sections (#19a)
    std::vector<std::pair<std::string, std::vector<std::array<std::int32_t, 6>>>> sections;
    if (const TomlValue* secs = at(rt, "section")) {
        if (secs->kind != TomlValue::kArray) return fail(e, "sections must be [[section]] rows");
        for (std::size_t i = 0; i < secs->array.size(); ++i) {
            std::string name;
            std::vector<std::array<std::int32_t, 6>> boxes;
            if (!parse_section(secs->array[i], name, boxes, e)) return false;
            for (const auto& s : sections)
                if (s.first == name) return fail(e, "duplicate section name '" + name + "'");
            sections.emplace_back(name, std::move(boxes));
        }
    }

    // components + catalog (#18/#21/#28a)
    std::vector<CompRow> comps;
    std::vector<std::pair<std::string, std::uint8_t>> catalog;  // type -> category, first-seen
    if (const TomlValue* cs = at(rt, "component")) {
        if (cs->kind != TomlValue::kArray) return fail(e, "components must be [[component]] rows");
        for (std::size_t i = 0; i < cs->array.size(); ++i) {
            const std::string where = "component[" + std::to_string(i) + "]";
            CompRow c;
            if (!parse_component(cs->array[i], where, c, e)) return false;
            for (const auto& o : comps)
                if (o.id == c.id) return fail(e, "duplicate component id '" + c.id + "'");
            bool conflict_seen = false;
            for (const auto& [type, cat] : catalog) {
                if (type != c.type) continue;
                conflict_seen = true;
                if (cat != c.category)
                    return fail(e, "component type '" + c.type +
                                     "' declared with conflicting categories");
            }
            if (!conflict_seen) catalog.emplace_back(c.type, c.category);
            comps.push_back(std::move(c));
        }
    }

    // wiring (#26a)
    std::vector<WireRow> wires;
    if (const TomlValue* ws = at(rt, "wire")) {
        if (ws->kind != TomlValue::kArray) return fail(e, "wires must be [[wire]] rows");
        for (std::size_t i = 0; i < ws->array.size(); ++i) {
            const std::string where = "wire[" + std::to_string(i) + "]";
            WireRow w;
            if (!parse_wire(ws->array[i], where, w, e)) return false;
            for (const auto& o : wires)
                if (o.id == w.id) return fail(e, "duplicate wire id '" + w.id + "'");
            wires.push_back(std::move(w));
        }
    }

    // seams (#46d): minimal station-mating declarations; geometry/identity
    // cross-validation rides 5.9b, the lattice-compat rule is the cook gate.
    std::vector<SeamRow> seams;
    if (const TomlValue* ss = at(rt, "seam")) {
        if (ss->kind != TomlValue::kArray) return fail(e, "seams must be [[seam]] rows");
        for (std::size_t i = 0; i < ss->array.size(); ++i) {
            const std::string where = "seam[" + std::to_string(i) + "]";
            const TomlValue& row = ss->array[i];
            if (!row.is_table()) return fail(e, where + " must be a table");
            SeamRow s;
            if (!req_str(row.table, "partner", where + ".", s.partner, e)) return false;
            if (!req_str(row.table, "socket", where + ".", s.socket, e)) return false;
            if (const TomlValue* off = at(row.table, "offset")) {
                if (off->kind != TomlValue::kArray || off->array.size() != 3)
                    return fail(e, where + ".offset must be an array of 3 unit strings");
                for (int k = 0; k < 3; ++k)
                    if (!req_unit(off->array[k], where + ".offset[" + std::to_string(k) + "]", s.off[k], e))
                        return false;
                s.has_offset = 1;
            }
            seams.push_back(std::move(s));
        }
        // The partner def is a dependency like any ref — the manifest edge
        // makes 5.9b's both-sides check order-independent. Duplicates fold.
        for (const auto& s : seams) {
            if (std::find(refs_out.begin(), refs_out.end(), s.partner) == refs_out.end())
                refs_out.push_back(s.partner);
        }
    }

    // ---- emit DEF0 v2 (layout pinned in ve/core/cooked.h) ----
    Writer w;
    w.u32(kPayloadVersion);
    w.str(class_tag);
    w.u8(has_mass);
    if (has_mass) w.f64(hull_mass);
    w.u8(has_com);
    if (has_com) for (double v : com) w.f64(v);
    w.u8(has_inertia);
    if (has_inertia) for (double v : inertia) w.f64(v);
    w.f64(cell_m);
    w.u32(ratio_u);
    w.f64(height_m);
    w.f64(tol_m);
    w.u32(static_cast<std::uint32_t>(sections.size()));
    for (const auto& [name, boxes] : sections) {
        w.str(name);
        w.u32(static_cast<std::uint32_t>(boxes.size()));
        for (const auto& box : boxes) for (std::int32_t v : box) w.i32(v);
    }
    w.u32(static_cast<std::uint32_t>(comps.size()));
    for (const auto& c : comps) {
        w.str(c.id);
        w.str(c.type);
        w.u8(c.category);
        w.str(c.socket);
        w.u32(static_cast<std::uint32_t>(c.tags.size()));
        for (const auto& t : c.tags) w.str(t);
        w.u32(static_cast<std::uint32_t>(c.attrs.size()));
        for (const auto& a : c.attrs) { w.str(a.key); w.f64(a.value); }
        w.u32(static_cast<std::uint32_t>(c.facets.size()));
        for (const auto& f : c.facets) {
            w.str(f.action); w.f64(f.range_m); w.u8(f.manual);
            w.u8(f.has_target);
            if (f.has_target) w.str(f.target);
        }
    }
    w.u32(static_cast<std::uint32_t>(catalog.size()));
    for (const auto& [type, cat] : catalog) { w.str(type); w.u8(cat); }
    w.u32(static_cast<std::uint32_t>(wires.size()));
    for (const auto& wire : wires) {
        w.str(wire.id);
        w.u8(wire.type);
        w.u8(wire.has_capacity ? 1 : 0);
        if (wire.has_capacity) w.f64(wire.capacity);
        w.u8(wire.from_kind);
        w.str(wire.from_ref);
        w.u8(wire.to_kind);
        w.str(wire.to_ref);
    }
    w.u32(static_cast<std::uint32_t>(seams.size()));
    for (const auto& s : seams) {
        w.str(s.partner);
        w.str(s.socket);
        w.u8(s.has_offset);
        if (s.has_offset) for (double v : s.off) w.f64(v);
    }
    w.u32(static_cast<std::uint32_t>(refs_out.size()));
    for (const auto& r : refs_out) w.str(r);

    payload_out = std::move(w.b);
    return true;
}

}  // namespace ve::cook
