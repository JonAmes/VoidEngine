#pragma once
// Phase 8 task 8.3 — ECS/stats inspector panel: the entity tree (ecs_world
// entities + their opaque component type names, 6.1 rule) over the live
// sim, plus stats totals and active modifier chains for the selection.
// Headless like every debug panel (#16): ui_io in, draw list out; the
// panel NEVER mutates the sim.
//
// The selection is NOT panel state: it lives in selection_service, a
// host-owned game-side service (#48 kit spine) any editor panel can
// read/drive — 8.4's browser and 8.5's loadout editor select the same
// entities the inspector shows.
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ve/core/ecs.h"
#include "ve/gfx/ui.h"

namespace ve::core {
class StatsSet;
class ModifierStack;
}  // namespace ve::core

namespace ve::game {

// Shared selection service. Deterministic: the expansion set is kept
// sorted by (slot, generation); selection is a raw id whose liveness the
// consumer checks (the service never touches the world).
class selection_service {
public:
    void select(core::entity_id e) { m_selected = e; }
    void clear_selection() { m_selected.reset(); }
    [[nodiscard]] const std::optional<core::entity_id>& selected() const {
        return m_selected;
    }

    bool toggle_expand(core::entity_id e);  // returns the NEW expanded state
    [[nodiscard]] bool is_expanded(core::entity_id e) const;

private:
    std::optional<core::entity_id> m_selected;
    std::vector<core::entity_id> m_expanded;  // sorted; ordered container (rule 5)
};

// What the inspector looks at. The world is const — the panel only reads.
// stats_for/mods_for map an entity to its stats/modifier surfaces: the
// game side owns that wiring, the panel never guesses it.
struct inspector_data {
    const core::ecs_world* world = nullptr;
    std::function<const core::StatsSet*(core::entity_id)> stats_for;
    std::function<const core::ModifierStack*(core::entity_id)> mods_for;
    std::function<std::string(core::entity_id)> label_for;  // empty -> "e<slot>"
};

class inspector_panel {
public:
    explicit inspector_panel(selection_service& sel) : m_sel(sel) {}

    // Two side-by-side windows: tree (left), selection detail (right).
    // ui has no wheel event (#15 keeps input neutral), so scrolling is
    // [^]/[v] row buttons whose state lives here (panel = view state,
    // service = shared state). Hit rects are recorded every frame for
    // deterministic headless clicks; keys:
    //   "tree:<slot>"  entity row (selects)
    //   "tw:<slot>"    twisty (expand/collapse)
    //   "scroll_up" / "scroll_dn"
    void render(gfx::ui_context& ui, const inspector_data& data,
                std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h);

    [[nodiscard]] std::optional<gfx::rect> rect_for(const std::string& key) const;
    [[nodiscard]] int tree_scroll() const { return m_tree_scroll; }

private:
    selection_service& m_sel;
    int m_tree_scroll = 0;
    std::map<std::string, gfx::rect> m_rects;
};

}  // namespace ve::game
