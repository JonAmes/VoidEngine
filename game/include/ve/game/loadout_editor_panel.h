#pragma once
// Phase 8 task 8.5 — loadout editor dev panel (#48d; #16 rule: editors are
// debug-UI panels in the running engine, write-back is TOML ONLY, watch-mode
// cook closes the loop, git is undo; #24c: the diegetic hangar UI in Phase 11
// drives this same data model, #24d: a hull swap replaces the whole loadout).
//
// Headless twin like 8.2/8.3/8.4: ui_io in, draw list out. File-free: the
// host injects `loadout_snapshot`s (cooked DEF0+SOCK via
// core::sim::load_hull_loadout + the def's live TOML text) — ve_game still
// links no ve_cook, the panel never opens a file.
//
// Gates are the SIM's own: every mount/unmount re-runs
// core::sim::Loadout::assemble, so a type mismatch is refused ON ENTRY with a
// reason drawn in the panel (#48: garbage hard to enter, easy to fix) — the
// candidate never enters the working state. Readout (mass / power supply /
// draw) comes from the stats spine: inertia_from_parts + the power attr keys
// (game-side names, core stays agnostic).
//
// Selection split stays per 8.4: entity selection would live in the 8.3
// selection_service; hull DEF selection is this panel's own working state
// (the host can seed it from the 8.4 browser's def_handoff).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ve/core/sim/inertia.h"
#include "ve/core/sim/loadout.h"
#include "ve/gfx/ui.h"

namespace ve::game {

// One catalogued part the editor can mount: default attrs carry BOTH the
// numeric value (readout math) and the authored TOML literal text (writeback
// keeps #42 unit suffixes verbatim — the panel does not reformat numbers).
struct loadout_catalog_attr {
    std::string key;
    double value = 0.0;
    std::string toml_literal;  // TOML value text as authored, e.g. "900" or "\"120 km\""
};

struct loadout_catalog_part {
    std::string type_id;
    core::sim::socket_category category = core::sim::socket_category::utility_;
    std::vector<loadout_catalog_attr> attrs;
    std::vector<std::string> tags;
};

// One hull option: snapshot of cooked hull data + its live def source. The
// panel deep-copies these and edits working `mounts` in place.
struct loadout_snapshot {
    std::string def_id;  // hulldef manifest content id
    std::string label;   // display name
    std::string source;  // hulldef TOML text (writeback base)
    core::sim::hull_info hull;
    std::vector<core::sim::socket_desc> sockets;
    // the whole known component-type set (assemble validates against this;
    // the catalog is only the pickable subset).
    std::vector<core::sim::component_type> types;
    std::vector<loadout_catalog_part> catalog;
    std::vector<core::sim::mount_request> mounts;  // current kit
};

class loadout_editor_panel {
public:
    // Host-injected hull options, ordered by def_id (deterministic rows).
    void set_hulls(std::vector<loadout_snapshot> hulls);

    void render(gfx::ui_context& ui, std::int32_t x, std::int32_t y,
                std::int32_t w, std::int32_t h);

    // Hit rects for headless tests (keys "use:<def_id>", "un:<socket_id>",
    // "pick:<socket_id>", "cat:<type_id>").
    gfx::rect rect_for(const std::string& key) const;

    // ---- state for pins ----------------------------------------------------
    const std::string* current_def_id() const;
    std::size_t mount_count() const;  // working mounts of the current hull
    double total_mass() const;        // inertia_from_parts hull + mounts
    double power_supply() const;      // sum power.supply attrs
    double power_draw() const;        // sum power.draw attrs
    // Non-null after an on-entry refusal; cleared by any successful edit or
    // selection change. Format: "<error name> (<type> @ <socket>)".
    const std::string* last_reject() const;

    // Write-back (#16): current hull def's TOML rewritten to the working
    // mount list — unmounted components' blocks removed, new mounts appended
    // as [[component]] blocks; everything else (root keys, wires, facets,
    // comments) passes through untouched. The host writes the text and the
    // watch-mode cook closes the loop; the panel never touches a file.
    std::string emit_source() const;

private:
    static core::sim::socket_desc* find_socket(loadout_snapshot& s, const std::string& id);
    // Re-run the sim's own validity pass over a candidate mount list; refuse
    // (reason recorded) or commit.
    bool apply(std::vector<core::sim::mount_request> candidate,
               const std::string& attempted_type, const std::string& attempted_socket);
    std::vector<core::sim::mass_point> mass_parts(const loadout_snapshot& s) const;

    std::vector<loadout_snapshot> m_hulls;
    std::size_t m_cur = 0;
    std::optional<std::string> m_pick_socket;  // picker window target
    std::optional<std::string> m_reject;
    std::map<std::string, gfx::rect> m_rects;
};

} // namespace ve::game
