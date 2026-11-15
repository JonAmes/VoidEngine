#pragma once
// Phase 8 task 8.19 (decision #43 mode A) — 3D-scene authoring gizmos.
//
// The author-time service under the mode-A editor: the suite picks a gizmo
// handle in the live cooked system and drags it; this layer turns the drag
// into a field edit on the SOURCE def text, round-trips the edit through
// the real incremental cooker (schema + 8.8 cross gates re-run on every
// write), and hot-swaps the cooked payloads + assembled rail_world on
// success. Gate failure reverts the source text bit-exact, so an invalid
// drop writes NOTHING (#43: "all write back TOML only" — #16 rule, git is
// the undo trail; there is no in-memory-only mutation path).
//
// Lives in the cooklet library on purpose: authoring is cook-side work
// (the same cook_worlddef/wrldvalidate/cook_incremental the CLI runs), and
// ve_cooklet already links ve_game, so selection_service (8.3) rides in.
// core/ is untouched — this layer is pure def-text + cooked-data surgery.
//
// Handles ride the assembled rail_world: positions are the ANALYTIC rail
// evaluation at the given tick (8.19 drags are "where the body is now"),
// never sampled render state. The pick map is the SAME off-axis pinhole x/y
// map the 8.10 log-zoom pass uses (VR rule: off-axis tangents from day one);
// the Tier-2 test pins CPU pick == GPU pixel placement with one shared
// projection (test_author_gizmo_pick).
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ve/core/ecs.h>
#include <ve/core/math/vec.h>
#include <ve/core/sim/rails.h>       // rail_world member (complete type)
#include <ve/game/inspector_panel.h> // selection_service (8.3, shared pick)
#include <ve/game/world_load.h>      // WorldDef member (complete type)

namespace ve::cook {

// One gizmo handle: what pick returns / what a drag targets.
struct gizmo_handle {
    enum class kind { body, waypoint, poi };
    std::string def_path;   // key into sources() (src-tree relative path)
    std::string row;        // waypoint id / poi name ('' for def-level)
    kind k = kind::body;
};

// The view a pick runs under. Orthonormal basis (f = forward); tangents are
// the off-axis tan set; near_d/far_d are the 8.10 log-zoom budget endpoints
// (pick depth tie-break = smaller view distance wins; log map is monotone).
struct gizmo_view {
    core::math::dvec3 eye{0.0, 0.0, 0.0};
    core::math::dvec3 f{0.0, 0.0, -1.0}, s{1.0, 0.0, 0.0}, u{0.0, 1.0, 0.0};
    double tan_l = -1.0, tan_r = 1.0, tan_b = -1.0, tan_t = 1.0;
    double near_d = 1.0, far_d = 1.0e12;
    int width_px = 0, height_px = 0;
};

// World -> framebuffer px under the pinhole x/y map, mirroring the Vulkan
// y-down framebuffer convention (same formulas the 8.10 depth pins use).
// nullopt at/behind the eye plane.
std::optional<std::pair<double, double>> project_px(const core::math::dvec3& world,
                                                    const gizmo_view& view);

// The cooklet's authoring service implements the game-side schematic source
// interface (8.20, #43): the mode-B panel edits this same live source tree,
// so mode A and mode B can never drift into two data models.
class author_world : public ve::game::schematic_source {
public:
    // Load every def source (map key = src-tree-relative path, e.g.
    // "world/system.toml"; values = TOML text). The texts are written to
    // src_dir and cooked through cook_incremental into out_dir — the exact
    // production path: schema gates + wrldvalidate cross gates included.
    // On false: *err = the first gate failure, nothing is live.
    bool load(const std::map<std::string, std::string>& sources,
              const std::string& src_dir, const std::string& out_dir,
              std::string* err);

    // Live source texts (the write-back trail: diff sources()[p] before and
    // after an op to see the exact TOML delta).
    const std::map<std::string, std::string>& sources() const { return m_sources; }
    // Parsed+decoded defs, keyed by the same path keys.
    const ve::game::WorldDef* def(std::string_view path) const;
    core::sim::rail_world& rails() { return m_rails; }
    const core::sim::rail_world& rails() const { return m_rails; }
    std::int32_t body_id(std::string_view path) const;  // -1 = not a body/station

    // ---- schematic_source (8.20 mode B) ----
    // stations (def-path order), then waypoints, then market nodes; positions
    // are the same analytic rail evaluations the gizmo handles ride.
    std::vector<ve::game::schematic_node_input> schematic_nodes(std::int32_t tick) const override;
    const ve::game::WorldDef* sys_def() const override;   // the system-class def
    // Glue-row writes ride the SAME recook gate as the scalar drags:
    // accepted = the new/edited row is in the live source text and cooked;
    // refused = zero bytes changed (bit-exact revert + old tree re-cooked).
    // fields/values are TOML literals as authored (quote strings yourself).
    bool add_glue_row(const std::string& table,
                      const std::vector<std::pair<std::string, std::string>>& fields,
                      std::string* err) override;
    bool set_glue_field(const std::string& table, const std::string& key_field,
                        const std::string& key_value, const std::string& field,
                        const std::string& value_literal, std::string* err) override;

    // Bind the 8.3 selection service + a def-path -> entity map (the game
    // world's entities for these defs). pick_at then selects through it.
    void bind_selection(game::selection_service* svc,
                        std::map<std::string, core::entity_id> entities) {
        m_sel = svc;
        m_entities = std::move(entities);
    }

    // ---- drag ops. Every op: drop gate -> text edit -> re-cook -> swap;
    // any failure reverts the text bit-exact and *err carries the gate's
    // first-failure message (invalid drop = no write-back, nothing swapped).
    //
    // Body/station drag: the world-space target at `tick` projects onto the
    // parent's rail plane (circular coplanar rails #45c): new
    // orbit_radius/orbit_phase place the body exactly at the cursor NOW.
    // Gates: target finite; radius > 0 and clear of the parent's surface;
    // a root-anchor body refuses (#45c: the static anchor never moves).
    bool drag_body(std::string_view def_path, const core::math::dvec3& target,
                   std::int32_t tick, std::string* err);

    // System [[waypoint]] node: free 3D drag of the `at` triple. This IS
    // the rail/lane-curve drag — [[edge]]/lane rows reference waypoint ids,
    // so moving the node moves every curve through it.
    bool drag_waypoint(std::string_view sys_path, std::string_view wp_id,
                       const core::math::dvec3& at, std::string* err);

    // Body [[poi]] pad: move the center direction on the unit sphere
    // (re-normalized; radius/level/blend/footprint ride unchanged).
    bool drag_poi(std::string_view body_path, std::string_view poi_name,
                  const core::math::dvec3& center_dir, std::string* err);

    // ---- handles & pick. World position of every draggable handle at the
    // tick (body/station centers; waypoint `at` riding its body if parented;
    // poi centers on the body surface along their direction).
    struct handle_pos { gizmo_handle h; core::math::dvec3 world; };
    std::vector<handle_pos> handles(std::int32_t tick) const;

    // Nearest handle to a framebuffer-px cursor within `radius_px`, ties
    // broken by distance to the eye (nearer wins). Selects through the
    // bound selection service when bound; returns nullopt on no hit.
    std::optional<gizmo_handle> pick_at(double px, double py, double radius_px,
                                        const gizmo_view& view,
                                        std::int32_t tick) const;

private:
    std::string sys_path() const;  // path key of the system-class def ('' none)
    struct cooked_def {
        ve::game::WorldDef def;
    };
    std::map<std::string, std::string> m_sources;
    std::map<std::string, cooked_def> m_defs;
    core::sim::rail_world m_rails;
    std::map<std::string, std::int32_t> m_body_ids;
    std::string m_src, m_out;
    game::selection_service* m_sel = nullptr;
    std::map<std::string, core::entity_id> m_entities;

    // Re-cook from m_sources into out_dir, reload payloads + rails.
    // On cook failure: false + *err (caller reverts text first or after).
    bool recook_and_swap(std::string* err);
    // Refused drop: restore the source text bit-exact and re-cook the old
    // tree (disk + units + rails back to pre-drag state).
    void revert(const std::string& path, const std::string& old_text);
    bool assemble_rails(std::string* err);
};

}  // namespace ve::cook
