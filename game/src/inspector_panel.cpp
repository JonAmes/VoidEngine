// 8.3 — selection_service + inspector_panel.
#include "ve/game/inspector_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/stats.h"

namespace ve::game {

// --- 8.20 mode B: 2D system schematic (#43) --------------------------------

void inspector_panel::begin_schematic(schematic_source* src) {
    m_ssrc = src;
    m_schem_sel.clear();   // selection KEY is schematic scope; the shared
    m_prev_down = false;   // 8.3 entity selection is left alone here
}

void inspector_panel::end_schematic() {
    m_ssrc = nullptr;
    m_schem_sel.clear();
    m_prev_down = false;
}

std::vector<schematic_node_input> inspector_panel::schematic_nodes() const {
    if (!m_ssrc) return {};
    return m_ssrc->schematic_nodes(0);  // map of the def: tick 0, not the sim
}

inspector_panel::schem_view inspector_panel::schematic_view() const {
    schem_view v;
    if (!m_ssrc) return v;
    const auto nodes = m_ssrc->schematic_nodes(0);
    // Ref rule (#43): selected node when set, else first station, else the
    // system origin. Everything else is placed by system-frame bearing.
    const schematic_node_input* ref = nullptr;
    for (const auto& n : nodes)
        if (n.key == m_schem_sel) { ref = &n; break; }
    if (!ref)
        for (const auto& n : nodes)
            if (n.station) { ref = &n; break; }
    if (ref) v.center_key = ref->key;
    const core::math::dvec3 org =
        ref ? ref->pos : core::math::dvec3{0.0, 0.0, 0.0};
    // Top-down projection: screen x = east (system +x), y = -north (+z up
    // on the map means -y in y-down px).
    constexpr double kPi = 3.14159265358979323846;
    struct px2 { double x, y; };
    double range = 0.0;
    std::vector<px2> off(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto d = nodes[i].pos - org;
        off[i] = {d.x, -d.z};
        range = std::max(range, std::hypot(off[i].x, off[i].y));
    }
    v.scale = range > 0.0 ? double(kSchemRadiusPx) / range : 1.0;
    // Co-located nodes (range 0 or exact zero offset): deterministic ring in
    // list order — the map never stacks markers invisibly.
    std::size_t ring = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        float x = float(off[i].x * v.scale), y = float(off[i].y * v.scale);
        if (std::hypot(x, y) < 1e-6 && (!ref || nodes[i].key != ref->key)) {
            const double a = -kPi * 0.5 + double(ring++) * (2.0 * kPi / 3.0);
            x = float(std::cos(a) * kSchemRingPx);
            y = float(std::sin(a) * kSchemRingPx);
        }
        v.nodes.push_back({nodes[i].key, x, y});
    }
    return v;
}

std::vector<inspector_panel::schem_edge> inspector_panel::schematic_edges() const {
    std::vector<schem_edge> out;
    if (!m_ssrc) return out;
    const auto* d = m_ssrc->sys_def();
    if (!d) return out;
    // nav-graph rows first, then trade lanes — both keyed by node id.
    for (const auto& e : d->nav_edges) {
        out.push_back({"node:" + e.from, "node:" + e.to,
                       e.kind == NavKind::lane   ? "lane" :
                       e.kind == NavKind::patrol ? "patrol" : "local",
                       "", "", 0.0});
    }
    for (const auto& l : d->lanes)
        out.push_back({"node:" + l.from, "node:" + l.to, "lane",
                       l.commodity, l.id, l.flow_rate});
    return out;
}

std::vector<std::string> inspector_panel::schematic_side_fields() const {
    std::vector<std::string> out;
    if (!m_ssrc) return out;
    const auto* d = m_ssrc->sys_def();
    if (!d) return out;
    // #39/#25b glue the map cannot project: readout rows, label only.
    for (const auto& l : d->lanes)
        out.push_back("lane " + l.id + " " + l.commodity +
                      " flow " + std::format("{:g}", l.flow_rate));
    for (const auto& s : d->spawns)
        out.push_back("spawn " + s.id + " " + s.at + " " + s.faction +
                      " " + s.ship_class + " rate " + std::format("{:g}", s.rate));
    for (const auto& f : d->factions)
        for (const auto& t : f.tags)
            if (t.rfind("stance.", 0) == 0) {
                auto rest = t.substr(7);  // "<kind>.<faction>"
                const auto dot = rest.find('.');
                if (dot != std::string::npos)
                    out.push_back("stance " + f.name + " " + rest.substr(0, dot) +
                                  " " + rest.substr(dot + 1));
            }
    return out;
}

void inspector_panel::click_schematic(const gfx::ui_io& io) {
    if (!m_ssrc) { m_prev_down = io.mouse_down; return; }
    const bool rising = io.mouse_down && !m_prev_down;
    m_prev_down = io.mouse_down;
    if (!rising) return;
    // Map-space px (window center is the origin — the view's own frame;
    // render_schematic adds the window offset when drawing).
    const double mx = io.mouse_x, my = io.mouse_y;
    const auto v = schematic_view();
    const schem_view_node* best = nullptr;
    double best_d = double(kSchemMarkerPx) + 1.0;
    for (const auto& n : v.nodes) {
        const double d = std::hypot(double(n.x) - mx, double(n.y) - my);
        if (d < best_d) { best_d = d; best = &n; }
    }
    if (!best) {
        m_schem_sel.clear();
        m_sel.clear_selection();
        return;
    }
    m_schem_sel = best->key;
    // The shared 8.3 service follows when the node has a live entity; nav
    // nodes (waypoints) have none, so the inspector shows no entity rather
    // than a stale one.
    for (const auto& n : schematic_nodes())
        if (n.key == best->key) {
            if (n.ent) m_sel.select(*n.ent);
            else m_sel.clear_selection();
            break;
        }
}

bool inspector_panel::schematic_add_edge(const std::string& from_id,
                                         const std::string& to_id,
                                         std::string* err) {
    if (!m_ssrc) { if (err) *err = "no schematic source"; return false; }
    // The 8.8 wrldvalidate gate is the wiring validator: a node-less endpoint
    // fails the recook and the write-back reverts to zero bytes.
    return m_ssrc->add_glue_row(
        "edge",
        {{"kind", "\"lane\""}, {"from", "\"" + from_id + "\""},
         {"to", "\"" + to_id + "\""}}, err);
}

bool inspector_panel::schematic_set_flow(const std::string& lane_id, double flow,
                                         std::string* err) {
    if (!m_ssrc) { if (err) *err = "no schematic source"; return false; }
    return m_ssrc->set_glue_field("lane", "id", lane_id, "flow",
                                  std::format("{}", flow), err);
}

bool inspector_panel::schematic_drag_node(std::string_view key, float to_x_px,
                                          float to_y_px, std::string* err) {
    if (!m_ssrc) { if (err) *err = "no schematic source"; return false; }
    const auto v = schematic_view();
    const auto nit = std::ranges::find(v.nodes, key, &schem_view_node::key);
    if (nit == v.nodes.end()) { if (err) *err = "no such node on the map"; return false; }
    // Inverse of the top-down projection (px = ((pos-org).x, -(pos-org).z)
    // * scale): world target about the current view center.
    const auto inputs = m_ssrc->schematic_nodes(0);
    const auto cit = std::ranges::find(inputs, v.center_key,
                                       &schematic_node_input::key);
    const core::math::dvec3 org =
        cit != inputs.end() ? cit->pos : core::math::dvec3{0.0, 0.0, 0.0};
    const double inv = v.scale > 0.0 ? 1.0 / v.scale : 0.0;
    const core::math::dvec3 target{org.x + double(to_x_px) * inv, org.y,
                                   org.z - double(to_y_px) * inv};
    return m_ssrc->move_def_waypoint(std::string(key), target, err);
}

void inspector_panel::render_schematic(gfx::ui_context& ui, std::int32_t x,
                                       std::int32_t y, std::int32_t w,
                                       std::int32_t h) {
    if (!m_ssrc) return;
    if (!ui.begin_window("system map", x, y, w, h)) { ui.end_window(); return; }
    const std::int32_t cx = x + w / 2, cy = y + h / 2;
    const auto vn = schematic_view().nodes;
    // Edges first (under the markers): cargo lanes bright, nav graph dim.
    for (const auto& e : schematic_edges()) {
        const schem_view_node *a = nullptr, *b = nullptr;
        for (const auto& n : vn) {
            if (n.key == e.from) a = &n;
            if (n.key == e.to) b = &n;
        }
        if (!a || !b) continue;
        if (!e.lane_id.empty())
            ui.line(cx + std::int32_t(a->x), cy + std::int32_t(a->y),
                    cx + std::int32_t(b->x), cy + std::int32_t(b->y), 235, 200, 90, 2);
        else
            ui.line(cx + std::int32_t(a->x), cy + std::int32_t(a->y),
                    cx + std::int32_t(b->x), cy + std::int32_t(b->y), 120, 160, 210, 1);
    }
    // Markers: short thick strokes (dots); selected node white.
    for (const auto& n : schematic_view().nodes) {
        const bool sel = n.key == m_schem_sel;
        ui.line(cx + std::int32_t(n.x) - 3, cy + std::int32_t(n.y),
                cx + std::int32_t(n.x) + 3, cy + std::int32_t(n.y),
                sel ? 255 : 200, sel ? 255 : 200, sel ? 255 : 220, 6);
    }
    // Side fields as a text column (labels ride the layout cursor).
    for (const auto& row : schematic_side_fields()) ui.label(row.c_str());
    ui.end_window();
}

// --- selection_service -----------------------------------------------------

namespace {
// (slot, generation) order — an ordered container and an explicit total
// order keep expansion lookup binary-searchable (AGENTS rule 5).
bool id_less(const core::entity_id& a, const core::entity_id& b) {
    if (a.slot != b.slot) return a.slot < b.slot;
    return a.generation < b.generation;
}
}  // namespace

bool selection_service::toggle_expand(core::entity_id e) {
    auto it = std::lower_bound(m_expanded.begin(), m_expanded.end(), e, id_less);
    if (it != m_expanded.end() && *it == e) {
        m_expanded.erase(it);
        return false;
    }
    m_expanded.insert(it, e);
    return true;
}

bool selection_service::is_expanded(core::entity_id e) const {
    return std::binary_search(m_expanded.begin(), m_expanded.end(), e, id_less);
}

// --- inspector_panel ---------------------------------------------------------

namespace {
const char* op_spell(core::mod_op op) {
    switch (op) {
        case core::mod_op::flat_add: return "add";
        case core::mod_op::percent: return "pct";
        case core::mod_op::multiplier: return "x";
    }
    return "?";
}

std::string entity_label(const inspector_data& data, core::entity_id e) {
    if (data.label_for) {
        if (std::string s = data.label_for(e); !s.empty()) return s;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "e%d", e.slot);
    return buf;
}
}  // namespace

void inspector_panel::render(gfx::ui_context& ui, const inspector_data& data,
                             std::int32_t x, std::int32_t y, std::int32_t w,
                             std::int32_t h) {
    m_rects.clear();
    if (!data.world) return;
    const auto tree = data.world->entities();  // slot order (deterministic)

    // Clamping happens BEFORE the buttons act is fine because a click only
    // ever moves scroll by one; clamp after mutation keeps the view sane.
    const int max_scroll =
        tree.empty() ? 0 : static_cast<int>(tree.size()) - 1;
    m_tree_scroll = std::clamp(m_tree_scroll, 0, max_scroll);

    // --- left column: entity tree ---------------------------------------
    const std::int32_t half = w / 2;
    if (ui.begin_window("entities", x, y, half, h)) {
        if (ui.button("[^]", "scroll_up")) --m_tree_scroll;
        m_rects["scroll_up"] = ui.last_rect();
        if (ui.button("[v]", "scroll_dn")) ++m_tree_scroll;
        m_rects["scroll_dn"] = ui.last_rect();
        m_tree_scroll = std::clamp(m_tree_scroll, 0, max_scroll);

        for (std::size_t i = 0; i < tree.size(); ++i) {
            // Scroll = skip whole entity ROWS (a scrolled-away row has no
            // hit rect this frame — clicking where it "was" does nothing).
            if (static_cast<int>(i) < m_tree_scroll) continue;
            const auto& ent = tree[i];
            const std::string slot = std::to_string(ent.id.slot);

            const bool expanded = m_sel.is_expanded(ent.id);
            const char tw = expanded ? '-' : '+';
            const std::string tw_label(1, tw);
            if (ui.button(tw_label.c_str(), ("tw:" + slot).c_str()))
                m_sel.toggle_expand(ent.id);
            m_rects["tw:" + slot] = ui.last_rect();

            const std::string label = entity_label(data, ent.id);
            if (ui.button(label.c_str(), ("tree:" + slot).c_str()))
                m_sel.select(ent.id);
            m_rects["tree:" + slot] = ui.last_rect();

            if (expanded) {
                for (const std::uint32_t t : ent.types) {
                    // component rows: the registry's opaque name, indented.
                    std::string row = "   ";
                    row += data.world->component_type_names()[t];
                    ui.label(row.c_str());
                }
            }
        }
    }
    ui.end_window();

    // --- right column: selection detail ---------------------------------
    const std::int32_t rx = x + half;
    const std::int32_t rw = w - half;
    if (ui.begin_window("selection", rx, y, rw, h)) {
        const auto sel = m_sel.selected();
        if (!sel || !data.world->alive(*sel)) {
            ui.label("(no live selection)");
        } else {
            const core::entity_id e = *sel;
            ui.label(entity_label(data, e).c_str());

            // components (opaque names, ascending = registry order)
            for (std::uint32_t t = 0; t < data.world->component_type_names().size(); ++t)
                if (data.world->has(e, t))
                    ui.label(("comp: " + data.world->component_type_names()[t]).c_str());

            // stats totals
            if (data.stats_for) {
                if (const auto* st = data.stats_for(e)) {
                    ui.label("stats:");
                    for (const auto& en : st->entries()) {
                        char buf[96];
                        std::snprintf(buf, sizeof buf, "  %s: %.3g -> %.3g",
                                      en.key.c_str(),
                                      static_cast<double>(en.value.base),
                                      static_cast<double>(en.value.derived));
                        ui.label(buf);
                    }
                }
            }

            // active modifier chains (source order, then modifier order —
            // the same total order 6.4's pipeline aggregates in)
            if (data.mods_for) {
                if (const auto* ms = data.mods_for(e)) {
                    ui.label("modifiers:");
                    char buf[128];
                    for (const auto& src : ms->sources()) {
                        std::snprintf(buf, sizeof buf, "src %llu:",
                                      static_cast<unsigned long long>(src.id));
                        ui.label(buf);
                        for (const auto& m : src.mods) {
                            std::snprintf(buf, sizeof buf, "  %s %s %.3g",
                                          m.target_attr.c_str(), op_spell(m.op),
                                          static_cast<double>(m.value));
                            ui.label(buf);
                        }
                    }
                }
            }
        }
    }
    ui.end_window();
}

std::optional<gfx::rect> inspector_panel::rect_for(const std::string& key) const {
    auto it = m_rects.find(key);
    return it == m_rects.end() ? std::nullopt : std::optional<gfx::rect>{it->second};
}

}  // namespace ve::game
