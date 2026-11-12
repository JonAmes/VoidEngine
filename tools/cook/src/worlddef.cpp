// Phase 8 task 8.7 — the world-def TOML schema parser + 8.7 gates (cook
// side; see tools/cook/include/ve/cook/worlddef.h for the schema). Gates run
// in fixed order, first failure only; the payload is encoded by the shared
// ve::game codec so cook/load cannot drift.
#include "ve/cook/worlddef.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <ve/game/world_load.h>

#include "ve/cook/toml.h"

namespace ve::cook {

namespace {

using ve::game::NavKind;
using ve::game::PoiKind;
using ve::game::WorldBuildPlan;
using ve::game::WorldBuildStep;
using ve::game::WorldClass;
using ve::game::WorldDef;
using ve::game::WorldJobStep;
using ve::game::WorldMarketCommodity;

// Flat-row staging for AOT rows that fold into parent structs in G6.
struct WorldBuildRow {
    std::string plan, module, socket;
    double at_s = 0.0;
};
struct ProdRow {
    std::string market, commodity;
    double rate = 0.0;
};
struct JobStepRow {
    std::string job, verb, target;
    double amount = 0.0;
};

// Flat-row AOT accessor: the rows of [[key]] as tables (empty if absent).
const std::vector<TomlValue>* rows_of(const TomlTable& t, const char* key) {
    const auto it = t.find(key);
    if (it == t.end()) return nullptr;
    if (it->second.kind != TomlValue::kArray) return nullptr;
    return &it->second.array;
}

struct Schema {
    WorldDef def;
    std::string ctx = "world def";  // grows to "world body 'Vesper Prime'"
    std::string err;
    std::vector<WorldBuildRow> build_rows;
    std::vector<ProdRow> prod_rows;
    std::vector<JobStepRow> job_step_rows;

    std::string* errp = nullptr;  // caller's err; every failure lands there

    bool fail(const std::string& key, const std::string& what) {
        return bad(ctx + ": " + key + ": " + what);
    }
    // first-failure doctrine: exactly one actionable message escapes.
    bool bad(std::string msg) {
        err = std::move(msg);
        if (errp) *errp = err;
        return false;
    }

    // orbit block shared by body + station classes (#24/#45: an orbiting
    // entry needs its parent AND a period; root anchors carry neither).
    bool orbit(const TomlTable& t) {
        auto& b = def.rails;
        if (!unit(t, "orbit_period", b.orbit_period_s, false)) return false;
        if (!unit(t, "orbit_radius", b.orbit_radius_m, false)) return false;
        if (!unit(t, "orbit_phase", b.orbit_phase_rad, false)) return false;
        double ax[3] = {0.0, 0.0, 0.0};
        const double up[3] = {0.0, 1.0, 0.0};
        if (!triple(t, "spin_axis", ax, false, up)) return false;
        b.spin_axis = {ax[0], ax[1], ax[2]};
        if (!unit(t, "spin_rate", b.spin_rate_rad_s, false)) return false;
        if (!def.parent_ref.empty() && !(b.orbit_period_s > 0.0))
            return fail("orbit_period", "an orbiting entry needs orbit_period > 0");
        return true;
    }

    // ---- scalar field helpers (first failure aborts; order = code order) ----

    // #42 unit string -> SI. Length/time/angle scalars are ALWAYS strings.
    bool unit(const TomlTable& t, const char* key, double& out, bool required,
              double dflt = 0.0) {
        const auto it = t.find(key);
        if (it == t.end()) {
            if (!required) { out = dflt; return true; }
            return fail(key, "missing required key");
        }
        if (it->second.kind != TomlValue::kString)
            return fail(key, "expected a \"NUMBER SUFFIX\" unit string (#42)");
        const auto si = si_from_string(it->second.str);
        if (!si) return fail(key, si.error());
        out = *si;
        return true;
    }

    // Bare SI number (dimensionless factors, rates, triples — hull-attr rule).
    bool bare(const TomlTable& t, const char* key, double& out, bool required,
              double dflt = 0.0) {
        const auto it = t.find(key);
        if (it == t.end()) {
            if (!required) { out = dflt; return true; }
            return fail(key, "missing required key");
        }
        if (!it->second.is_number())
            return fail(key, "expected a bare SI number");
        out = it->second.num();
        return true;
    }

    bool strf(const TomlTable& t, const char* key, std::string& out, bool required) {
        const auto it = t.find(key);
        if (it == t.end()) {
            if (!required) return true;
            return fail(key, "missing required key");
        }
        if (it->second.kind != TomlValue::kString)
            return fail(key, "expected a string");
        out = it->second.str;
        return true;
    }

    bool triple(const TomlTable& t, const char* key, double v[3], bool required,
                const double dflt[3]) {
        const auto it = t.find(key);
        if (it == t.end()) {
            if (!required) { v[0] = dflt[0]; v[1] = dflt[1]; v[2] = dflt[2]; return true; }
            return fail(key, "missing required key");
        }
        if (it->second.kind != TomlValue::kArray || it->second.array.size() != 3)
            return fail(key, "expected [x, y, z]");
        for (int i = 0; i < 3; ++i) {
            if (!it->second.array[i].is_number())
                return fail(key, "expected bare SI numbers");
            v[i] = it->second.array[i].num();
        }
        return true;
    }

    bool strs(const TomlTable& t, const char* key, std::vector<std::string>& out) {
        const auto it = t.find(key);
        if (it == t.end()) return true;
        if (it->second.kind != TomlValue::kArray)
            return fail(key, "expected an array of strings");
        for (const auto& e : it->second.array) {
            if (e.kind != TomlValue::kString) return fail(key, "expected an array of strings");
            out.push_back(e.str);
        }
        return true;
    }

    bool integer(const TomlTable& t, const char* key, std::int64_t& out, bool required,
                 std::int64_t dflt = 0) {
        const auto it = t.find(key);
        if (it == t.end()) {
            if (!required) { out = dflt; return true; }
            return fail(key, "missing required key");
        }
        if (it->second.kind != TomlValue::kInteger)
            return fail(key, "expected an integer");
        out = it->second.integer;
        return true;
    }

    static bool has_dup(const std::vector<std::string>& v) {
        auto s = v;
        std::sort(s.begin(), s.end());
        return std::adjacent_find(s.begin(), s.end()) != s.end();
    }
};

bool parse_stamp_fields(Schema& sc, const TomlTable& row, core::sim::terrain_stamp& st,
                        const char* type_key) {
    std::string type;
    if (!sc.strf(row, type_key, type, true)) return false;
    if (type == "flatten") st.type = core::sim::stamp_type::flatten;
    else if (type == "crater") st.type = core::sim::stamp_type::crater;
    else if (type == "mesa") st.type = core::sim::stamp_type::mesa;
    else return sc.fail(type_key, "stamp type '" + type + "' must be flatten, crater or mesa");
    double c[3];
    if (!sc.triple(row, "center", c, true, c)) return false;
    if (c[0] == 0.0 && c[1] == 0.0 && c[2] == 0.0)
        return sc.fail("center", "stamp center must be non-zero");
    st.center_dir = {c[0], c[1], c[2]};
    if (!sc.unit(row, "radius", st.radius_rad, true)) return false;
    if (!(st.radius_rad > 0.0)) return sc.fail("radius", "stamp radius must be > 0");
    if (!sc.unit(row, "level", st.level_m, false)) return false;
    if (!sc.bare(row, "blend", st.blend, false, 0.25)) return false;
    if (st.blend < 0.0 || st.blend > 1.0)
        return sc.fail("blend", "blend ring must be within [0, 1]");
    return true;
}

}  // namespace

bool toml_source_has_world_class(const std::string& toml_text) {
    TomlValue root;
    std::string err;
    if (!parse_toml(toml_text, root, &err)) return false;
    return root.is_table() && root.table.count("world_class") != 0;
}

bool cook_worlddef_source(const std::string& toml_text, core::cooked::Bytes& payload_out,
                          std::vector<std::string>& refs_out, std::string* err) {
    TomlValue root;
    if (!parse_toml(toml_text, root, err)) return false;  // historical parse message

    Schema sc;
    sc.errp = err;

    // G1: world_class (routing truth: cook.cpp already sniffed it; re-check
    // spelling here so every gate failure is reported through one pipeline).
    const auto wc = root.table.find("world_class");
    if (wc == root.table.end() || wc->second.kind != TomlValue::kString)
        return sc.fail("world_class", "must be a string: system, body or station");
    const auto cls = ve::game::world_class_of(wc->second.str);
    if (!cls) {
        *err = sc.ctx + ": " + cls.error();
        return false;
    }
    sc.def.world_class = *cls;
    sc.ctx = std::string("world ") + ve::game::world_class_name(*cls) + " def";

    // G2: name.
    if (!sc.strf(root.table, "name", sc.def.name, true)) return false;
    if (sc.def.name.empty()) return sc.fail("name", "must be non-empty");
    sc.ctx = std::string("world ") + ve::game::world_class_name(*cls) + " '" + sc.def.name + "'";

    // G3: class sections, in schema order; G4 grammar errors ride along.
    if (*cls == WorldClass::body) {
        auto& b = sc.def.rails;
        if (!sc.strf(root.table, "parent", sc.def.parent_ref, false)) return false;
        if (!sc.unit(root.table, "radius", b.surface_radius, true)) return false;
        if (!(b.surface_radius > 0.0)) return sc.fail("radius", "surface radius must be > 0");
        if (!sc.unit(root.table, "well_radius", b.well_radius, false)) return false;
        // surface_g + atm_rho0 are COMPOSITE units (m/s^2, kg/m^3) — no
        // ratified #42 suffix exists, so they ride bare SI like hull attrs.
        if (!sc.bare(root.table, "surface_g", b.surface_g, false)) return false;
        if (!sc.unit(root.table, "well_taper_start", b.well_taper_start, false)) return false;
        if (b.well_radius > 0.0 && b.well_radius < b.surface_radius)
            return sc.fail("well_radius", "well radius must reach >= surface radius");
        if (!sc.bare(root.table, "atm_rho0", b.atm_rho0_kg_m3, false)) return false;
        if (!sc.unit(root.table, "atm_scale_height", b.atm_scale_height_m, false)) return false;
        if (b.atm_rho0_kg_m3 > 0.0 && !(b.atm_scale_height_m > 0.0))
            return sc.fail("atm_scale_height", "atmosphere needs scale height > 0");
        if (!sc.orbit(root.table)) return false;
        // [terrain] — absent = flat body (relief 0).
        const auto tr = root.table.find("terrain");
        if (tr != root.table.end()) {
            if (!tr->second.is_table()) return sc.fail("terrain", "expected a [terrain] table");
            std::int64_t seed = 0, oct = 1;
            if (!sc.integer(tr->second.table, "seed", seed, false)) return false;
            if (seed < 0) return sc.fail("terrain.seed", "seed must be >= 0");
            sc.def.terrain.seed = static_cast<std::uint64_t>(seed);
            if (!sc.unit(tr->second.table, "relief", sc.def.terrain.relief_m, false)) return false;
            if (!sc.bare(tr->second.table, "base_frequency", sc.def.terrain.base_frequency,
                         false, 1.0)) return false;
            if (!sc.integer(tr->second.table, "octaves", oct, false, 1)) return false;
            if (oct < 1 || oct > core::sim::kMax_octaves)
                return sc.fail("terrain.octaves", "octaves must be within [1, 8]");
            sc.def.terrain.octaves = static_cast<int>(oct);
            if (!sc.bare(tr->second.table, "lacunarity", sc.def.terrain.lacunarity,
                         false, 2.0)) return false;
            if (!sc.bare(tr->second.table, "gain", sc.def.terrain.gain, false, 0.5)) return false;
        }
        if (const auto* rs = rows_of(root.table, "stamp")) {
            std::size_t i = 0;
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[stamp]] rows must be tables"); }
                core::sim::terrain_stamp st;
                sc.ctx += " stamp " + std::to_string(i);  // row-scoped context
                const bool ok = parse_stamp_fields(sc, r.table, st, "type");
                sc.ctx = std::string("world body '") + sc.def.name + "'";
                if (!ok) return false;
                sc.def.stamps.push_back(st);
                ++i;
            }
        }
        if (const auto* rs = rows_of(root.table, "poi")) {
            std::size_t i = 0;
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[poi]] rows must be tables"); }
                ve::game::WorldPoi p;
                sc.ctx += " poi " + std::to_string(i);
                bool ok = sc.strf(r.table, "name", p.name, true) && !p.name.empty();
                if (ok) {
                    std::string kind;
                    ok = sc.strf(r.table, "kind", kind, true);
                    if (ok && kind == "pad") p.kind = PoiKind::pad;
                    else if (ok && kind == "outpost") p.kind = PoiKind::outpost;
                    else if (ok && kind == "landmark") p.kind = PoiKind::landmark;
                    else if (ok) ok = sc.fail("kind", "poi kind '" + kind +
                                                      "' must be pad, outpost or landmark");
                }
                if (ok) ok = parse_stamp_fields(sc, r.table, p.stamp, "stamp");
                if (ok) ok = sc.unit(r.table, "footprint", p.footprint_m, true);
                if (ok && !(p.footprint_m > 0.0)) ok = sc.fail("footprint", "must be > 0");
                sc.ctx = std::string("world body '") + sc.def.name + "'";
                if (!ok) return false;
                sc.def.pois.push_back(std::move(p));
                ++i;
            }
        }
        if (const auto* rs = rows_of(root.table, "biome")) {
            std::size_t i = 0;
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[biome]] rows must be tables"); }
                ve::game::WorldBiomeBand bb;
                sc.ctx += " biome " + std::to_string(i);
                bool ok = sc.unit(r.table, "lo", bb.lo_m, true) &&
                          sc.unit(r.table, "hi", bb.hi_m, true);
                if (ok && !(bb.lo_m < bb.hi_m)) ok = sc.fail("hi", "biome band needs lo < hi");
                const double white[3] = {1.0, 1.0, 1.0};
                if (ok) ok = sc.triple(r.table, "color", bb.color, false, white);
                if (ok) ok = sc.bare(r.table, "roughness", bb.roughness, false, 0.8);
                if (ok) ok = sc.bare(r.table, "scatter", bb.scatter_density, false, 0.0);
                sc.ctx = std::string("world body '") + sc.def.name + "'";
                if (!ok) return false;
                sc.def.biomes.push_back(bb);
                ++i;
            }
        }
        if (!sc.strs(root.table, "tags", sc.def.tags)) return false;
    } else if (*cls == WorldClass::station) {
        if (!sc.strf(root.table, "hull_def", sc.def.hull_def_ref, true) ||
            sc.def.hull_def_ref.empty())
            return sc.fail("hull_def", "station needs a hull def ref (#24a)");
        if (!sc.strf(root.table, "parent", sc.def.parent_ref, false)) return false;
        if (!sc.orbit(root.table)) return false;
        if (!sc.strf(root.table, "root_socket", sc.def.root_socket, false)) return false;
        if (const auto* rs = rows_of(root.table, "build_step")) {
            std::size_t i = 0;
            for (const auto& r : *rs) {
                if (!r.is_table()) {
                    return sc.bad(sc.ctx + ": [[build_step]] rows must be tables");
                }
                sc.ctx += " build_step " + std::to_string(i);
                WorldBuildRow row;
                bool ok = sc.strf(r.table, "plan", row.plan, true) && !row.plan.empty() &&
                          sc.strf(r.table, "module", row.module, true) && !row.module.empty() &&
                          sc.strf(r.table, "socket", row.socket, true) && !row.socket.empty() &&
                          sc.unit(r.table, "at", row.at_s, true) && row.at_s >= 0.0;
                if (ok && !(row.at_s >= 0.0)) ok = sc.fail("at", "build time must be >= 0");
                sc.ctx = std::string("world station '") + sc.def.name + "'";
                if (!ok) return false;
                sc.build_rows.push_back(std::move(row));
                ++i;
            }
        }
        if (!sc.strs(root.table, "tags", sc.def.tags)) return false;
    } else {
        std::int64_t seed = 0;
        if (!sc.integer(root.table, "seed", seed, false)) return false;
        if (seed < 0) return sc.fail("seed", "seed must be >= 0");
        sc.def.seed = static_cast<std::uint64_t>(seed);
        if (!sc.strs(root.table, "members", sc.def.members)) return false;
        if (const auto* rs = rows_of(root.table, "faction")) {
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[faction]] rows must be tables"); }
                ve::game::WorldFaction f;
                if (!sc.strf(r.table, "name", f.name, true) || f.name.empty())
                    return sc.fail("name", "faction name missing or empty");
                if (!sc.bare(r.table, "power", f.power_production, false, 0.0)) return false;
                if (!sc.strs(r.table, "stance", f.tags)) return false;
                sc.def.factions.push_back(std::move(f));
            }
        }
        if (const auto* rs = rows_of(root.table, "market")) {
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[market]] rows must be tables"); }
                ve::game::WorldMarketNode m;
                if (!sc.strf(r.table, "id", m.id, true) || m.id.empty())
                    return sc.fail("id", "market id missing or empty");
                if (!sc.strf(r.table, "anchor", m.anchor_ref, true) || m.anchor_ref.empty())
                    return sc.fail("anchor", "market anchor missing");
                sc.def.markets.push_back(std::move(m));
            }
        }
        if (const auto* rs = rows_of(root.table, "production")) {
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[production]] rows must be tables"); }
                ProdRow p;
                if (!sc.strf(r.table, "market", p.market, true) || p.market.empty() ||
                    !sc.strf(r.table, "commodity", p.commodity, true) || p.commodity.empty())
                    return sc.fail("market", "production row needs market + commodity");
                if (!sc.bare(r.table, "rate", p.rate, true)) return false;
                if (!(p.rate >= 0.0)) return sc.fail("rate", "production rate must be >= 0");
                sc.prod_rows.push_back(std::move(p));
            }
        }
        if (const auto* rs = rows_of(root.table, "lane")) {
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[lane]] rows must be tables"); }
                ve::game::WorldLane l;
                if (!sc.strf(r.table, "id", l.id, true) || l.id.empty() ||
                    !sc.strf(r.table, "from", l.from, true) || l.from.empty() ||
                    !sc.strf(r.table, "to", l.to, true) || l.to.empty())
                    return sc.fail("lane", "lane row needs id, from and to");
                if (!sc.strf(r.table, "commodity", l.commodity, true) || l.commodity.empty())
                    return sc.fail("commodity", "lane needs a commodity");
                if (!sc.bare(r.table, "flow", l.flow_rate, true)) return false;
                if (!(l.flow_rate >= 0.0)) return sc.fail("flow", "lane flow must be >= 0");
                sc.def.lanes.push_back(std::move(l));
            }
        }
        if (const auto* rs = rows_of(root.table, "waypoint")) {
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[waypoint]] rows must be tables"); }
                ve::game::WorldNavNode n;
                if (!sc.strf(r.table, "id", n.id, true) || n.id.empty())
                    return sc.fail("id", "waypoint id missing or empty");
                std::string kind;
                if (!sc.strf(r.table, "kind", kind, true)) return false;
                if (kind == "lane") n.kind = NavKind::lane;
                else if (kind == "patrol") n.kind = NavKind::patrol;
                else if (kind == "local") n.kind = NavKind::local;
                else return sc.fail("kind", "waypoint kind '" + kind +
                                            "' must be lane, patrol or local");
                if (!sc.strf(r.table, "body", n.body_ref, false)) return false;
                const double zero[3] = {0.0, 0.0, 0.0};
                if (!sc.triple(r.table, "at", n.offset, false, zero)) return false;
                sc.def.waypoints.push_back(std::move(n));
            }
        }
        if (const auto* rs = rows_of(root.table, "edge")) {
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[edge]] rows must be tables"); }
                ve::game::WorldNavEdge e;
                std::string kind;
                if (!sc.strf(r.table, "kind", kind, true)) return false;
                if (kind == "lane") e.kind = NavKind::lane;
                else if (kind == "patrol") e.kind = NavKind::patrol;
                else if (kind == "local") e.kind = NavKind::local;
                else return sc.fail("kind", "edge kind '" + kind +
                                            "' must be lane, patrol or local");
                if (!sc.strf(r.table, "from", e.from, true) || e.from.empty() ||
                    !sc.strf(r.table, "to", e.to, true) || e.to.empty())
                    return sc.fail("edge", "edge row needs from and to");
                sc.def.nav_edges.push_back(std::move(e));
            }
        }
        if (const auto* rs = rows_of(root.table, "job")) {
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[job]] rows must be tables"); }
                ve::game::WorldJobTemplate j;
                if (!sc.strf(r.table, "id", j.id, true) || j.id.empty())
                    return sc.fail("id", "job id missing or empty");
                sc.def.job_templates.push_back(std::move(j));
            }
        }
        if (const auto* rs = rows_of(root.table, "job_step")) {
            for (const auto& r : *rs) {
                if (!r.is_table()) { return sc.bad(sc.ctx + ": [[job_step]] rows must be tables"); }
                JobStepRow s;
                if (!sc.strf(r.table, "job", s.job, true) || s.job.empty() ||
                    !sc.strf(r.table, "verb", s.verb, true) || s.verb.empty())
                    return sc.fail("job", "job_step row needs job + verb");
                if (!sc.strf(r.table, "target", s.target, false)) return false;
                if (!sc.bare(r.table, "amount", s.amount, false, 0.0)) return false;
                sc.job_step_rows.push_back(std::move(s));
            }
        }
    }

    // G5: duplicates within the doc.
    if (*cls == WorldClass::body) {
        std::vector<std::string> poi_names;
        for (const auto& p : sc.def.pois) poi_names.push_back(p.name);
        if (Schema::has_dup(poi_names)) { return sc.bad(sc.ctx + ": duplicate poi name"); }
    } else if (*cls == WorldClass::system) {
        if (Schema::has_dup(sc.def.members)) {
            return sc.bad(sc.ctx + ": duplicate member ref");
        }
        std::vector<std::string> names;
        for (const auto& f : sc.def.factions) names.push_back(f.name);
        if (Schema::has_dup(names)) { return sc.bad(sc.ctx + ": duplicate faction name"); }
        names.clear();
        for (const auto& m : sc.def.markets) names.push_back(m.id);
        if (Schema::has_dup(names)) { return sc.bad(sc.ctx + ": duplicate market id"); }
        names.clear();
        for (const auto& l : sc.def.lanes) names.push_back(l.id);
        if (Schema::has_dup(names)) { return sc.bad(sc.ctx + ": duplicate lane id"); }
        names.clear();
        for (const auto& w : sc.def.waypoints) names.push_back(w.id);
        if (Schema::has_dup(names)) { return sc.bad(sc.ctx + ": duplicate waypoint id"); }
        names.clear();
        for (const auto& j : sc.def.job_templates) names.push_back(j.id);
        if (Schema::has_dup(names)) { return sc.bad(sc.ctx + ": duplicate job id"); }
    }

    // G6: in-doc row joins (cross-FILE resolution is 8.8). Flat rows fold
    // into their parent structures in first-seen order (deterministic).
    if (*cls == WorldClass::station) {
        for (const auto& row : sc.build_rows) {
            WorldBuildPlan* plan = nullptr;
            for (auto& p : sc.def.build_plans)
                if (p.id == row.plan) { plan = &p; break; }
            if (!plan) {
                plan = &sc.def.build_plans.emplace_back();
                plan->id = row.plan;
            }
            WorldBuildStep s;
            s.module_def_ref = row.module;
            s.target_socket = row.socket;
            s.at_time_s = row.at_s;
            plan->steps.push_back(std::move(s));
        }
    }
    if (*cls == WorldClass::system) {
        for (const auto& row : sc.prod_rows) {
            bool found = false;
            for (auto& m : sc.def.markets)
                if (m.id == row.market) {
                    found = true;
                    WorldMarketCommodity c;
                    c.commodity = row.commodity;
                    c.production = row.rate;
                    m.commodities.push_back(std::move(c));
                    break;
                }
            if (!found) {
                return sc.bad(sc.ctx + ": production row references unknown market '" + row.market + "'");
            }
        }
        for (const auto& row : sc.job_step_rows) {
            bool found = false;
            for (auto& j : sc.def.job_templates)
                if (j.id == row.job) {
                    found = true;
                    WorldJobStep s;
                    s.verb = row.verb;
                    s.target_ref = row.target;
                    s.amount = row.amount;
                    j.steps.push_back(std::move(s));
                    break;
                }
            if (!found) {
                return sc.bad(sc.ctx + ": job_step references unknown job '" + row.job + "'");
            }
        }
    }

    payload_out = ve::game::encode_world(sc.def);
    refs_out = sc.def.collect_refs();
    return true;
}

}  // namespace ve::cook
