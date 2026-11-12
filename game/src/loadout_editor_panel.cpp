#include "ve/game/loadout_editor_panel.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

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
        // stats-spine readout (game-side attr keys; core stays agnostic)
        char buf[128];
        std::snprintf(buf, sizeof buf, "mass %.0f kg   supply %.0f W   draw %.0f W",
                      total_mass(), power_supply(), power_draw());
        ui.label(buf);
    }
    ui.end_window();

    // ---- part picker (clicking a mismatched part is refused ON ENTRY) -----
    if (ui.begin_window("pick part", x + 2 * col, y, col, h)) {
        if (!m_pick_socket) {
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
// other lines (root keys, [[wire]], comments) pass through byte-identical.
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
