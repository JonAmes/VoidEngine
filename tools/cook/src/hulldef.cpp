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
//   [[component]]      id/type/category/socket; optional tags, refs (#55b art
//                      override), [component.attrs] (registry keys, #42
//                      coercion), [[component.facet]] (action + range +
//                      optional manual) (#28a). Entries whose `type` matches a
//                      component TYPE def inherit its category + [attrs]
//                      preset, overridden key-by-key (#52a/#55).
//   [[wire]]           id/type power|signal|power+signal, optional capacity,
//                      required route = ordered [[x,y,z],...] cell path (#51),
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
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <ve/game/attr_keys.h>

#include "ve/cook/toml.h"
#include "ve/game/def_schema.h"
#include "ve/game/facet_actions.h"

namespace ve::cook {
namespace {

constexpr std::uint32_t kPayloadVersion = 4;  // DEF0 v4: per-wire route + section->wires index (cooked.h layout, #51); v3 added per-component art_ref + mount TRS (#52b)

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
    void f32(float v) {
        unsigned char t[4];
        std::memcpy(t, &v, 4);
        for (int i = 0; i < 4; ++i) b.push_back(t[i]);
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
// 8.1: spellings come from the game-side schema table (def_schema.h) —
// the forms panel and the cooklet share one enumeration source.
bool category_u8(const std::string& s, std::uint8_t& outv) {
    for (std::size_t i = 0; i < ve::game::kCategory_names.size(); ++i)
        if (s == ve::game::kCategory_names[i]) { outv = static_cast<std::uint8_t>(i); return true; }
    return false;
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
    // #46c1 explicit facing for door.*/airlock.* components: 0 = absent,
    // 1 +x, 2 -x, 3 +y, 4 -y, 5 +z, 6 -z (engine axes, #34). Required (cook
    // fail at 5.9b validation) on those types; meaningless elsewhere.
    std::uint8_t face = 0;
    std::vector<std::string> tags;
    // #55b: entry-level art refs; when non-empty they FULLY REPLACE the type
    // def's refs (art override; key-by-key override applies to attrs only).
    std::vector<std::string> refs;
    std::vector<Attr> attrs;   // std::map key order (deterministic)
    std::vector<Facet> facets;
};

// #46c1 axis spelling -> face code (engine axes, #34). Spellings from the
// game-side schema table (8.1); code = index + 1.
bool face_u8(std::string_view s, std::uint8_t& out) {
    for (std::size_t i = 0; i < ve::game::kFace_axis_names.size(); ++i)
        if (s == ve::game::kFace_axis_names[i]) { out = static_cast<std::uint8_t>(i + 1); return true; }
    return false;
}

// [attrs] map flattened + #42-gated — shared by hull [[component.attrs]]
// entries AND component TYPE def presets (9.1: "the #42 entry gate operates
// at both def kinds", #52a). `prefix` is the def-relative path without a
// trailing dot ("component[0].attrs" / "attrs"); hull-end messages stay
// byte-identical to the 5.8 entry gate.
bool parse_attrs_map(const TomlValue& attrs, const std::string& prefix,
                     std::vector<Attr>& out, std::string& err) {
    if (!attrs.is_table()) return fail(err, prefix + " must be a table");
    // TOML dotted keys nest ([component.attrs] + `power.draw = 1` creates
    // a `power` sub-table). The attr registry uses flat dotted names, so
    // flatten nested tables to dot-joined keys before validating. std::map
    // over the joined names keeps emission order deterministic (key order).
    std::vector<std::pair<std::string, const TomlValue*>> flat;
    std::function<void(const TomlTable&, const std::string&)> walk =
        [&](const TomlTable& tbl, const std::string& prefix2) {
            for (const auto& [k, v] : tbl) {
                const std::string key = prefix2.empty() ? k : prefix2 + "." + k;
                if (v.is_table()) walk(v.table, key);
                else flat.emplace_back(key, &v);
            }
        };
    walk(attrs.table, "");
    for (const auto& [key, val] : flat) {
        if (!ve::game::is_valid_attr_key(key))
            return fail(err, prefix + " key '" + key + "' fails grammar");
        const bool known = std::any_of(ve::game::kPhase5_attr_keys.begin(),
                                       ve::game::kPhase5_attr_keys.end(),
                                       [&](std::string_view k) { return k == key; });
        if (!known)
            return fail(err, prefix + " key '" + key + "' not in the ratified attr-key registry");
        const std::string path = prefix + "." + key;
        double v = 0.0;
        if (val->is_number()) {
            if (!req_num(*val, path, v, err)) return false;
        } else if (!req_unit(*val, path, v, err)) {  // string form: unit-suffixed only (#42)
            return false;
        }
        out.push_back({key, v});
    }
    return true;
}

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
    if (const TomlValue* fv = at(t, "face")) {  // #46c1 (door/airlock facing)
        if (!fv->is_string())
            return fail(err, where + ".face must be a string (\"+x\"/\"-x\"/\"+y\"/\"-y\"/\"+z\"/\"-z\")");
        if (!face_u8(fv->str, c.face))
            return fail(err, where + ".face '" + fv->str + "' not one of +x/-x/+y/-y/+z/-z");
    }
    if (const TomlValue* tags = at(t, "tags"))
        if (!req_string_array(*tags, where + ".tags", c.tags, err)) return false;
    if (const TomlValue* refs = at(t, "refs"))  // #55b art override
        if (!req_string_array(*refs, where + ".refs", c.refs, err)) return false;

    if (const TomlValue* attrs = at(t, "attrs"))
        if (!parse_attrs_map(*attrs, where + ".attrs", c.attrs, err)) return false;
    if (const TomlValue* facets = at(t, "facet")) {
        if (facets->kind != TomlValue::kArray)
            return fail(err, where + ".facet must be an array ([[component.facet]])");
        for (std::size_t i = 0; i < facets->array.size(); ++i) {
            const auto& f = facets->array[i];
            const std::string fpath = where + ".facet[" + std::to_string(i) + "]";
            if (!f.is_table()) return fail(err, fpath + " must be a table");
            Facet fac;
            if (!req_str(f.table, "action", fpath + ".", fac.action, err)) return false;
            // #28a: unknown action tags are rejected cook-time; the ratified
            // closed set lives game-side (kPhase5_facet_actions).
            if (!ve::game::is_valid_facet_action(fac.action))
                return fail(err, fpath + ".action '" + fac.action +
                                 "' not in the ratified facet-action registry (#28a)");
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
    std::vector<std::array<std::int32_t, 3>> route;  // ordered #44 cell path (#51)
};

// #51 wire route grammar: `route = [[x,y,z], ...]` — an ordered chain of
// CELL lattice points (integer cell coords, same kind gate as section
// boxes). Spatial law (endpoint attach, adjacency, lattice membership,
// door-seam-only boundary crossing) is validated at gate G12.
bool parse_route(const TomlTable& t, const std::string& where,
                 std::vector<std::array<std::int32_t, 3>>& route, std::string& err) {
    const TomlValue* r = at(t, "route");
    if (!r)
        return fail(err, where + "route is required (#51): an ordered path of cell coords");
    if (r->kind != TomlValue::kArray || r->array.empty())
        return fail(err, where + "route must be a non-empty array of [x,y,z] cells (#51)");
    for (std::size_t i = 0; i < r->array.size(); ++i) {
        const TomlValue& c = r->array[i];
        if (c.kind != TomlValue::kArray || c.array.size() != 3)
            return fail(err, where + "route[" + std::to_string(i) +
                                 "] must be [x,y,z] of 3 integers (cell coords)");
        std::array<std::int32_t, 3> cell{};
        for (int ax = 0; ax < 3; ++ax) {
            if (c.array[ax].kind != TomlValue::kInteger)
                return fail(err, where + "route cell coords must be integers (cell coords)");
            cell[ax] = static_cast<std::int32_t>(c.array[ax].integer);
        }
        route.push_back(cell);
    }
    return true;
}

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
    // 8.1: wire-type spellings from the game-side schema table; code = index.
    bool known_wire = false;
    for (std::size_t i = 0; i < ve::game::kWire_type_names.size(); ++i)
        if (type == ve::game::kWire_type_names[i]) { w.type = static_cast<std::uint8_t>(i); known_wire = true; break; }
    if (!known_wire)
        return fail(err, where + ".type '" + type + "' not one of power/signal/power+signal");
    if (const TomlValue* cap = at(t, "capacity")) {
        if (!req_num(*cap, where + ".capacity", w.capacity, err)) return false;
        if (w.capacity <= 0.0) return fail(err, where + ".capacity must be > 0");
        w.has_capacity = true;
    }
    if (!parse_endpoint(t, "from", where + ".", w.from_kind, w.from_ref, err)) return false;
    if (!parse_endpoint(t, "to", where + ".", w.to_kind, w.to_ref, err)) return false;
    if (!parse_route(t, where + ".", w.route, err)) return false;
    return true;
}

}  // namespace

// ---- #51c: cook-generated wire tube mesh (wire visuals) -----------------
// A thin-tube MESH payload (layout pinned in ve/core/cooked.h) per hull:
// ONE submesh per wire, name = wire id — the mount key the render side uses
// to draw each tube in its reachability colour ("dark not dead", #51c).
// No MAT0 material (0xFFFFFFFF = none), no textures, no new passes or deps.
// Geometry: a 6-sided prism swept along the route polyline; polyline points
// are CELL CENTRES in hull-local metres — cell (x,y,z) maps to
// ((x+0.5)*cell_m, (y+0.5)*height_m, (z+0.5)*cell_m), keeping the tube off
// the wall planes (G12d lets legal crossings pierce a face at its centre).
// Routes are 6-adjacent chains only AFTER gate G12, which runs later, so
// the builder must not trust adjacency: segment frames come from the
// delta's DOMINANT axis with exact integer axes and a literal ring cos/sin
// table — no libm anywhere, so payload bytes are bit-identical on every
// machine (determinism doctrine). A doomed def may cook a garbage tube
// shape; the gate still cooks it out. Tri winding: ring k is CCW around the
// outgoing tangent t with frame (u, b=t×u, u×b=t); quad tris (a,b,c)+(a,c,d)
// face outward — the cooklet owns culling (MESH layout comment).
constexpr float kTubeRadius = 0.05f;  // cosmetic thin conduit (#51c)
constexpr int kTubeSides = 6;
constexpr float kRingC[kTubeSides] = {1.f, .5f, -.5f, -1.f, -.5f, .5f};
constexpr float kRingS[kTubeSides] = {0.f, .8660254f, .8660254f,
                                      0.f, -.8660254f, -.8660254f};

void emit_wire_tubes(const std::vector<WireRow>& wires, double cell_m,
                     double height_m, Bytes& out) {
    Writer wr;
    wr.u32(static_cast<std::uint32_t>(wires.size()));
    for (const auto& wire : wires) {
        wr.str(wire.id);
        wr.u32(0xFFFFFFFFu);  // material_index: none (#51c)
        const auto n = static_cast<std::uint32_t>(wire.route.size());
        wr.u32(n * kTubeSides);
        // MESH layout is PLANAR channel blocks (pos|nrm|uv|tan), not
        // interleaved — collect vertices in ring-major order, emit channels.
        std::vector<float> pos, nrm, uvs, tan;
        pos.reserve(n * kTubeSides * 3);
        nrm.reserve(n * kTubeSides * 3);
        uvs.reserve(n * kTubeSides * 2);
        tan.reserve(n * kTubeSides * 4);
        float dist = 0.f;  // uv.u = cumulative path length (metres)
        std::array<int, 3> prev{1, 0, 0};  // last ring repeats last frame
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto& cell = wire.route[i];
            std::array<int, 3> d{0, 0, 0};
            if (i + 1 < n) {
                const auto& nx = wire.route[i + 1];
                d = {nx[0] - cell[0], nx[1] - cell[1], nx[2] - cell[2]};
                prev = d;
            } else {
                d = prev;
            }
            // dominant-axis frame: t on the step axis, u fixed per axis,
            // b = t×u. All exact ints; sign of the step rides into t.
            int axis = 0;
            if (std::abs(d[1]) > std::abs(d[axis])) axis = 1;
            if (std::abs(d[2]) > std::abs(d[axis])) axis = 2;
            std::array<float, 3> t{0.f, 0.f, 0.f}, u{0.f, 0.f, 0.f};
            t[axis] = d[axis] < 0 ? -1.f : 1.f;
            if (axis == 0) u[1] = 1.f; else u[0] = 1.f;
            const std::array<float, 3> b{t[1] * u[2] - t[2] * u[1],
                                         t[2] * u[0] - t[0] * u[2],
                                         t[0] * u[1] - t[1] * u[0]};
            // world distance advanced entering this vertex (first stays 0)
            if (i > 0)
                dist += static_cast<float>(std::abs(d[axis])) *
                        static_cast<float>(axis == 1 ? height_m : cell_m);
            const float cx = static_cast<float>((cell[0] + 0.5) * cell_m);
            const float cy = static_cast<float>((cell[1] + 0.5) * height_m);
            const float cz = static_cast<float>((cell[2] + 0.5) * cell_m);
            for (int k = 0; k < kTubeSides; ++k) {
                const float rc = kRingC[k], rs = kRingS[k];
                const std::array<float, 3> nr{rc * u[0] + rs * b[0],
                                              rc * u[1] + rs * b[1],
                                              rc * u[2] + rs * b[2]};
                pos.insert(pos.end(),
                           {cx + kTubeRadius * nr[0], cy + kTubeRadius * nr[1],
                            cz + kTubeRadius * nr[2]});
                nrm.insert(nrm.end(), nr.begin(), nr.end());
                uvs.push_back(dist);
                uvs.push_back(static_cast<float>(k) / kTubeSides);
                tan.insert(tan.end(), {t[0], t[1], t[2], 1.f});
            }
        }
        for (float v : pos) wr.f32(v);
        for (float v : nrm) wr.f32(v);
        for (float v : uvs) wr.f32(v);
        for (float v : tan) wr.f32(v);
        wr.u32(n > 0 ? (n - 1) * kTubeSides * 2 : 0u);  // 2 tris per side quad
        for (std::uint32_t i = 0; i + 1 < n; ++i)
            for (int k = 0; k < kTubeSides; ++k) {
                const std::uint32_t a = i * kTubeSides + k,
                                    b2 = i * kTubeSides + (k + 1) % kTubeSides,
                                    c2 = (i + 1) * kTubeSides + (k + 1) % kTubeSides,
                                    d2 = (i + 1) * kTubeSides + k;
                wr.u32(a); wr.u32(b2); wr.u32(c2);
                wr.u32(a); wr.u32(c2); wr.u32(d2);
            }
    }
    wr.u32(0u);  // material_count: no MAT0 (#51c)
    wr.u32(0u);  // socket_count: tubes carry no anchors
    wr.u32(0u);  // root_extras_len
    out = std::move(wr.b);
}

bool cook_hulldef_unit(const std::string& toml_text, Bytes& payload_out,
                       std::vector<std::string>& refs_out, std::string* err,
                       const std::map<std::string, CompTypeDef>* type_defs,
                       std::vector<std::string>* used_types_out,
                       std::vector<std::string>* entry_refs_out,
                       const std::map<std::string, std::string>* art_by_comp,
                       const std::map<std::string, MountTRS>* anchors,
                       Bytes* tubes_out) {
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
            // #52a/#55 component TYPE def resolution: the type def is the
            // single truth for category, and its [attrs] preset joins the
            // entry's map — an explicit hull key ALWAYS wins (#52a). The
            // merged map emits in std::map key order like every attrs list.
            const CompTypeDef* td = nullptr;
            if (type_defs) {
                auto it = type_defs->find(c.type);
                if (it != type_defs->end()) td = &it->second;
            }
            if (td) {
                if (c.category != td->category)
                    return fail(e, where + ".category '" +
                                     std::string{ve::game::kCategory_names[c.category]} +
                                     "' does not match component type '" + c.type +
                                     "' category '" +
                                     std::string{ve::game::kCategory_names[td->category]} +
                                     "' (#55)");
                std::map<std::string, double> merged;
                for (const auto& [k, v] : td->attrs) merged.emplace(k, v);
                for (const auto& a : c.attrs) merged[a.key] = a.value;
                c.attrs.clear();
                for (const auto& [k, v] : merged) c.attrs.push_back({k, v});
            }
            if (used_types_out && td) used_types_out->push_back(c.type);
            // #55b edge doctrine: only the entry's OWN refs become hull dep
            // edges (authored against the hull's directory). The type def's
            // refs are authored against the type def's directory and already
            // ride the type def unit's own manifest edges — the hull reaches
            // inherited art transitively through its hull -> type-def edge;
            // re-resolving a type-def-relative path against the hull dir
            // would point at the wrong content.
            if (entry_refs_out && !c.refs.empty())
                entry_refs_out->insert(entry_refs_out->end(), c.refs.begin(), c.refs.end());
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
        w.u8(c.face);  // #46c1
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
        // #52b (DEF0 v3): resolved art asset ('' = unresolved -> G11 fails
        // the cook) + the world TRS of the socket/empty this entry mounts
        // to (the model's keyed node carries the offset from it).
        std::string art;
        if (art_by_comp) {
            if (auto a = art_by_comp->find(c.id); a != art_by_comp->end())
                art = a->second;
        }
        w.str(art);
        const MountTRS* m = nullptr;
        if (anchors && !c.socket.empty()) {
            if (auto an = anchors->find(c.socket); an != anchors->end())
                m = &an->second;
        }
        w.u8(m ? 1 : 0);
        if (m) {
            for (double v : m->t) w.f64(v);
            for (double v : m->q) w.f64(v);
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
        w.u32(static_cast<std::uint32_t>(wire.route.size()));
        // `rc` not `cell`: MSVC /W4 flags C4456 against the grid.cell TOML
        // lookup earlier in this function.
        for (const auto& rc : wire.route)
            for (std::int32_t v : rc) w.i32(v);
    }
    // #51: section->wires index, DERIVED at cook — the query site for
    // damage severing (9.6: a hit on a section cuts exactly the wires routed
    // through it). Row i corresponds to section row i above (authored order,
    // deterministic); entries are indices into the wire table above,
    // ascending, a wire listed once per section any of its route cells lies
    // in. Gate G12 later requires every route cell to sit in exactly one
    // section, so a valid def yields a well-defined index; cells outside all
    // boxes simply contribute nothing here (the gate catches that case).
    w.u32(static_cast<std::uint32_t>(sections.size()));
    for (const auto& [sname, boxes] : sections) {
        std::vector<std::uint32_t> hits;
        for (std::size_t i = 0; i < wires.size(); ++i) {
            bool through = false;
            for (const auto& rc : wires[i].route) {
                for (const auto& b : boxes) {
                    if (rc[0] >= b[0] && rc[0] <= b[3] &&
                        rc[1] >= b[1] && rc[1] <= b[4] &&
                        rc[2] >= b[2] && rc[2] <= b[5]) { through = true; break; }
                }
                if (through) break;
            }
            if (through) hits.push_back(static_cast<std::uint32_t>(i));
        }
        w.u32(static_cast<std::uint32_t>(hits.size()));
        for (std::uint32_t h : hits) w.u32(h);
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

    // #51c: companion thin-tube MESH payload for the wire visuals, built
    // from the same ordered wire table (never re-parsed). Driver writes it
    // as the <def id>.wires unit — see cook.cpp (sock-sibling doctrine).
    if (tubes_out) emit_wire_tubes(wires, cell_m, height_m, *tubes_out);

    payload_out = std::move(w.b);
    return true;
}

bool cook_hulldef_comp_requests(const std::string& toml_text,
                                std::vector<CompArtRequest>& out, std::string* err) {
    out.clear();
    std::string local;
    std::string& e = err ? *err : local;
    TomlValue root;
    if (!parse_toml(toml_text, root, &e)) return false;
    const TomlTable& rt = root.table;
    if (!at(rt, "class_tag")) return true;  // not a hull def: no requests
    const TomlValue* cs = at(rt, "component");
    if (!cs || cs->kind != TomlValue::kArray) return true;
    for (std::size_t i = 0; i < cs->array.size(); ++i) {
        CompRow c;
        if (!parse_component(cs->array[i], "component[" + std::to_string(i) + "]", c, e))
            return false;
        out.push_back({std::move(c.id), std::move(c.type), std::move(c.refs)});
    }
    return true;
}

bool toml_source_has_component_type(const std::string& toml_text) {
    TomlValue root;
    std::string err;
    if (!parse_toml(toml_text, root, &err)) return false;
    return root.is_table() && root.table.count("component_type") != 0;
}

bool cook_comptype_source(const std::string& toml_text, CompTypeDef& out, std::string* err) {
    std::string local;
    std::string& e = err ? *err : local;
    TomlValue root;
    if (!parse_toml(toml_text, root, &e)) return false;  // historical parse message
    if (!root.is_table()) return fail(e, "component type def root must be a table");
    const TomlTable& rt = root.table;
    out = CompTypeDef{};

    // #55(a): the three def markers are mutually exclusive — checked before
    // the closed-field-set pass so the conflict gets its own loud message.
    if (rt.count("class_tag"))
        return fail(e, "component type def declares both class_tag and component_type (#55)");
    if (rt.count("world_class"))
        return fail(e, "component type def declares both world_class and component_type (#55)");

    if (!req_str(rt, "component_type", "", out.type, e)) return false;
    std::string cat;
    if (!req_str(rt, "category", "", cat, e)) return false;
    if (!category_u8(cat, out.category))
        return fail(e, "category '" + cat + "' not one of main/rcs/gun/utility");
    // refs may be empty until the 9.2 strict art gate lands (#55a).
    if (const TomlValue* r = at(rt, "refs"))
        if (!req_string_array(*r, "refs", out.refs, e)) return false;
    std::vector<Attr> preset;
    if (const TomlValue* a = at(rt, "attrs"))
        if (!parse_attrs_map(*a, "attrs", preset, e)) return false;  // #42 at this end too
    for (const auto& p : preset) out.attrs.emplace_back(p.key, p.value);

    // #55 grammar is CLOSED: an unknown top-level field would silently no-op
    // (a typo'd preset key never applies) — fail loudly instead.
    for (const auto& [k, v] : rt)
        if (k != "component_type" && k != "category" && k != "refs" && k != "attrs")
            return fail(e, "unknown field '" + k + "' in component type def (#55)");
    return true;
}

}  // namespace ve::cook
