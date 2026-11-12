#pragma once

// Phase 8 task 8.2 — the def-forms panel (#43 mode C, editor kit #48).
// A schema-driven form over ANY cooked def type's TOML source:
//   def .toml text  <->  def_forms_doc (line-preserving editable model)
//   panel render    = 8.1 typed widgets over every schema field
//   commit          = leaf change -> candidate source -> host GATE (the
//                     cooklet, #16: cooked bytes stay cooklet-only; git is
//                     undo) -> dirty; the host writes the .toml only.
//
// #16 rule pinned: the panel NEVER writes cooked payloads and never
// mutates def data that a cooklet already accepted — it edits source
// text; watch-mode cook (tools/cook cook_incremental — the same call the
// watcher loop makes) hot-swaps the payload. The gate is injected so this
// layer stays cooklet-free (ve_game does not link ve_cook).
//
// Line model: every leaf (schema field occurrence) maps to its authored
// line(s); set_leaf re-emits ONLY those, preserving comments, blank
// lines, headers, and untouched lines — the write-back is a minimal,
// reviewable TOML delta (git diff is the undo trail).
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ve/game/def_fields.h"
#include "ve/game/def_schema.h"
#include "ve/gfx/ui.h"

namespace ve::game {

class def_forms_doc {
public:
    // Parse source under the schema. On false, *err = "line N: ..." —
    // structures this forms layer cannot represent (unknown keys — forms
    // follow the schema, #43) or non-line-oriented TOML.
    bool load(const def_schema& schema, std::string source, std::string* err);

    const std::string& source() const { return source_; }
    const std::vector<std::string>& paths() const { return order_; }
    std::string value(std::string_view path) const;      // "" if unknown
    const def_field* field(std::string_view path) const; // null if unknown
    // Mutate one leaf and re-emit its source line(s) in place.
    bool set_leaf(std::string_view path, std::string value);

    // Append a new [[row]] block with one literal per REQUIRED schema field
    // (sane defaults; the result is schema-valid *shape*, the gate judges
    // values). The doc is re-parsed so the new leaves behave like loaded
    // ones. Only top-level array rows (no '.') are supported.
    bool add_child(std::string_view row, std::string* err = nullptr);

private:
    struct leaf {
        std::string path, value;
        const def_field* field = nullptr;
        int line = -1;   // owning line (or block start for attr maps)
        int slot = 0;    // literal index within a multi-leaf line
    };
    struct line_src {
        char form = 0;   // 0 = not a leaf line; 's','n','b','A','N','T','X'
        std::string indent, key, comment; // comment = raw trailing text
        std::vector<int> leaf_ids;        // file order = literal order
        int block_start = -1, block_end = -1; // attr-map block lines only
    };

    std::vector<std::string> raw_;      // physical lines ('\n'-joined = source_)
    std::vector<line_src> src_;         // parallel to raw_
    std::vector<leaf> leaves_;
    std::map<std::string, int> idx_;
    std::vector<std::string> order_;    // leaf paths, file order
    std::string source_;
    const def_schema* schema_ = nullptr;  // kept for add_child

    void rebuild_source();
    bool reemit_line(int line_idx);
    bool reemit_block(int leaf_id);
};

class def_forms_panel {
public:
    // gate(candidate_source) -> "" accept, else the error to show inline.
    // The host wires this to the cooklet (stage candidate -> re-cook ->
    // report); #16: write is blocked until the gate says OK.
    using gate_fn = std::function<std::string(const std::string&)>;
    gate_fn gate;

    explicit def_forms_panel(def_forms_doc doc) : doc_(std::move(doc)) {}

    // Ref candidates per leaf path (endpoint wire/socket names, def paths).
    // Unset paths get derived defaults where the def itself can answer
    // (wire ids); caller-supplied lists win.
    void set_candidates(std::string_view path, std::vector<std::string> list) {
        cands_[std::string(path)] = std::move(list);
    }

    // first_leaf/leaf_count slice the leaf list (a full def exceeds any
    // window; the dev UI scrolls, headless tests address fields directly).
    void render(gfx::ui_context& ui, const char* title,
                std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                int first_leaf = 0, int leaf_count = -1);

    // Index of a leaf path in doc().paths() (-1 unknown) — pairs with the
    // render slice to bring a field on screen.
    int leaf_index(std::string_view path) const;

    // Center-clickable rect of a field's primary widget from the LAST
    // render (empty optional if never drawn).
    std::optional<gfx::rect> field_rect(std::string_view path) const;
    // Rect of candidate #i of an OPEN picker at `path` (last render).
    std::optional<gfx::rect> candidate_rect(std::string_view path, std::size_t i) const;

    bool dirty() const { return dirty_; }
    std::string take_write(); // source to write; clears dirty

    const def_forms_doc& doc() const { return doc_; }
    // 8.2 tests host-gate structure edits (add_child) through the panel's
    // doc; widget state keyed by path survives the doc reload.
    def_forms_doc& doc_mut() { return doc_; }
    const std::string& last_gate_error() const { return gate_error_; }

private:
    def_forms_doc doc_;
    std::map<std::string, edit_field> edits_;
    std::map<std::string, enum_field> enums_;
    std::map<std::string, picker_field> picks_;
    std::map<std::string, bool> bools_;
    std::map<std::string, std::vector<std::string>> cands_;
    std::map<std::string, gfx::rect> rects_;
    std::map<std::string, std::vector<gfx::rect>> cand_rects_;
    std::string gate_error_, error_path_;
    bool dirty_ = false;
    bool blocked_ = false; // last commit attempt rejected by gate

    edit_field& edit_state(const std::string& path);
    enum_field& enum_state(const std::string& path);
    picker_field& picker_state(const std::string& path);
    bool bool_state(const std::string& path);
    std::vector<std::string> candidates_for(const std::string& path,
                                            const def_field& f) const;
    // Grammar for kinds the 8.1 widgets don't cover (tag lists, attr maps,
    // int-box grids). Returns true and fills msg on rejection.
    static bool composite_ok(const def_field& f, std::string_view text,
                             std::string& msg);
    // Apply/verify one commit. Returns true if accepted (gate included).
    bool try_commit(const std::string& path, const std::string& text);
};

}  // namespace ve::game
