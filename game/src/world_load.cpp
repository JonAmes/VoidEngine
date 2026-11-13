// Phase 8 task 8.7 — WORLD0 payload encode/decode (single codec, DEF0
// precedent; layout documented in core/cooked.h). Pure byte-shuffling: the
// TOML schema lives on the cook side (tools/cook/src/worlddef.cpp).
#include "ve/game/world_load.h"

#include <array>
#include <cstring>
#include <string_view>

namespace ve::game {

std::expected<WorldClass, std::string> world_class_of(std::string_view text) {
    if (text == "system") return WorldClass::system;
    if (text == "body") return WorldClass::body;
    if (text == "station") return WorldClass::station;
    return std::unexpected("world_class '" + std::string(text) +
                           "' must be system, body or station");
}

const char* world_class_name(WorldClass c) {
    switch (c) {
        case WorldClass::system: return "system";
        case WorldClass::body: return "body";
        case WorldClass::station: return "station";
    }
    return "?";
}

std::vector<std::string> WorldDef::collect_refs() const {
    std::vector<std::string> refs;
    if (!parent_ref.empty()) refs.push_back(parent_ref);
    if (!hull_def_ref.empty()) refs.push_back(hull_def_ref);
    for (const auto& m : members) refs.push_back(m);
    for (const auto& p : build_plans)
        for (const auto& s : p.steps)
            if (!s.module_def_ref.empty()) refs.push_back(s.module_def_ref);
    // 8.8: market anchors and waypoint bodies are def refs too — they must
    // become manifest edges so dangling ones fail the cook at authoring time
    // (#20d/#27a). "poi:<name>" anchors reference POIs INSIDE a member body
    // and are resolved by the wrld graph gates, not by the file graph.
    for (const auto& mk : markets)
        if (!mk.anchor_ref.empty() && mk.anchor_ref.rfind("poi:", 0) != 0)
            refs.push_back(mk.anchor_ref);
    for (const auto& w : waypoints)
        if (!w.body_ref.empty()) refs.push_back(w.body_ref);
    return refs;
}

core::sim::body_def sim_body_def(const WorldDef& body) {
    core::sim::body_def b = body.rails;
    b.parent = -1;  // def refs become body ids only at world assembly
    return b;
}

namespace {

struct Writer {
    core::cooked::Bytes b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void f64(double v) {
        std::array<std::uint8_t, 8> t{};
        std::memcpy(t.data(), &v, 8);
        for (auto c : t) b.push_back(c);
    }
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        for (char c : s) b.push_back(static_cast<std::uint8_t>(c));
    }
    void strs(const std::vector<std::string>& v) {
        u32(static_cast<std::uint32_t>(v.size()));
        for (const auto& s : v) str(s);
    }
    void stamp(const core::sim::terrain_stamp& st) {
        u8(static_cast<std::uint8_t>(st.type));
        f64(st.center_dir.x); f64(st.center_dir.y); f64(st.center_dir.z);
        f64(st.radius_rad); f64(st.level_m); f64(st.blend);
    }
};

struct Reader {
    const core::cooked::Bytes& b;
    std::size_t at = 0;
    bool bad = false;  // one flag: every read past the end is caught, report once

    void need(std::size_t n) { if (at + n > b.size()) { bad = true; at = b.size(); } }
    std::uint8_t u8() { need(1); if (bad) return 0; return b[at++]; }
    std::uint32_t u32() {
        need(4); if (bad) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[at + i]) << (8 * i);
        at += 4;
        return v;
    }
    std::uint64_t u64() {
        need(8); if (bad) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[at + i]) << (8 * i);
        at += 8;
        return v;
    }
    double f64() {
        need(8); if (bad) return 0.0;
        double v = 0.0;
        std::memcpy(&v, &b[at], 8);
        at += 8;
        return v;
    }
    std::string str() {
        const auto n = u32();
        if (bad) return {};
        need(n); if (bad) return {};
        std::string s(reinterpret_cast<const char*>(&b[at]), n);
        at += n;
        return s;
    }
    std::vector<std::string> strs() {
        const auto n = u32();
        std::vector<std::string> v;
        v.reserve(n < b.size() ? n : 0);  // never trust a corrupt count
        for (std::uint32_t i = 0; i < n && !bad; ++i) v.push_back(str());
        return v;
    }
    core::sim::terrain_stamp stamp() {
        core::sim::terrain_stamp st;
        st.type = static_cast<core::sim::stamp_type>(u8());
        st.center_dir = {f64(), f64(), f64()};
        st.radius_rad = f64(); st.level_m = f64(); st.blend = f64();
        return st;
    }
};

}  // namespace

core::cooked::Bytes encode_world(const WorldDef& def) {
    Writer w;
    w.u32(1);  // payload_ver
    w.u8(static_cast<std::uint8_t>(def.world_class));
    w.str(def.name);

    if (def.world_class == WorldClass::body) {
        w.str(def.parent_ref);
        const auto& r = def.rails;
        w.f64(r.surface_radius); w.f64(r.well_radius);
        w.f64(r.surface_g); w.f64(r.well_taper_start);
        w.f64(r.atm_rho0_kg_m3); w.f64(r.atm_scale_height_m);
        w.f64(r.orbit_period_s); w.f64(r.orbit_radius_m); w.f64(r.orbit_phase_rad);
        w.f64(r.spin_axis.x); w.f64(r.spin_axis.y); w.f64(r.spin_axis.z);
        w.f64(r.spin_rate_rad_s);
        // #50 safe bubble trio (8.13): wall radius + sublight ceiling
        // (core rails fields), then the game-side flat impact value.
        w.f64(r.safe_bubble_radius); w.f64(r.safe_speed);
        w.f64(def.safe_impact);
        w.u64(def.terrain.seed); w.f64(def.terrain.relief_m);
        w.f64(def.terrain.base_frequency); w.u32(static_cast<std::uint32_t>(def.terrain.octaves));
        w.f64(def.terrain.lacunarity); w.f64(def.terrain.gain);
        w.u32(static_cast<std::uint32_t>(def.stamps.size()));
        for (const auto& st : def.stamps) w.stamp(st);
        w.u32(static_cast<std::uint32_t>(def.pois.size()));
        for (const auto& p : def.pois) {
            w.str(p.name); w.u8(static_cast<std::uint8_t>(p.kind)); w.stamp(p.stamp);
            w.f64(p.footprint_m);
        }
        w.u32(static_cast<std::uint32_t>(def.biomes.size()));
        for (const auto& bb : def.biomes) {
            w.f64(bb.lo_m); w.f64(bb.hi_m);
            w.f64(bb.color[0]); w.f64(bb.color[1]); w.f64(bb.color[2]);
            w.f64(bb.roughness); w.f64(bb.scatter_density);
        }
        w.strs(def.tags);
    } else if (def.world_class == WorldClass::station) {
        w.str(def.hull_def_ref);
        w.str(def.parent_ref);
        const auto& r = def.rails;
        w.f64(r.orbit_period_s); w.f64(r.orbit_radius_m); w.f64(r.orbit_phase_rad);
        w.f64(r.spin_axis.x); w.f64(r.spin_axis.y); w.f64(r.spin_axis.z);
        w.f64(r.spin_rate_rad_s);
        w.str(def.root_socket);
        w.u32(static_cast<std::uint32_t>(def.build_plans.size()));
        for (const auto& p : def.build_plans) {
            w.str(p.id);
            w.u32(static_cast<std::uint32_t>(p.steps.size()));
            for (const auto& s : p.steps) {
                w.str(s.module_def_ref); w.str(s.target_socket); w.f64(s.at_time_s);
            }
        }
        w.strs(def.tags);
    } else {
        w.u64(def.seed);
        w.strs(def.members);
        w.u32(static_cast<std::uint32_t>(def.factions.size()));
        for (const auto& f : def.factions) {
            w.str(f.name); w.f64(f.power_production); w.strs(f.tags);
        }
        w.u32(static_cast<std::uint32_t>(def.markets.size()));
        for (const auto& m : def.markets) {
            w.str(m.id); w.str(m.anchor_ref);
            w.u32(static_cast<std::uint32_t>(m.commodities.size()));
            for (const auto& c : m.commodities) { w.str(c.commodity); w.f64(c.production); }
        }
        w.u32(static_cast<std::uint32_t>(def.lanes.size()));
        for (const auto& l : def.lanes) {
            w.str(l.id); w.str(l.from); w.str(l.to); w.str(l.commodity);
            w.f64(l.flow_rate);
        }
        w.u32(static_cast<std::uint32_t>(def.waypoints.size()));
        for (const auto& n : def.waypoints) {
            w.str(n.id); w.u8(static_cast<std::uint8_t>(n.kind)); w.str(n.body_ref);
            w.f64(n.offset[0]); w.f64(n.offset[1]); w.f64(n.offset[2]);
        }
        w.u32(static_cast<std::uint32_t>(def.nav_edges.size()));
        for (const auto& e : def.nav_edges) {
            w.u8(static_cast<std::uint8_t>(e.kind)); w.str(e.from); w.str(e.to);
        }
        w.u32(static_cast<std::uint32_t>(def.job_templates.size()));
        for (const auto& j : def.job_templates) {
            w.str(j.id);
            w.u32(static_cast<std::uint32_t>(j.steps.size()));
            for (const auto& s : j.steps) { w.str(s.verb); w.str(s.target_ref); w.f64(s.amount); }
        }
    }
    return std::move(w.b);
}

std::expected<WorldDef, std::string> decode_world(const core::cooked::Bytes& payload) {
    Reader r{payload};
    WorldDef def;
    if (r.u32() != 1) return std::unexpected("WRLD payload_ver must be 1");
    const std::uint8_t cls = r.u8();
    if (cls > 2) return std::unexpected("WRLD world_class byte out of range");
    def.world_class = static_cast<WorldClass>(cls);
    def.name = r.str();

    switch (def.world_class) {
        case WorldClass::body: {
            def.parent_ref = r.str();
            auto& b = def.rails;
            b.surface_radius = r.f64(); b.well_radius = r.f64();
            b.surface_g = r.f64(); b.well_taper_start = r.f64();
            b.atm_rho0_kg_m3 = r.f64(); b.atm_scale_height_m = r.f64();
            b.orbit_period_s = r.f64(); b.orbit_radius_m = r.f64();
            b.orbit_phase_rad = r.f64();
            b.spin_axis = {r.f64(), r.f64(), r.f64()};
            b.spin_rate_rad_s = r.f64();
            b.safe_bubble_radius = r.f64(); b.safe_speed = r.f64();
            def.safe_impact = r.f64();
            b.parent = -1;  // refs resolve at assembly; payload never carries an index
            def.terrain.seed = r.u64(); def.terrain.relief_m = r.f64();
            def.terrain.base_frequency = r.f64();
            def.terrain.octaves = static_cast<int>(r.u32());
            def.terrain.lacunarity = r.f64(); def.terrain.gain = r.f64();
            for (auto n = r.u32(); n-- && !r.bad;) def.stamps.push_back(r.stamp());
            for (auto n = r.u32(); n-- && !r.bad;) {
                WorldPoi p;
                p.name = r.str();
                p.kind = static_cast<PoiKind>(r.u8());
                p.stamp = r.stamp();
                p.footprint_m = r.f64();
                def.pois.push_back(std::move(p));
            }
            for (auto n = r.u32(); n-- && !r.bad;) {
                WorldBiomeBand bb;
                bb.lo_m = r.f64(); bb.hi_m = r.f64();
                bb.color[0] = r.f64(); bb.color[1] = r.f64(); bb.color[2] = r.f64();
                bb.roughness = r.f64(); bb.scatter_density = r.f64();
                def.biomes.push_back(bb);
            }
            def.tags = r.strs();
            if (!def.hull_def_ref.empty() || !def.root_socket.empty() ||
                !def.build_plans.empty() || def.seed != 0 || !def.members.empty() ||
                !def.factions.empty() || !def.markets.empty() || !def.lanes.empty() ||
                !def.waypoints.empty() || !def.nav_edges.empty() ||
                !def.job_templates.empty())
                return std::unexpected("WRLD body payload carries system/station sections");
            break;
        }
        case WorldClass::station: {
            def.hull_def_ref = r.str();
            def.parent_ref = r.str();
            auto& b = def.rails;
            b.orbit_period_s = r.f64(); b.orbit_radius_m = r.f64();
            b.orbit_phase_rad = r.f64();
            b.spin_axis = {r.f64(), r.f64(), r.f64()};
            b.spin_rate_rad_s = r.f64();
            b.parent = -1;
            def.root_socket = r.str();
            for (auto n = r.u32(); n-- && !r.bad;) {
                WorldBuildPlan p;
                p.id = r.str();
                for (auto m = r.u32(); m-- && !r.bad;) {
                    WorldBuildStep s;
                    s.module_def_ref = r.str();
                    s.target_socket = r.str();
                    s.at_time_s = r.f64();
                    p.steps.push_back(std::move(s));
                }
                def.build_plans.push_back(std::move(p));
            }
            def.tags = r.strs();
            if (!def.stamps.empty() || !def.pois.empty() || !def.biomes.empty() ||
                def.seed != 0 || !def.members.empty() || !def.factions.empty() ||
                !def.markets.empty() || !def.lanes.empty() || !def.waypoints.empty() ||
                !def.nav_edges.empty() || !def.job_templates.empty())
                return std::unexpected("WRLD station payload carries system/body sections");
            break;
        }
        case WorldClass::system: {
            def.seed = r.u64();
            def.members = r.strs();
            for (auto n = r.u32(); n-- && !r.bad;) {
                WorldFaction f;
                f.name = r.str(); f.power_production = r.f64(); f.tags = r.strs();
                def.factions.push_back(std::move(f));
            }
            for (auto n = r.u32(); n-- && !r.bad;) {
                WorldMarketNode m;
                m.id = r.str(); m.anchor_ref = r.str();
                for (auto k = r.u32(); k-- && !r.bad;) {
                    WorldMarketCommodity c;
                    c.commodity = r.str(); c.production = r.f64();
                    m.commodities.push_back(std::move(c));
                }
                def.markets.push_back(std::move(m));
            }
            for (auto n = r.u32(); n-- && !r.bad;) {
                WorldLane l;
                l.id = r.str(); l.from = r.str(); l.to = r.str();
                l.commodity = r.str(); l.flow_rate = r.f64();
                def.lanes.push_back(std::move(l));
            }
            for (auto n = r.u32(); n-- && !r.bad;) {
                WorldNavNode w;
                w.id = r.str();
                w.kind = static_cast<NavKind>(r.u8());
                w.body_ref = r.str();
                w.offset[0] = r.f64(); w.offset[1] = r.f64(); w.offset[2] = r.f64();
                def.waypoints.push_back(std::move(w));
            }
            for (auto n = r.u32(); n-- && !r.bad;) {
                WorldNavEdge e;
                e.kind = static_cast<NavKind>(r.u8());
                e.from = r.str(); e.to = r.str();
                def.nav_edges.push_back(std::move(e));
            }
            for (auto n = r.u32(); n-- && !r.bad;) {
                WorldJobTemplate j;
                j.id = r.str();
                for (auto k = r.u32(); k-- && !r.bad;) {
                    WorldJobStep s;
                    s.verb = r.str(); s.target_ref = r.str(); s.amount = r.f64();
                    j.steps.push_back(std::move(s));
                }
                def.job_templates.push_back(std::move(j));
            }
            if (!def.stamps.empty() || !def.pois.empty() || !def.biomes.empty() ||
                !def.hull_def_ref.empty() || !def.root_socket.empty() ||
                !def.build_plans.empty() || !def.parent_ref.empty())
                return std::unexpected("WRLD system payload carries body/station sections");
            break;
        }
        default:
            return std::unexpected("WRLD world_class byte out of range");
    }
    if (r.bad || r.at != payload.size())
        return std::unexpected("WRLD payload size mismatch (trailing/truncated bytes)");
    return def;
}

}  // namespace ve::game
