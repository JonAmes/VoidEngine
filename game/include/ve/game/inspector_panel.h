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
#include "ve/core/math/vec.h"
#include "ve/game/world_load.h"
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
// ---- 8.20 authoring suite mode B (#43): the schematic is a different
// projection of the SAME world def, not a second data model. The panel
// consumes neutral node inputs + the cooked WorldDef through this source
// interface (the cooklet's author_world implements it, so every edit rides
// the real incremental cooker's schema + wrldvalidate gates — the 8.19
// doctrine); the game layer never links the cooklet.
struct schematic_node_input {
    std::string key;                                  // def path, or "node:<id>"
    std::string label;
    std::string detail;                               // hull / anchor / kind
    std::optional<core::entity_id> ent;               // nullopt: no sim entity
    bool station = false;                             // station nodes center the map
    core::math::dvec3 pos{0.0, 0.0, 0.0};             // system frame at the shown tick
};

class schematic_source {
public:
    virtual ~schematic_source() = default;
    // Nodes: stations (member order), then waypoints, then markets.
    virtual std::vector<schematic_node_input> schematic_nodes(std::int32_t tick) const = 0;
    // Cooked system def (edges/lanes/spawns/stances side fields).
    virtual const ve::game::WorldDef* sys_def() const = 0;
    // Same gate as 8.19 scalar drags: accepted = recooked + hot-swapped;
    // refused = ZERO bytes changed, *err = the gate's first message.
    virtual bool add_glue_row(const std::string& table,
                              const std::vector<std::pair<std::string, std::string>>& fields,
                              std::string* err) = 0;
    virtual bool set_glue_field(const std::string& table, const std::string& key_field,
                                const std::string& key_value, const std::string& field,
                                const std::string& value_literal, std::string* err) = 0;
};

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

    // ---- 8.20 authoring suite mode B: 2D system schematic (#43) ----
    // The schematic shares this panel (and its selection service) with the
    // tree view: begin_schematic switches the render target to the map.
    void begin_schematic(schematic_source* src);
    void end_schematic();
    [[nodiscard]] bool schematic_active() const { return m_ssrc != nullptr; }

    struct schem_view_node { std::string key; float x, y; };  // px about center
    struct schem_view {
        std::string center_key;   // "" = raw offsets about the system origin
        double scale = 0.0;       // world m per px (max-range node at kSchemRadiusPx)
        std::vector<schem_view_node> nodes;
    };
    struct schem_edge {
        std::string from, to;     // node keys
        std::string kind;         // nav-row kind, or "lane" for [[lane]] rows
        std::string commodity;    // [[lane]] rows only (#39 side field)
        std::string lane_id;      // [[lane]] id ('' for nav rows) — set_flow target
        double flow = 0.0;        // bare flow rate (#39 side field)
    };
    [[nodiscard]] std::vector<schematic_node_input> schematic_nodes() const;
    [[nodiscard]] schem_view schematic_view() const;
    [[nodiscard]] std::vector<schem_edge> schematic_edges() const;
    // Side-field readout rows (labels only): lanes (commodity+flow), spawn
    // tables, faction stances — the #39/#25b glue the map cannot project.
    [[nodiscard]] std::vector<std::string> schematic_side_fields() const;
    // Rising mouse_down edge: the node marker under the cursor (within
    // kSchemMarkerPx, nearest wins) selects through the SHARED 8.3 service;
    // empty space clears. Panel state tracks the selected node KEY.
    void click_schematic(const gfx::ui_io& io);
    [[nodiscard]] const std::string& schematic_selected() const { return m_schem_sel; }
    // Glue writes ride the source's gate (recook; refused = zero bytes).
    bool schematic_add_edge(const std::string& from_id, const std::string& to_id,
                            std::string* err);
    bool schematic_set_flow(const std::string& lane_id, double flow, std::string* err);

    // Layout constants (tests pin against these exactly).
    static constexpr float kSchemRadiusPx = 400.f;  // max-range ring
    static constexpr float kSchemMarkerPx = 12.f;   // click/hit radius
    static constexpr float kSchemRingPx = 30.f;     // co-located-node ring

    // Draws the map (line primitive from 8.20.0 + marker dots + labels).
    void render_schematic(gfx::ui_context& ui, std::int32_t x, std::int32_t y,
                          std::int32_t w, std::int32_t h);

private:
    selection_service& m_sel;
    int m_tree_scroll = 0;
    std::map<std::string, gfx::rect> m_rects;
    schematic_source* m_ssrc = nullptr;
    std::string m_schem_sel;          // node KEY, not entity
    bool m_prev_down = false;         // click_schematic rising-edge memory
};

}  // namespace ve::game
