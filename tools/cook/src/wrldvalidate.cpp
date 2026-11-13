// Phase 8 task 8.8 — world-def cross-field / cross-file validation gates.
// See ve/cook/wrldvalidate.h for the gate doctrine and fixed order.
#include "ve/cook/wrldvalidate.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>

namespace ve::cook {
namespace {

bool starts_with(const std::string& s, const std::string_view p) {
    return s.rfind(p, 0) == 0;
}

// Meter values in gate text carry no false precision (deterministic bytes).
std::string m_num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", v);
    return buf;
}

} // namespace

bool cook_wrld_validate(const std::string& self_id,
                        const ve::game::WorldDef& self,
                        const std::map<std::string, ve::game::WorldDef>& wrld_deps,
                        const std::map<std::string, HullInputs>& hulls,
                        const RefResolve& resolve,
                        std::string* err) {
    using namespace ve::game;
    auto fail = [&](const std::string& msg) {
        if (err) *err = "world '" + self_id + "': " + msg;
        return false;
    };

    // ---- rail parent chain: bottoms at the root anchor, no cycle (#45c) ----
    // (body + station classes; a system def IS the root, no chain to walk).
    if (self.world_class == WorldClass::body || self.world_class == WorldClass::station) {
        std::set<std::string> visited{self_id};
        std::string cur = self_id;
        for (;;) {
            const WorldDef* dp = nullptr;
            if (cur == self_id) dp = &self;
            else if (auto it = wrld_deps.find(cur); it != wrld_deps.end()) dp = &it->second;
            else
                return fail("rail parent '" + cur + "' did not cook as a world def (#45c)");
            const WorldDef& d = *dp;
            if (d.parent_ref.empty()) break;  // chain bottoms at the root anchor (#45c)
            std::string pid;
            if (!resolve(cur, d.parent_ref, pid) || pid.empty())
                return fail("rail parent ref '" + d.parent_ref + "' escapes the source root (#45c)");
            if (visited.count(pid))
                return fail("rail parent cycle through '" + pid + "' (#45c)");
            visited.insert(pid);
            auto it = wrld_deps.find(pid);
            if (it == wrld_deps.end())
                return fail("rail parent '" + pid + "' is not a cooked world def (#45c)");
            if (it->second.world_class != WorldClass::body)
                return fail(std::string("rail parent '") + pid + "' is a " +
                            world_class_name(it->second.world_class) +
                            " def; rails pin to body parents or the root anchor (#45c)");
            cur = pid;
        }
    }

    // ---- body: POI stamps cover their footprints; pads ride plateaus (#29c) ----
    if (self.world_class == WorldClass::body) {
        const double r = self.rails.surface_radius;
        for (const auto& poi : self.pois) {
            const auto& st = poi.stamp;
            // #29c: stamp radius is angular (radians of the sphere); the
            // footprint is a linear structure radius. Prove coverage in
            // meters so 8.11's runtime pad math cannot meet a stamp that
            // stops short of the pad it claims to prepare.
            const double stamp_m = st.radius_rad * r;
            if (!(stamp_m >= poi.footprint_m))
                return fail("POI '" + poi.name + "': stamp radius " + m_num(stamp_m) +
                            " m does not cover footprint " + m_num(poi.footprint_m) +
                            " m (#29c)");
            if (poi.kind == PoiKind::pad) {
                if (st.type == core::sim::stamp_type::crater)
                    return fail("pad POI '" + poi.name +
                                "': pad stamp must be flatten or mesa, not crater (#29c)");
                // #29c pad-normal rule by proof, not tolerance: the stamp's
                // full-weight core is bit-constant height (terrain.h stamp
                // weight), so anywhere the footprint sits inside that core
                // the surface plane equals the sphere tangent — parallel to
                // the radial well normal by construction. blend is the blend
                // RING fraction of the radius, so the core radius is
                // (1 - blend) * stamp_m.
                const double core_m = (1.0 - st.blend) * stamp_m;
                if (!(core_m >= poi.footprint_m))
                    return fail("pad POI '" + poi.name + "': blend " + std::to_string(st.blend) +
                                " leaves a " + m_num(core_m) + " m plateau under a " +
                                m_num(poi.footprint_m) +
                                " m footprint; the pad plane must sit in the stamp's " +
                                "full-weight core to stay parallel to the well normal (#29c)");
            }
        }
    }

    // ---- body [safe] gates (#50, 8.13), fixed order, first failure ----
    // A bubble must reach past the well edge AND the surface (wall outside
    // well outside body), needs its speed ceiling, and the impact value is
    // a plain non-negative magnitude. All keys default 0 = no bubble.
    if (self.world_class == WorldClass::body) {
        const auto& sf = self.rails;
        if (sf.safe_bubble_radius < 0.0 || sf.safe_speed < 0.0 || self.safe_impact < 0.0)
            return fail("world '" + self_id + "': [safe] fields must be >= 0");
        if (sf.safe_bubble_radius > 0.0) {
            if (!(sf.safe_speed > 0.0))
                return fail("world '" + self_id + "': [safe].bubble set without [safe].speed");
            if (sf.safe_bubble_radius < sf.well_radius)
                return fail("world '" + self_id + "': [safe].bubble " + m_num(sf.safe_bubble_radius) +
                            " m is inside well_radius " + m_num(sf.well_radius) +
                            " m — the warp wall cannot sit inside the gravity well");
            if (sf.safe_bubble_radius < sf.surface_radius)
                return fail("world '" + self_id + "': [safe].bubble " + m_num(sf.safe_bubble_radius) +
                            " m is inside surface_radius " + m_num(sf.surface_radius) +
                            " m — the warp wall cannot sit inside the terrain");
        }
    }

    // ---- system: the map graph itself (#20/#25/#27/#33/#39/#45) ----
    if (self.world_class == WorldClass::system) {
        // Members resolve and cooked as world defs (S1).
        std::set<std::string> member_ids;
        for (const auto& ref : self.members) {
            std::string mid;
            if (!resolve(self_id, ref, mid) || mid.empty())
                return fail("member ref '" + ref + "' escapes the source root (#27a)");
            if (!wrld_deps.count(mid))
                return fail("member '" + mid + "' is not a cooked world def (#45d)");
            if (!member_ids.insert(mid).second)
                return fail("duplicate member '" + mid + "' (#45d)");
        }

        // Member rail parents are members of THIS system (#39 rails graph).
        for (const auto& mid : member_ids) {
            const WorldDef& w = wrld_deps.at(mid);
            if (w.parent_ref.empty()) continue;  // root-pinned member (#45c)
            std::string pid;
            if (!resolve(mid, w.parent_ref, pid) || pid.empty())
                return fail("member '" + mid + "' parent ref '" + w.parent_ref +
                            "' escapes the source root (#45c)");
            if (!member_ids.count(pid))
                return fail("member '" + mid + "' rail parent '" + pid +
                            "' is not a member of this system (#39)");
        }

        // Faction name set first: owner/stance tags reference it (#25a/b).
        std::set<std::string> factions;
        for (const auto& f : self.factions) {
            if (f.name.empty()) return fail("faction with empty name (#25a)");
            if (!factions.insert(f.name).second)
                return fail("duplicate faction '" + f.name + "' (#25a)");
        }
        for (const auto& mid : member_ids) {
            for (const auto& tag : wrld_deps.at(mid).tags) {
                if (!starts_with(tag, "owner.")) continue;
                const std::string fac = tag.substr(6);
                if (fac.empty() || !factions.count(fac))
                    return fail("member '" + mid + "' owner tag '" + tag +
                                "' names undefined faction '" + fac + "' (#25a)");
            }
        }
        for (const auto& f : self.factions) {
            for (const auto& tag : f.tags) {
                if (!starts_with(tag, "stance.")) continue;
                const std::string rest = tag.substr(7);
                const auto dot = rest.find('.');
                if (dot == std::string::npos || dot == 0 || dot + 1 >= rest.size())
                    return fail("faction '" + f.name + "' stance tag '" + tag +
                                "' must be stance.<verb>.<faction> (#25b)");
                const std::string target = rest.substr(dot + 1);
                if (target == f.name)
                    return fail("faction '" + f.name + "' stance tag '" + tag +
                                "' targets itself (#25b)");
                if (!factions.count(target))
                    return fail("faction '" + f.name + "' stance tag '" + tag +
                                "' names undefined faction '" + target + "' (#25b)");
            }
        }

        // Market nodes: unique ids; anchors resolve to a member def or a
        // member body's POI (#45d, #29c, #20b).
        std::set<std::string> market_ids;
        for (const auto& mk : self.markets) {
            if (mk.id.empty()) return fail("market node with empty id (#20b)");
            if (!market_ids.insert(mk.id).second)
                return fail("duplicate market node '" + mk.id + "' (#20b)");
            if (mk.anchor_ref.empty())
                return fail("market '" + mk.id + "' has no anchor (#45d)");
            if (starts_with(mk.anchor_ref, "poi:")) {
                const std::string poi_name = mk.anchor_ref.substr(4);
                bool found = false;
                for (const auto& mid : member_ids)
                    for (const auto& poi : wrld_deps.at(mid).pois)
                        if (poi.name == poi_name) found = true;
                if (!found)
                    return fail("market '" + mk.id + "' POI anchor '" + mk.anchor_ref +
                                "' is defined by no member body (#29c)");
            } else {
                std::string aid;
                if (!resolve(self_id, mk.anchor_ref, aid) || aid.empty())
                    return fail("market '" + mk.id + "' anchor '" + mk.anchor_ref +
                                "' escapes the source root (#45d)");
                if (!member_ids.count(aid))
                    return fail("market '" + mk.id + "' anchor '" + aid +
                                "' is not a member of this system (#45d)");
            }
        }

        // Lanes: endpoints name market nodes, flow > 0, and the commodity
        // is actually produced at the from-market (#20b).
        for (const auto& ln : self.lanes) {
            if (ln.id.empty()) return fail("lane with empty id (#20b)");
            if (!market_ids.count(ln.from))
                return fail("lane '" + ln.id + "' from '" + ln.from +
                            "' names no market node (#20b)");
            if (!market_ids.count(ln.to))
                return fail("lane '" + ln.id + "' to '" + ln.to +
                            "' names no market node (#20b)");
            if (!(ln.flow_rate > 0.0) || !std::isfinite(ln.flow_rate))
                return fail("lane '" + ln.id + "' flow_rate must be > 0 (#20b)");
            const WorldMarketNode* from_mk = nullptr;
            for (const auto& mk : self.markets)
                if (mk.id == ln.from) from_mk = &mk;
            bool produced = false;
            for (const auto& c : from_mk->commodities)
                if (c.commodity == ln.commodity && c.production > 0.0) produced = true;
            if (!produced)
                return fail("lane '" + ln.id + "' ships '" + ln.commodity +
                            "' but market '" + ln.from + "' produces no such commodity (#20b)");
        }

        // Nav graph: waypoint bodies are members; edge endpoints name a
        // market or waypoint node (#27a — dangling edges fail the cook).
        std::set<std::string> node_ids = market_ids;
        for (const auto& wp : self.waypoints) {
            if (wp.id.empty()) return fail("waypoint with empty id (#27a)");
            if (!node_ids.insert(wp.id).second)
                return fail("duplicate nav node '" + wp.id + "' (#27a)");
            if (wp.body_ref.empty()) continue;  // fixed point in system frame
            std::string bid;
            if (!resolve(self_id, wp.body_ref, bid) || bid.empty())
                return fail("waypoint '" + wp.id + "' body ref '" + wp.body_ref +
                            "' escapes the source root (#27a)");
            if (!member_ids.count(bid))
                return fail("waypoint '" + wp.id + "' body '" + bid +
                            "' is not a member of this system (#27a)");
        }
        for (const auto& e : self.nav_edges) {
            if (!node_ids.count(e.from))
                return fail("nav edge '" + e.from + "'->'" + e.to +
                            "' from-endpoint names no market or waypoint node (#27a)");
            if (!node_ids.count(e.to))
                return fail("nav edge '" + e.from + "'->'" + e.to +
                            "' to-endpoint names no market or waypoint node (#27a)");
        }

        // Job templates: step targets name a node, lane or member station
        // ('' = no target, e.g. undock) (#33a).
        std::set<std::string> lane_ids;
        for (const auto& ln : self.lanes) lane_ids.insert(ln.id);
        for (const auto& j : self.job_templates) {
            if (j.id.empty()) return fail("job template with empty id (#33a)");
            for (std::size_t i = 0; i < j.steps.size(); ++i) {
                const auto& s = j.steps[i];
                if (s.target_ref.empty()) continue;
                if (!node_ids.count(s.target_ref) && !lane_ids.count(s.target_ref) &&
                    !member_ids.count(s.target_ref))
                    return fail("job '" + j.id + "' step " + std::to_string(i) +
                                " target '" + s.target_ref +
                                "' names no node, lane or station (#33a)");
            }
        }
    }

    // ---- station: hull pin + build-plan clamp pairing (#24a/#24b/#46d) ----
    if (self.world_class == WorldClass::station) {
        std::string hid;
        if (self.hull_def_ref.empty() || !resolve(self_id, self.hull_def_ref, hid) ||
            hid.empty())
            return fail("station hull def ref '" + self.hull_def_ref +
                        "' escapes the source root (#24a)");
        auto hit = hulls.find(hid);
        if (hit == hulls.end())
            return fail("station hull '" + hid + "' is not a cooked hull def (#24a)");
        const HullInputs& hull = hit->second;

        if (!self.root_socket.empty()) {
            bool hp = false;
            for (const auto& sv : hull.socks)
                for (const auto& a : sv.anchors)
                    if (a.kind == 0 && a.name == self.root_socket) hp = true;
            if (!hp)
                return fail("root socket '" + self.root_socket +
                            "' is not a hardpoint on the station hull (#46d)");
        }

        std::set<std::string> claimed_sockets;
        for (const auto& plan : self.build_plans) {
            if (plan.id.empty()) return fail("build plan with empty id (#24b)");
            for (const auto& s : plan.steps) {
                std::string mid;
                if (s.module_def_ref.empty() ||
                    !resolve(self_id, s.module_def_ref, mid) || mid.empty())
                    return fail("build plan '" + plan.id +
                                "' module ref escapes the source root (#24b)");
                auto mit = hulls.find(mid);
                if (mit == hulls.end())
                    return fail("build plan '" + plan.id + "': module '" + mid +
                                "' is not a cooked hull def (#24b)");
                // #24b/#46d: the step is executable only if the module's
                // OWN clamp seam pairs the step socket with THIS station
                // hull. hullvalidate.cpp already proved those seam rows
                // symmetric, lattice-compatible and on exterior cells; this
                // binds the plan onto a proven pair.
                const HullInputs& mod = mit->second;
                bool paired = false;
                for (std::size_t i = 0; i < mod.def.seams.size(); ++i) {
                    const auto& seam = mod.def.seams[i];
                    const std::string pid =
                        i < mod.seam_partner_ids.size() ? mod.seam_partner_ids[i] : "";
                    if (seam.socket == s.target_socket && pid == hid) paired = true;
                }
                if (!paired)
                    return fail("build plan '" + plan.id + "': module '" + mid +
                                "' declares no clamp seam pairing socket '" +
                                s.target_socket + "' with this station hull (#24b/#46d)");
                if (!(s.at_time_s > 0.0) || !std::isfinite(s.at_time_s))
                    return fail("build plan '" + plan.id +
                                "': step at_time must be > 0 (#24b)");
                if (!self.root_socket.empty() && s.target_socket == self.root_socket)
                    return fail("build plan '" + plan.id + "' mounts a module on the "
                                "station's own root socket '" + self.root_socket +
                                "' — the station's pin stays clear (#24a/#24b)");
                if (!claimed_sockets.insert(s.target_socket).second)
                    return fail("build plans put two modules on socket '" +
                                s.target_socket + "' (#24b)");
            }
        }
    }

    if (err) err->clear();
    return true;
}

} // namespace ve::cook
