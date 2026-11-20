#include "ve/game/loadout_editor_panel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string_view>

#include "ve/core/sim/inertia.h"
#include "ve/game/attr_keys.h"
#include "ve/game/def_schema.h"

namespace ve::game {

namespace sim = core::sim;

// Deterministic human-readable names for assemble's error enum — the reason
// drawn on entry must come from a fixed table, not the sim's internals.
static const char* error_name(sim::loadout_error e) {
    switch (e) {
        case sim::loadout_error::duplicate_socket_id:     return "duplicate socket id";
        case sim::loadout_error::unknown_component_type:  return "unknown component type";
        case sim::loadout_error::unknown_socket:          return "unknown socket";
        case sim::loadout_error::socket_occupied:         return "socket already occupied";
        case sim::loadout_error::category_mismatch:       return "category mismatch";
        case sim::loadout_error::duplicate_instance_id:   return "duplicate instance id";
        case sim::loadout_error::duplicate_attr_key:      return "duplicate attr key";
        case sim::loadout_error::duplicate_tag:           return "duplicate tag";
        case sim::loadout_error::invalid_grid:            return "invalid grid";
        case sim::loadout_error::invalid_hull_mass:       return "invalid hull mass";
    }
    return "invalid loadout";  // unreachable unless the enum grows
}

// Instance ids for editor-mounted parts: type + socket, sanitized to
// [a-z0-9_] so they can never collide with anything else and round-trip
// through the cooklet without quoting surprises.
static std::string sanitize_id(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') out.push_back(c);
        else if (c >= 'A' && c <= 'Z') out.push_back(static_cast<char>(c - 'A' + 'a'));
        else out.push_back('_');
    }
    return out;
}

void loadout_editor_panel::set_hulls(std::vector<loadout_snapshot> hulls) {
    m_hulls.clear();
    for (auto& h : hulls) m_hulls.push_back(std::move(h));
    m_cur = 0;
    m_pick_socket.reset();
    m_route_edit.reset();
    m_reject.reset();
}

sim::socket_desc* loadout_editor_panel::find_socket(loadout_snapshot& s,
                                                    const std::string& id) {
    for (auto& sock : s.sockets)
        if (sock.id == id) return &sock;
    return nullptr;
}

gfx::rect loadout_editor_panel::rect_for(const std::string& key) const {
    auto it = m_rects.find(key);
    return it != m_rects.end() ? it->second : gfx::rect{};
}

const std::string* loadout_editor_panel::current_def_id() const {
    if (m_hulls.empty()) return nullptr;
    return &m_hulls[m_cur].def_id;
}

std::size_t loadout_editor_panel::mount_count() const {
    return m_hulls.empty() ? 0 : m_hulls[m_cur].mounts.size();
}

std::vector<sim::mass_point> loadout_editor_panel::mass_parts(
        const loadout_snapshot& s) const {
    std::vector<sim::mass_point> parts;
    sim::mass_point hull{};
    hull.mass = s.hull.hull_mass;
    hull.pos = s.hull.hull_com_local;
    hull.local_tensor = core::math::dmat3::diag(s.hull.hull_inertia_diag);
    parts.push_back(hull);
    for (const auto& m : s.mounts) {
        double mass = 0.0;
        for (const auto& a : m.attrs)
            if (a.key == std::string{kAttr_mass}) mass = a.value;
        const sim::socket_desc* sock = nullptr;
        for (const auto& sock_ref : s.sockets)
            if (sock_ref.id == m.socket_id) sock = &sock_ref;
        sim::mass_point p{};
        p.mass = mass;
        if (sock) p.pos = sock->position_local;
        parts.push_back(p);
    }
    return parts;
}

double loadout_editor_panel::total_mass() const {
    if (m_hulls.empty()) return 0.0;
    auto parts = mass_parts(m_hulls[m_cur]);
    auto ip = sim::inertia_from_parts(parts);
    return ip ? ip->total_mass : 0.0;
}

static double sum_attr(const std::vector<sim::mount_request>& mounts,
                       std::string_view key) {
    double total = 0.0;
    for (const auto& m : mounts)
        for (const auto& a : m.attrs)
            if (a.key == key) total += a.value;
    return total;
}

double loadout_editor_panel::power_supply() const {
    if (m_hulls.empty()) return 0.0;
    return sum_attr(m_hulls[m_cur].mounts, kAttr_power_supply);
}

double loadout_editor_panel::power_draw() const {
    if (m_hulls.empty()) return 0.0;
    return sum_attr(m_hulls[m_cur].mounts, kAttr_power_draw);
}

const std::string* loadout_editor_panel::last_reject() const {
    return m_reject ? &*m_reject : nullptr;
}

bool loadout_editor_panel::apply(std::vector<sim::mount_request> candidate,
                                 const std::string& attempted_type,
                                 const std::string& attempted_socket) {
    auto& s = m_hulls[m_cur];
    // The sim's OWN validity pass — the panel does not re-derive the rules
    // (5.9b: validity is a property of the loadout, enforced in one place).
    auto r = sim::Loadout::assemble(s.hull, s.sockets, s.types, candidate);
    if (!r) {
        std::string reason = error_name(r.error());
        if (!attempted_type.empty())
            reason += " (" + attempted_type + " @ " + attempted_socket + ")";
        m_reject = std::move(reason);
        return false;  // candidate discarded: garbage never enters state
    }
    m_hulls[m_cur].mounts = std::move(candidate);
    m_reject.reset();
    return true;
}

route_wire* loadout_editor_panel::find_wire(loadout_snapshot& s, std::string_view id) {
    for (auto& w : s.wires)
        if (w.id == id) return &w;
    return nullptr;
}

bool loadout_editor_panel::apply_route(std::size_t wire_idx,
                                       std::vector<route_cell> candidate) {
    auto& s = m_hulls[m_cur];
    if (wire_idx >= s.wires.size()) return false;
    route_wire view = s.wires[wire_idx];
    view.route = std::move(candidate);
    // THE shared law (route_law — the same fn the cook G12 gate wraps): an
    // edit refused here is exactly an edit that would fail cook, reason
    // byte-identical (grammar strings from parse_route_text, spatial from
    // route_law_error). Refused draft leaves state untouched.
    if (std::string err = route_law_error(s.sections, s.door_seams, view);
        !err.empty()) {
        m_reject = std::move(err);
        return false;
    }
    s.wires[wire_idx] = std::move(view);
    m_reject.reset();
    return true;
}

const std::string* loadout_editor_panel::editing_wire() const {
    return m_route_edit ? &*m_route_edit : nullptr;
}

const route_wire* loadout_editor_panel::wire_route(std::string_view id) const {
    if (m_hulls.empty()) return nullptr;
    for (const auto& w : m_hulls[m_cur].wires)
        if (w.id == id) return &w;
    return nullptr;
}

namespace {
// Canonical TOML route line body: [[x, y, z], [x, y, z]].
std::string toml_route(const std::vector<route_cell>& route) {
    std::string out = "[";
    for (std::size_t i = 0; i < route.size(); ++i) {
        if (i) out += ", ";
        out += "[" + std::to_string(route[i][0]) + ", " +
               std::to_string(route[i][1]) + ", " +
               std::to_string(route[i][2]) + "]";
    }
    return out + "]";
}
} // namespace

void loadout_editor_panel::render(gfx::ui_context& ui, std::int32_t x,
                                  std::int32_t y, std::int32_t w,
                                  std::int32_t h) {
    m_rects.clear();
    const std::int32_t col = w / 3;

    // ---- hull options (whole-ship swap, #24d) ------------------------------
    if (ui.begin_window("hulls", x, y, col, h)) {
        for (std::size_t i = 0; i < m_hulls.size(); ++i) {
            std::string label = (i == m_cur ? "* " : "  ") + m_hulls[i].label;
            const std::string key = "use:" + m_hulls[i].def_id;
            if (ui.button(label.c_str(), key.c_str())) {
                m_cur = i;
                m_pick_socket.reset();
                m_route_edit.reset();
                m_reject.reset();
            }
            m_rects[key] = ui.last_rect();
        }
    }
    ui.end_window();
    if (m_hulls.empty()) return;
    auto& s = m_hulls[m_cur];

    // ---- socket tree grouped by category, with 5.x gates ------------------
    if (ui.begin_window("sockets", x + col, y, col, h)) {
        for (int cat = 0; cat < 4; ++cat) {
            const auto category = static_cast<sim::socket_category>(cat);
            const std::string cat_label = std::string{kCategory_names[cat]} + ":";
            ui.label(cat_label.c_str());
            for (const auto& sock : s.sockets) {
                if (sock.category != category) continue;
                bool mounted = false;
                std::string info;
                for (const auto& m : s.mounts) {
                    if (m.socket_id == sock.id) {
                        mounted = true;
                        info = sock.id + " = " + m.instance_id + " (" + m.type_id + ")";
                        break;
                    }
                }
                if (mounted) {
                    if (ui.button("[x]", ("un:" + sock.id).c_str())) {
                        std::vector<sim::mount_request> cand;
                        for (const auto& m : s.mounts)
                            if (m.socket_id != sock.id) cand.push_back(m);
                        apply(std::move(cand), "", "");
                    }
                    m_rects["un:" + sock.id] = ui.last_rect();
                    ui.label(info.c_str());
                } else {
                    const std::string key = "pick:" + sock.id;
                    if (ui.button(("[pick] " + sock.id).c_str(), key.c_str())) {
                        m_pick_socket = sock.id;
                        m_reject.reset();
                    }
                    m_rects[key] = ui.last_rect();
                }
            }
        }
        // ---- wire routes (9.5): one button per wire enters route edit mode
        // (#51); drafts re-validate through the SHARED route_law (G12).
        if (!s.wires.empty()) {
            ui.label("wires:");
            for (auto& w : s.wires) {
                const std::string key = "rt:" + w.id;
                const std::string lbl =
                    w.id + (m_route_edit && *m_route_edit == w.id
                                ? " [editing, " + std::to_string(w.route.size()) +
                                      " cells]"
                                : " [" + std::to_string(w.route.size()) + " cells]");
                if (ui.button(lbl.c_str(), key.c_str())) {
                    m_pick_socket.reset();  // route editor owns column 3
                    m_reject.reset();
                    m_route_edit = w.id;
                    // Seed the draft buffer from the CURRENT committed route
                    // (canonical flat text; brackets optional).
                    const std::string seed = route_text_of(w.route);
                    const std::size_t n =
                        seed.size() < sizeof m_route_buf - 1 ? seed.size()
                                                             : sizeof m_route_buf - 1;
                    std::memcpy(m_route_buf, seed.data(), n);
                    m_route_buf[n] = '\0';
                }
                m_rects[key] = ui.last_rect();
            }
        }

        // stats-spine readout (game-side attr keys; core stays agnostic)
        char buf[128];
        std::snprintf(buf, sizeof buf, "mass %.0f kg   supply %.0f W   draw %.0f W",
                      total_mass(), power_supply(), power_draw());
        ui.label(buf);
    }
    ui.end_window();

    // ---- part picker (clicking a mismatched part is refused ON ENTRY) -----
    if (ui.begin_window("pick part", x + 2 * col, y, col, h)) {
        route_wire* editing =
            m_route_edit ? find_wire(s, *m_route_edit) : nullptr;
        if (editing) {
            // ---- route edit mode (9.5, #51) ------------------------------
            const std::string head = "wire: " + editing->id;
            ui.label(head.c_str());
            if (m_reject) {
                const std::string why = "REFUSED: " + *m_reject;
                ui.label(why.c_str());
            }
            ui.label("route cells (x, y, z):");
            // text_field edits the persistent buffer across frames; returns
            // true on the ENTER commit frame. A refused draft KEEPS the text
            // and refocuses (the fix-and-re-Enter pattern #48 pinned for
            // forms): garbage never enters state, the draft survives.
            if (ui.text_field("route", m_route_buf, sizeof m_route_buf)) {
                std::vector<route_cell> draft;
                std::string msg;
                if (!parse_route_text(m_route_buf, draft, msg)) {
                    m_reject = std::move(msg);  // grammar refuse (route_law)
                    ui.refocus_text("route");
                } else if (apply_route(
                               static_cast<std::size_t>(editing - s.wires.data()),
                               std::move(draft))) {
                    const std::string seed = route_text_of(editing->route);
                    const std::size_t n =
                        seed.size() < sizeof m_route_buf - 1 ? seed.size()
                                                             : sizeof m_route_buf - 1;
                    std::memcpy(m_route_buf, seed.data(), n);
                    m_route_buf[n] = '\0';
                } else {
                    ui.refocus_text("route");  // spatial law refuse, draft kept
                }
            }
            m_rects["route"] = ui.last_rect();  // the text field (tests click it)
            if (ui.button("[clear]", "rclr")) m_route_buf[0] = '\0';
            m_rects["rclr"] = ui.last_rect();
            const std::string done_key = "rd:" + editing->id;
            if (ui.button("[done]", done_key.c_str())) m_route_edit.reset();
            m_rects[done_key] = ui.last_rect();
        } else if (!m_pick_socket) {
            ui.label("(pick a socket)");
        } else {
            const std::string pick_head = "socket: " + *m_pick_socket;
            ui.label(pick_head.c_str());
            if (m_reject) {
                const std::string why = "REFUSED: " + *m_reject;
                ui.label(why.c_str());
            }
            for (const auto& part : s.catalog) {
                const std::string key = "cat:" + part.type_id;
                if (ui.button(part.type_id.c_str(), key.c_str())) {
                    auto cand = s.mounts;
                    sim::mount_request req;
                    req.instance_id = sanitize_id(part.type_id) + "_" + sanitize_id(*m_pick_socket);
                    req.type_id = part.type_id;
                    req.socket_id = *m_pick_socket;
                    for (const auto& a : part.attrs)
                        req.attrs.push_back({a.key, a.value});
                    req.tags = part.tags;
                    cand.push_back(std::move(req));
                    apply(std::move(cand), part.type_id, *m_pick_socket);
                }
                m_rects[key] = ui.last_rect();
            }
        }
    }
    ui.end_window();
}

// ---- write-back: TOML only (#16) ------------------------------------------
// Component element regions in the def source: a "[[component]]" header plus
// its "[component.*]" / "[[[component.*]]]" sub-table continuation, up to the
// next unrelated top-level header or EOF. Removing an unmounted component
// removes its whole region; new mounts append after the last region. All
// other lines (root keys, comments, and the [[wire]] route lines of
// UNCHANGED wires) pass through byte-identical; only the route line of a
// wire edited in route mode (9.5, #51) is rewritten, to canonical form.
std::string loadout_editor_panel::emit_source() const {
    if (m_hulls.empty()) return {};
    const auto& s = m_hulls[m_cur];
    std::vector<std::string> lines;
    {
        std::istringstream in(s.source);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    auto is_header = [](const std::string& l) {
        return !l.empty() && l[0] == '[';
    };
    auto is_comp_cont = [](const std::string& l) {
        return l.rfind("[component.", 0) == 0 || l.rfind("[[component.", 0) == 0;
    };
    // #51 route mode (9.5): rewrite the route line of each [[wire]] region
    // whose WORKING route differs from the authored one. Unchanged wires
    // keep their authored bytes exactly (author style preserved); changed
    // wires get the canonical "route = [[x, y, z], ...]" form. Wires absent
    // from the working view and every other line pass through unchanged.
    {
        auto trim_view = [](const std::string& l) {
            std::size_t a = 0, b = l.size();
            while (a < b && std::isspace(static_cast<unsigned char>(l[a]))) ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(l[b - 1]))) --b;
            return std::string_view(l).substr(a, b - a);
        };
        for (std::size_t h = 0; h < lines.size(); ++h) {
            if (lines[h] != "[[wire]]") continue;
            std::size_t end = h + 1;
            while (end < lines.size() && !is_header(lines[end])) ++end;
            std::string id;
            std::size_t rl = std::string::npos;
            for (std::size_t k = h + 1; k < end; ++k) {
                const std::string_view u = trim_view(lines[k]);
                if (id.empty() && u.rfind("id =", 0) == 0) {
                    const auto q1 = u.find('"'), q2 = u.rfind('"');
                    if (q1 != std::string_view::npos && q2 > q1)
                        id = std::string(u.substr(q1 + 1, q2 - q1 - 1));
                } else if (rl == std::string::npos &&
                           u.rfind("route", 0) == 0 &&
                           u.find('=') != std::string_view::npos) {
                    rl = k;
                }
            }
            const route_wire* w = nullptr;
            for (const auto& cw : s.wires)
                if (cw.id == id) { w = &cw; break; }
            if (!w) continue;
            if (rl != std::string::npos) {
                const std::size_t eq = lines[rl].find('=');
                std::vector<route_cell> authored;
                std::string msg;
                const bool parsed =
                    parse_route_text(trim_view(lines[rl].substr(eq + 1)), authored, msg);
                const bool same =
                    parsed && authored.size() == w->route.size() &&
                    std::equal(authored.begin(), authored.end(), w->route.begin());
                if (!same) lines[rl] = "route = " + toml_route(w->route);
            } else if (!w->route.empty()) {
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(h) + 1,
                             "route = " + toml_route(w->route));
                ++h;  // inserted line sits inside this region; keep scanning
            }
        }
    }
    // Locate component element regions and their ids.
    struct region { std::size_t begin, end; std::string id; };
    std::vector<region> regions;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] != "[[component]]") continue;
        region r{i, i + 1, ""};
        std::size_t j = i + 1;
        for (; j < lines.size(); ++j) {
            if (is_header(lines[j])) {
                // Next ELEMENT header ends the region too ("[[component]]"
                // exact — sub-tables like "[component.attrs]" continue it).
                if (lines[j] == "[[component]]" || !is_comp_cont(lines[j])) break;
            }
            const std::string idp = "id = \"";
            if (r.id.empty() && lines[j].rfind(idp, 0) == 0) {
                auto close = lines[j].find('"', idp.size());
                if (close != std::string::npos)
                    r.id = lines[j].substr(idp.size(), close - idp.size());
            }
        }
        r.end = j;
        regions.push_back(std::move(r));
        i = j - 1;
    }
    const auto keep = [&](const std::string& id) {
        return std::any_of(s.mounts.begin(), s.mounts.end(),
                           [&](const auto& m) { return m.instance_id == id; });
    };
    std::ostringstream out;
    std::size_t written = 0;
    std::size_t last_region_end = 0;
    for (const auto& r : regions) {
        for (std::size_t i = written; i < r.begin; ++i) out << lines[i] << "\n";
        if (keep(r.id))
            for (std::size_t i = r.begin; i < r.end; ++i) out << lines[i] << "\n";
        written = r.end;
        last_region_end = r.end;
    }
    // Append new mounts (ids not present in the original def).
    std::ostringstream added;
    for (const auto& m : s.mounts) {
        const bool existed = std::any_of(regions.begin(), regions.end(),
                                         [&](const auto& r) { return r.id == m.instance_id; });
        if (existed) continue;
        const sim::component_type* ct = nullptr;
        for (const auto& c : s.types)
            if (c.type_id == m.type_id) ct = &c;
        added << "\n[[component]]\nid = \"" << m.instance_id << "\"\n"
              << "type = \"" << m.type_id << "\"\ncategory = \""
              << std::string{kCategory_names[static_cast<int>(
                     ct ? ct->category : sim::socket_category::utility_)]}
              << "\"\nsocket = \"" << m.socket_id << "\"\n";
        if (!m.attrs.empty()) {
            added << "[component.attrs]\n";
            for (const auto& a : m.attrs) {
                std::string lit;  // authored literal text from the catalog
                const loadout_catalog_part* cp = nullptr;
                for (const auto& c : s.catalog)
                    if (c.type_id == m.type_id) cp = &c;
                if (cp)
                    for (const auto& ca : cp->attrs)
                        if (ca.key == a.key) lit = ca.toml_literal;
                added << a.key << " = " << (lit.empty() ? std::to_string(a.value) : lit) << "\n";
            }
        }
    }
    // Insert the added blocks right after the last component region so the
    // def keeps its authored section order; tail lines follow unchanged. A
    // def with no components at all gets them appended at EOF.
    if (!regions.empty()) {
        for (std::size_t i = written; i < lines.size(); ++i) {
            if (i == last_region_end) out << added.str();
            out << lines[i] << "\n";
        }
        if (last_region_end >= lines.size()) out << added.str();
    } else {
        for (const auto& l : lines) out << l << "\n";
        out << added.str();
    }
    return out.str();
}

} // namespace ve::game
