#pragma once
// Phase 8 task 8.4 — manifest/asset browser panel (#16 roster): a view of
// manifest.bin — stable content IDs (= src-relative source paths), dep
// graph, cooked sizes, re-cook status — where selecting a def hands its
// source to the 8.2 forms panel (the editor kit spine, #48).
//
// The panel NEVER reads files: the host assembles manifest_row snapshots
// (core::cooked::read_manifest + cooked-file stat + live-source sha256 —
// the watch-mode stale comparison) and injects them. ve_game still links
// no ve_cook (#16); the def-source handoff goes through an injected
// provider so the panel stays file-free.
//
// Note on selection: this panel tracks a CONTENT ID (a def path), not an
// ECS entity — the 8.3 selection_service is entity-keyed, so def
// selection is panel state here by design; 8.5's loadout editor selects
// entities (via the service) AND defs (via this panel's handoff).
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ve/gfx/ui.h"

namespace ve::game {

// One manifest.bin entry plus the host-side file facts the manifest does
// not carry. deps arrive exactly as cooked (sorted by the cooklet).
struct manifest_row {
    std::string id;                 // content ID == src-relative path
    std::vector<std::string> deps;
    std::uint64_t cooked_size = 0;  // bytes of units/<id>.ck; 0 => not cooked
    bool stale = false;             // live source sha != manifest source_sha
};

// What a selection hands to the forms panel (8.2): the def's content ID
// and the exact TOML source text the provider returned.
struct def_handoff {
    std::string id;
    std::string source;
};

class manifest_browser_panel {
public:
    // id -> def source text (the HOST reads the file; nullopt = not a
    // form-editable def, e.g. meshes/fonts/shaders). Consulted on select;
    // the result is held until the selection changes.
    using source_provider =
        std::function<std::optional<std::string>(const std::string& id)>;
    source_provider def_source;

    // Snapshot in content-ID order (manifest std::map order — sorted,
    // deterministic). A selection whose id vanished from the snapshot
    // drops selection + handoff with it.
    void set_rows(std::vector<manifest_row> rows);

    // Two windows: entry list (left — [^]/[v] row scroll, whole-row
    // culling: a scrolled-away row has no hit rect that frame) and entry
    // detail (right — size / NOT COOKED / STALE badge / clickable deps =
    // graph navigation; a dep jump also scrolls the row into view).
    // Hit-rect keys (recorded every render):
    //   "mrow:<id>"   list row (selects; handoff if provider answers)
    //   "dep:<id>"    dep link in the detail (graph jump)
    //   "scroll_up" / "scroll_dn"
    void render(gfx::ui_context& ui, std::int32_t x, std::int32_t y,
                std::int32_t w, std::int32_t h);

    [[nodiscard]] std::optional<gfx::rect> rect_for(const std::string& key) const;
    [[nodiscard]] const std::string* selected_id() const {
        return m_sel ? &*m_sel : nullptr;
    }
    [[nodiscard]] const def_handoff* handoff() const {
        return m_handoff ? &*m_handoff : nullptr;
    }
    [[nodiscard]] int scroll() const { return m_scroll; }
    [[nodiscard]] std::size_t row_count() const { return m_rows.size(); }

private:
    std::vector<manifest_row> m_rows;
    std::optional<std::string> m_sel;
    std::optional<def_handoff> m_handoff;
    int m_scroll = 0;
    std::map<std::string, gfx::rect> m_rects;

    int find_row(const std::string& id) const;
    void select_id(const std::string& id);
};

}  // namespace ve::game
