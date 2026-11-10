// Phase 5 task 5.9b — hull cross-validation. See hullvalidate.h for the
// gate list; PLAN #44 (cook gates), #46a-e (conventions), #26a/#28a/#46d.
#include "ve/cook/hullvalidate.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <string_view>

namespace ve::cook {
namespace {

// ---- payload reading (little-endian, bounds-checked) --------------------
struct Reader {
    const Bytes& b;
    std::size_t i = 0;
    bool bad = false;

    std::uint8_t u8() {
        if (i + 1 > b.size()) { bad = true; return 0; }
        return b[i++];
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(u8()) << (8 * k);
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    double f64() {
        if (i + 8 > b.size()) { bad = true; return 0.0; }
        double v;
        std::memcpy(&v, &b[i], 8);
        i += 8;
        return v;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (bad || i + n > b.size()) { bad = true; return {}; }
        std::string s(reinterpret_cast<const char*>(&b[i]), n);
        i += n;
        return s;
    }
};

void put32(Bytes& p, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) p.push_back(static_cast<std::uint8_t>((v >> (8 * k)) & 0xFF));
}
void put_f64(Bytes& p, double v) {
    std::uint8_t bits[8];
    std::memcpy(bits, &v, 8);
    p.insert(p.end(), bits, bits + 8);
}
void put_str(Bytes& p, const std::string& s) {
    put32(p, static_cast<std::uint32_t>(s.size()));
    p.insert(p.end(), s.begin(), s.end());
}

// ---- grid helpers --------------------------------------------------------
using Cell = std::array<std::int32_t, 3>;

Cell cell_of(const SockAnchor& a, const DefView& d) {
    // floor-divide: cells tile all of space; exact multiples land exactly.
    return {static_cast<std::int32_t>(std::floor(a.t[0] / d.cell_m)),
            static_cast<std::int32_t>(std::floor(a.t[1] / d.cell_height_m)),
            static_cast<std::int32_t>(std::floor(a.t[2] / d.cell_m))};
}

bool cell_in_box(const Cell& c, const std::array<std::int32_t, 6>& box) {
    for (int ax = 0; ax < 3; ++ax)
        if (c[ax] < box[ax] || c[ax] > box[ax + 3]) return false;
    return true;
}

// face byte -> unit step per axis (#46c1 encoding; engine axes, #34).
const int kFaceDir[7][3] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const char* kFaceName[7] = {"none", "+x", "-x", "+y", "-y", "+z", "-z"};

bool is_door_type(std::string_view t) { return t.rfind("door.", 0) == 0; }
bool is_airlock_type(std::string_view t) { return t.rfind("airlock.", 0) == 0; }

} // namespace

bool decode_def_payload(const Bytes& payload, DefView& out, std::string* err) {
    Reader r{payload};
    if (r.u32() != 2u) { if (err) *err = "DEF0 payload version must be 2"; return false; }
    out.class_tag = r.str();
    if (r.u8()) r.f64();               // hull_mass (validated at 5.10 loadout use)
    if (r.u8()) { r.f64(); r.f64(); r.f64(); }
    if (r.u8()) { r.f64(); r.f64(); r.f64(); }
    out.cell_m = r.f64();
    out.subcell_ratio = r.u32();
    out.cell_height_m = r.f64();
    out.tolerance_m = r.f64();
    const std::uint32_t nsec = r.u32();
    for (std::uint32_t s = 0; s < nsec && !r.bad; ++s) {
        DefView::Section sec;
        sec.name = r.str();
        const std::uint32_t nbox = r.u32();
        sec.boxes.push_back({});
        // 5.8 layout: exactly one flat 6-int box per box row; rows >1 are
        // stored as separate Section copies with the SAME name.
        for (std::uint32_t b = 0; b < nbox && !r.bad; ++b) {
            if (b) sec.boxes.push_back({});
            for (int k = 0; k < 6; ++k) sec.boxes[b][k] = r.i32();
        }
        out.sections.push_back(std::move(sec));
    }
    const std::uint32_t ncomp = r.u32();
    for (std::uint32_t c = 0; c < ncomp && !r.bad; ++c) {
        DefView::Comp comp;
        comp.id = r.str();
        comp.type = r.str();
        comp.category = r.u8();
        comp.socket = r.str();
        comp.face = r.u8();
        const std::uint32_t ntag = r.u32();
        for (std::uint32_t t = 0; t < ntag && !r.bad; ++t) r.str();  // tags: not gated in Phase 5
        const std::uint32_t nattr = r.u32();
        for (std::uint32_t a = 0; a < nattr && !r.bad; ++a) {
            // Separate statements: argument evaluation order in one call is
            // unspecified, and the Reader is stateful (right-to-left GCC
            // would consume the f64 before the str).
            std::string key = r.str();
            comp.attrs.emplace_back(std::move(key), r.f64());  // #47b: gated by G10
        }
        const std::uint32_t nfac = r.u32();
        for (std::uint32_t f = 0; f < nfac && !r.bad; ++f) {
            DefView::Facet fac;
            fac.action = r.str();
            r.f64();  // range_m (interaction reach; Phase 6)
            r.u8();  // manual
            fac.has_target = r.u8();
            if (fac.has_target) fac.target = r.str();
            comp.facets.push_back(std::move(fac));
        }
        out.comps.push_back(std::move(comp));
    }
    const std::uint32_t ncat = r.u32();  // catalog: re-encodes comps; skip
    for (std::uint32_t k = 0; k < ncat && !r.bad; ++k) { r.str(); r.u8(); }
    const std::uint32_t nwire = r.u32();
    for (std::uint32_t w = 0; w < nwire && !r.bad; ++w) {
        DefView::Wire wire;
        wire.id = r.str();
        wire.type = r.u8();
        if (r.u8()) r.f64();
        wire.from_kind = r.u8();
        wire.from_ref = r.str();
        wire.to_kind = r.u8();
        wire.to_ref = r.str();
        out.wires.push_back(std::move(wire));
    }
    const std::uint32_t nseam = r.u32();
    for (std::uint32_t s = 0; s < nseam && !r.bad; ++s) {
        DefView::Seam seam;
        seam.partner_ref = r.str();
        seam.socket = r.str();
        if (r.u8()) { r.f64(); r.f64(); r.f64(); }
        out.seams.push_back(std::move(seam));
    }
    const std::uint32_t nref = r.u32();  // refs: manifest handles deps
    for (std::uint32_t k = 0; k < nref && !r.bad; ++k) r.str();
    if (r.bad) { if (err) *err = "DEF0 payload truncated/malformed"; return false; }
    return true;
}

bool decode_sock_payload(const Bytes& payload, SockView& out, std::string* err) {
    Reader r{payload};
    out.mesh_id = r.str();
    if (r.u32() != 2u) { if (err) *err = "SOCK payload version must be 2"; return false; }
    const std::uint32_t n = r.u32();
    for (std::uint32_t k = 0; k < n && !r.bad; ++k) {
        SockAnchor a;
        a.name = r.str();
        a.kind = r.u8();
        a.category = r.u8();
        for (double& v : a.t) v = r.f64();
        for (double& v : a.q) v = r.f64();
        out.anchors.push_back(std::move(a));
    }
    if (r.bad) { if (err) *err = "SOCK payload truncated/malformed"; return false; }
    return true;
}

Bytes encode_sock_payload(const SockView& in) {
    Bytes p;
    put_str(p, in.mesh_id);
    put32(p, 2u);
    put32(p, static_cast<std::uint32_t>(in.anchors.size()));
    for (const auto& a : in.anchors) {
        put_str(p, a.name);
        p.push_back(a.kind);
        p.push_back(a.category);
        for (double v : a.t) put_f64(p, v);
        for (double v : a.q) put_f64(p, v);
    }
    return p;
}

bool cook_hull_validate(const HullInputs& hull,
                        const std::map<std::string, HullInputs>& partners,
                        std::vector<SockView>& snapped_out,
                        std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = hull.def_id + ": " + msg;
        return false;
    };
    const DefView& d = hull.def;

    // ---- G1: door/airlock components carry an explicit face (#46c1) ----
    for (const auto& c : d.comps)
        if ((is_door_type(c.type) || is_airlock_type(c.type)) && c.face == 0)
            return fail("component '" + c.id + "' (type '" + c.type +
                        "') missing required 'face' axis (#46c1)");

    // ---- G2: every component socket names a real anchor (any kind) ----
    //      (interior kind + id pairing in G3)
    std::map<std::string, const SockAnchor*> anchor_of;
    for (const auto& sk : hull.socks)
        for (const auto& a : sk.anchors)
            anchor_of[a.name] = &a;  // glb cook already failed on dup names
    for (const auto& c : d.comps)
        if (!anchor_of.count(c.socket))
            return fail("component '" + c.id + "' socket '" + c.socket +
                        "' names no anchor in any cooked SOCK (#46a)");

    // ---- G3: #46a named-empty pairing is 1:1 --------------------------
    //  (a) a component mounted on an INTERIOR anchor pairs by id: comp.id
    //      must equal the anchor name (the empty's name is the pairing key).
    //  (b) every interior anchor (kind 1..5) pairs with EXACTLY ONE
    //      component (via comp.socket == name or comp.id == name) — no
    //      dangling art empties, no double-pairing.
    for (const auto& c : d.comps) {
        const SockAnchor* a = anchor_of[c.socket];
        if (a->kind >= 1 && 5 >= a->kind && c.id != a->name)
            return fail("component '" + c.id + "' mounts interior anchor '" + a->name +
                        "' but pairing ids differ (#46a)");
    }
    for (const auto& sk : hull.socks)
        for (const auto& a : sk.anchors) {
            if (a.kind == 0) continue;  // hardpoints may be unoccupied
            std::size_t n = 0;
            for (const auto& c : d.comps)
                if (c.socket == a.name || c.id == a.name) ++n;
            if (n != 1)
                return fail("interior anchor '" + a.name + "' paired with " +
                            std::to_string(n) + " def components (need exactly 1, #46a)");
        }

    // ---- G4: lattice snap within tolerance, rewrite coords (#46b) -----
    snapped_out = hull.socks;
    for (auto& sk : snapped_out)
        for (auto& a : sk.anchors) {
            // primary anchors (hp/door/berth) ride the CELL lattice;
            // furnishings (seat/panel/counter) the SUBCELL lattice (#46b).
            const bool subcell = a.kind == 2 || a.kind == 3 || a.kind == 4;
            const double lat_xz = d.subcell_ratio ? d.cell_m / d.subcell_ratio : d.cell_m;
            const double lat_y = d.subcell_ratio ? d.cell_height_m / d.subcell_ratio : d.cell_height_m;
            const double lat[3] = {subcell ? lat_xz : d.cell_m,
                                   subcell ? lat_y : d.cell_height_m,
                                   subcell ? lat_xz : d.cell_m};
            for (int ax = 0; ax < 3; ++ax) {
                const double q = std::round(a.t[ax] / lat[ax]) * lat[ax];
                if (std::fabs(a.t[ax] - q) > d.tolerance_m + 1e-9)
                    return fail("anchor '" + a.name + "' coord " + std::to_string(ax) +
                                " off the " + (subcell ? "subcell" : "cell") +
                                " lattice beyond tolerance (#46b)");
                a.t[ax] = q;  // runtime only ever sees perfect lattice coords
            }
        }

    for (auto& sk : snapped_out)
        for (auto& a : sk.anchors)
            anchor_of[a.name] = &a;  // re-point at snapped coords (G5/G6/G9)

    // ---- G5: every anchor lands in EXACTLY ONE section (#44) ----------
    auto sections_containing = [&](const Cell& c) {
        std::vector<std::size_t> hit;
        for (std::size_t s = 0; s < d.sections.size(); ++s)
            for (const auto& box : d.sections[s].boxes)
                if (cell_in_box(c, box)) { hit.push_back(s); break; }
        return hit;
    };
    for (const auto& sk : snapped_out)
        for (const auto& a : sk.anchors) {
            const auto hit = sections_containing(cell_of(a, d));
            if (hit.size() != 1)
                return fail("anchor '" + a.name + "' is in " + std::to_string(hit.size()) +
                            " sections (must be exactly 1, #44)");
        }

    // ---- G6: door/airlock facing (#46c, strict per 5.9b checkpoint) ----
    // A door joins two DIFFERENT sections; an opening whose opposite cell is
    // in NO section must be declared airlock.*; an airlock's opposite cell
    // must be in NO section.
    for (const auto& c : d.comps) {
        const bool door = is_door_type(c.type), airlock = is_airlock_type(c.type);
        if (!door && !airlock) continue;
        const Cell cell = cell_of(*anchor_of[c.socket], d);
        const std::size_t own = sections_containing(cell).size();
        if (own != 1) continue;  // G5 already failed for the anchor path
        Cell nb = cell;
        for (int ax = 0; ax < 3; ++ax) nb[ax] += kFaceDir[c.face][ax];
        const auto hit = sections_containing(nb);
        if (door) {
            if (hit.empty())
                return fail("door '" + c.id + "' face '" + kFaceName[c.face] +
                            "' opens to NO section — declare it airlock.* (#46c)");
            if (hit.size() != 1)
                return fail("door '" + c.id + "' neighbor cell is in " + std::to_string(hit.size()) +
                            " sections (#46c)");
            if (d.sections[hit[0]].name == d.sections[sections_containing(cell)[0]].name)
                return fail("door '" + c.id + "' faces a cell inside its own section (#46c)");
        } else {  // airlock
            if (!hit.empty())
                return fail("airlock '" + c.id + "' faces cell inside section '" +
                            d.sections[hit[0]].name + "' — use door.* (#46c)");
        }
    }

    // ---- G7: wire endpoints exist (#26a) -------------------------------
    std::map<std::string, bool> comp_id;
    for (const auto& c : d.comps) comp_id[c.id] = true;
    std::map<std::string, bool> hardpoint;
    for (const auto& sk : hull.socks)
        for (const auto& a : sk.anchors)
            if (a.kind == 0) hardpoint[a.name] = true;
    for (const auto& w : d.wires) {
        const bool from_ok = w.from_kind ? comp_id.count(w.from_ref) : hardpoint.count(w.from_ref);
        const bool to_ok = w.to_kind ? comp_id.count(w.to_ref) : hardpoint.count(w.to_ref);
        if (!from_ok) return fail("wire '" + w.id + "' from '" + w.from_ref +
                                  "' names no " + (w.from_kind ? "component" : "hardpoint") + " (#26a)");
        if (!to_ok) return fail("wire '" + w.id + "' to '" + w.to_ref +
                                "' names no " + (w.to_kind ? "component" : "hardpoint") + " (#26a)");
    }

    // ---- G8: facet targets name real wires (#28a) ----------------------
    std::map<std::string, bool> wire_id;
    for (const auto& w : d.wires) wire_id[w.id] = true;
    for (const auto& c : d.comps)
        for (const auto& f : c.facets)
            if (f.has_target && !wire_id.count(f.target))
                return fail("component '" + c.id + "' facet '" + f.action + "' target '" +
                            f.target + "' names no wire (#28a)");

    // ---- G9: seam rules (#46d) -----------------------------------------
    if (hull.seam_partner_ids.size() != d.seams.size())
        return fail("internal: seam partner ids not parallel to seam rows");
    for (std::size_t s = 0; s < d.seams.size(); ++s) {
        const auto& seam = d.seams[s];
        const std::string& pid = hull.seam_partner_ids[s];
        auto it = partners.find(pid);
        if (pid.empty() || it == partners.end())
            return fail("seam to '" + seam.partner_ref + "': partner def not cooked/manifested (#46d)");
        const HullInputs& P = it->second;
        // shared socket must exist as a HARDPOINT anchor on BOTH hulls.
        auto has_hardpoint = [&](const HullInputs& h) {
            for (const auto& sk : h.socks)
                for (const auto& a : sk.anchors)
                    if (a.kind == 0 && a.name == seam.socket) return true;
            return false;
        };
        if (!has_hardpoint(hull))
            return fail("seam socket '" + seam.socket + "' is not a hardpoint anchor here (#46d)");
        if (!has_hardpoint(P))
            return fail("seam socket '" + seam.socket + "' not found in partner '" + pid + "' (#46d)");
        // the seam socket's cell must be on the hull EXTREME: some face
        // neighbor of its cell is in no section.
        const SockAnchor* sa = anchor_of[seam.socket];  // has_hardpoint checked above
        const Cell sc = cell_of(*sa, d);
        bool exterior = false;
        for (std::uint8_t f = 1; f <= 6 && !exterior; ++f) {
            Cell nb = sc;
            for (int ax = 0; ax < 3; ++ax) nb[ax] += kFaceDir[f][ax];
            if (sections_containing(nb).empty()) exterior = true;
        }
        if (!exterior)
            return fail("seam socket '" + seam.socket + "' cell is enclosed by sections (#46d)");
        // lattice compat: equal cell size or an integer multiple (#46d).
        const double a = d.cell_m, b = P.def.cell_m;
        const double hi = a > b ? a : b, lo = a > b ? b : a;
        const bool grid_ok = std::fabs(a - b) <= 1e-9 ||
                             (lo > 1e-9 && std::fabs(hi / lo - std::round(hi / lo)) <= 1e-9);
        if (!grid_ok)
            return fail("seam grid incompatible: cell " + std::to_string(a) + " vs partner " +
                        std::to_string(b) + " (#46d)");
        // symmetric back-declaration: partner declares a seam pointing back.
        bool back = false;
        for (const auto& pid2 : P.seam_partner_ids)
            if (pid2 == hull.def_id) { back = true; break; }
        if (!back)
            return fail("partner '" + pid + "' declares no seam back to this def (#46d)");
    }

    // ---- G10 (#47b): dock port component contract ----------------------
    // Dock gear is a ship component like everything else: utility-socket
    // mount, positive clamp strength + mass, and the undock verb. A
    // destroyed port stays dark-not-dead at RUNTIME (7.10); cook at least
    // refuses to author a port that could not physically hold on.
    for (const auto& c : d.comps) {
        if (c.type.compare(0, 5, "dock.") != 0) continue;
        if (c.category != 3)
            return fail("dock port '" + c.id + "' must mount a utility socket (#47b)");
        const auto ait = anchor_of.find(c.socket);
        if (ait != anchor_of.end() && ait->second->category != 3)
            return fail("dock port '" + c.id + "' socket '" + c.socket +
                        "' is not a utility anchor (#47b)");
        double clamp = 0.0, mass = 0.0;
        bool have_clamp = false, have_mass = false;
        for (const auto& [key, val] : c.attrs) {
            if (key == "dock.clamp_strength") { clamp = val; have_clamp = true; }
            else if (key == "mass") { mass = val; have_mass = true; }
        }
        if (!have_clamp || !(clamp > 0.0))
            return fail("dock port '" + c.id +
                        "' needs a positive dock.clamp_strength (#47b)");
        if (!have_mass || !(mass > 0.0))
            return fail("dock port '" + c.id + "' needs a positive mass attr (#47b)");
        bool undock = false;
        for (const auto& f : c.facets)
            if (f.action == "port.undock") { undock = true; break; }
        if (!undock)
            return fail("dock port '" + c.id + "' needs a port.undock facet (#47b)");
    }
    return true;
}

} // namespace ve::cook
