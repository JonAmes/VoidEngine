// 8.3 — selection_service + inspector_panel.
#include "ve/game/inspector_panel.h"

#include <algorithm>
#include <cstdio>

#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/stats.h"

namespace ve::game {

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
