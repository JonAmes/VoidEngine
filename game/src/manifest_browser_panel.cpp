// 8.4 — manifest_browser_panel: manifest.bin rows, dep-graph detail,
// def handoff to the forms panel. No file IO (host injects snapshots).
#include "ve/game/manifest_browser_panel.h"

#include <algorithm>
#include <cstdio>

namespace ve::game {

void manifest_browser_panel::set_rows(std::vector<manifest_row> rows) {
    m_rows = std::move(rows);
    // Keep the current view usable: a selection that left the snapshot
    // (source deleted, entry dropped) takes its handoff with it.
    if (m_sel && find_row(*m_sel) < 0) {
        m_sel.reset();
        m_handoff.reset();
    }
}

int manifest_browser_panel::find_row(const std::string& id) const {
    for (std::size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].id == id) return static_cast<int>(i);
    return -1;
}

void manifest_browser_panel::select_id(const std::string& id) {
    m_sel = id;
    m_handoff.reset();
    if (def_source) {
        if (auto src = def_source(id))
            m_handoff = def_handoff{id, *src};
    }
    // Graph jumps must land visibly (only the upper edge is clampable in
    // render — the lower clamp knows `visible`, which render owns).
    const int idx = find_row(id);
    if (idx >= 0 && idx < m_scroll) m_scroll = idx;
}

void manifest_browser_panel::render(gfx::ui_context& ui, std::int32_t x,
                                    std::int32_t y, std::int32_t w,
                                    std::int32_t h) {
    m_rects.clear();

    // Row budget: client height minus the two scroll-button rows, at the
    // ui layout pitch (public constants — the panel and the layout
    // engine share one source of truth, so this stays exact).
    const int pitch = gfx::ui_context::kRowH + gfx::ui_context::kSpacing;
    const int visible = std::max(
        0, (h - gfx::ui_context::kTitleH - gfx::ui_context::kPad - 2 * pitch) / pitch);
    const int max_scroll =
        m_rows.size() > static_cast<std::size_t>(visible)
            ? static_cast<int>(m_rows.size()) - visible : 0;
    m_scroll = std::clamp(m_scroll, 0, max_scroll);

    // --- left column: manifest entries ---------------------------------
    const std::int32_t half = w / 2;
    if (ui.begin_window("manifest", x, y, half, h)) {
        if (ui.button("[^]", "scroll_up")) --m_scroll;
        m_rects["scroll_up"] = ui.last_rect();
        if (ui.button("[v]", "scroll_dn")) ++m_scroll;
        m_rects["scroll_dn"] = ui.last_rect();
        m_scroll = std::clamp(m_scroll, 0, max_scroll);

        for (int i = m_scroll;
             i < static_cast<int>(m_rows.size()) && i < m_scroll + visible; ++i) {
            const auto& row = m_rows[i];
            std::string label = row.id;
            if (row.stale) label += "  [stale]";   // badge rides the row label
            if (m_sel && *m_sel == row.id) label = "* " + label;
            if (ui.button(label.c_str(), ("mrow:" + row.id).c_str()))
                select_id(row.id);
            m_rects["mrow:" + row.id] = ui.last_rect();
        }
    }
    ui.end_window();

    // --- right column: selected entry detail ----------------------------
    if (ui.begin_window("entry", x + half, y, w - half, h)) {
        const int idx = m_sel ? find_row(*m_sel) : -1;
        if (idx < 0) {
            ui.label("(no entry selected)");
        } else {
            const auto& row = m_rows[idx];
            ui.label(row.id.c_str());

            char buf[96];
            if (row.cooked_size)
                std::snprintf(buf, sizeof buf, "cooked: %llu bytes",
                              static_cast<unsigned long long>(row.cooked_size));
            else
                std::snprintf(buf, sizeof buf, "NOT COOKED");
            ui.label(buf);
            if (row.stale) ui.label("STALE - re-cook pending");

            std::snprintf(buf, sizeof buf, "deps (%zu):", row.deps.size());
            ui.label(buf);
            for (const auto& d : row.deps) {
                // Graph navigation; a dangling dep (no row — should not
                // happen in a cooked tree) simply does not navigate.
                if (ui.button(d.c_str(), ("dep:" + d).c_str()))
                    if (find_row(d) >= 0) select_id(d);
                m_rects["dep:" + d] = ui.last_rect();
            }
        }
    }
    ui.end_window();
}

std::optional<gfx::rect> manifest_browser_panel::rect_for(
    const std::string& key) const {
    auto it = m_rects.find(key);
    return it == m_rects.end() ? std::nullopt : std::optional<gfx::rect>{it->second};
}

}  // namespace ve::game
