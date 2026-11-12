// Phase 8 task 8.1 — typed def-form field widgets (see def_fields.h).
// All widgets are pure ui_context I/O (3.13 immediate UI, 3.16 headless
// panel pattern): no game state, no TOML — 8.2 composes these into the
// forms panel and owns write-back.
#include "ve/game/def_fields.h"

#include <charconv>
#include <cmath>
#include <cstring>

#include "ve/game/attr_keys.h"
#include "ve/game/units.h"
#include "ve/gfx/ui.h"

namespace ve::game {
namespace {

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Strict bare-number parse (same strictness as the cooklet's number path:
// from_chars rejects inf/nan spellings — never valid def data).
bool parse_bare_double(std::string_view tok, double& out) {
    const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), out);
    return res.ec == std::errc{} && res.ptr == tok.data() + tok.size() && std::isfinite(out);
}

}  // namespace

void prime_field(edit_field& st, std::string_view authored) {
    const std::size_t n = authored.size() < sizeof st.draft - 1 ? authored.size() : sizeof st.draft - 1;
    std::memcpy(st.draft, authored.data(), n);
    st.draft[n] = '\0';
    st.committed = std::string(authored);
    st.error = false;
    st.message.clear();
}

field_edit draw_unit_field(gfx::ui_context& ui, const char* label, edit_field& st,
                           unit_dim want, gfx::rect* out) {
    const bool enter = ui.text_field(label, st.draft, sizeof st.draft);
    if (out) *out = ui.last_rect();  // text box rect (panel hit target)
    if (enter) {
        // #42 ON ENTRY: shared grammar + the schema's dim gate. The
        // message is the cooklet's own wording (entry_error_message).
        auto r = parse_unit_entry(st.draft, want);
        if (!r) {
            st.error = true;
            st.message = entry_error_message(r.error());
            ui.refocus_text(label);  // rejected draft stays editable (#48)
        } else {
            st.committed = std::string(trimmed(st.draft));
            st.error = false;
            st.message.clear();
        }
    }
    if (st.error) ui.label(st.message.c_str());
    return enter ? (st.error ? field_edit::rejected : field_edit::committed) : field_edit::none;
}

field_edit draw_number_field(gfx::ui_context& ui, const char* label, edit_field& st,
                             gfx::rect* out) {
    const bool enter = ui.text_field(label, st.draft, sizeof st.draft);
    if (out) *out = ui.last_rect();
    if (enter) {
        double v = 0.0;
        const auto tok = trimmed(st.draft);
        if (!parse_bare_double(tok, v)) {
            st.error = true;
            st.message = "not a plain number (SI/dimensionless, #42 scope)";
            ui.refocus_text(label);
        } else {
            st.committed = std::string(tok);
            st.error = false;
            st.message.clear();
        }
    }
    if (st.error) ui.label(st.message.c_str());
    return enter ? (st.error ? field_edit::rejected : field_edit::committed) : field_edit::none;
}

field_edit draw_integer_field(gfx::ui_context& ui, const char* label, edit_field& st,
                              gfx::rect* out) {
    const bool enter = ui.text_field(label, st.draft, sizeof st.draft);
    if (out) *out = ui.last_rect();
    if (enter) {
        long long v = 0;
        const auto tok = trimmed(st.draft);
        const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), v);
        if (res.ec != std::errc{} || res.ptr != tok.data() + tok.size()) {
            st.error = true;
            st.message = "not an integer";
            ui.refocus_text(label);
        } else {
            st.committed = std::string(tok);
            st.error = false;
            st.message.clear();
        }
    }
    if (st.error) ui.label(st.message.c_str());
    return enter ? (st.error ? field_edit::rejected : field_edit::committed) : field_edit::none;
}

field_edit draw_text_field(gfx::ui_context& ui, const char* label, edit_field& st,
                           gfx::rect* out) {
    const bool enter = ui.text_field(label, st.draft, sizeof st.draft);
    if (out) *out = ui.last_rect();
    if (enter) {
        const auto tok = trimmed(st.draft);
        if (tok.empty()) {
            st.error = true;
            st.message = "must not be empty";
            ui.refocus_text(label);
        } else {
            st.committed = std::string(tok);
            st.error = false;
            st.message.clear();
        }
    }
    if (st.error) ui.label(st.message.c_str());
    return enter ? (st.error ? field_edit::rejected : field_edit::committed) : field_edit::none;
}

field_edit draw_tag_field(gfx::ui_context& ui, const char* label, edit_field& st,
                          gfx::rect* out) {
    const bool enter = ui.text_field(label, st.draft, sizeof st.draft);
    if (out) *out = ui.last_rect();
    if (enter) {
        const auto tok = trimmed(st.draft);
        if (!is_valid_attr_key(tok)) {  // same dotted-name grammar as attr keys/tags
            st.error = true;
            st.message = "tag must be a.b_c (lowercase letters/digits/underscores, dotted)";
            ui.refocus_text(label);
        } else {
            st.committed = std::string(tok);
            st.error = false;
            st.message.clear();
        }
    }
    if (st.error) ui.label(st.message.c_str());
    return enter ? (st.error ? field_edit::rejected : field_edit::committed) : field_edit::none;
}

void prime_enum_field(enum_field& st, std::string_view set, std::string_view authored) {
    st.set = set;
    const auto opts = enum_options(set);
    bool found = false;
    for (const auto& o : opts)
        if (o == authored) found = true;
    st.value = found ? std::string(authored)
                     : (opts.empty() ? std::string{} : std::string(opts[0]));
}

field_edit draw_enum_field(gfx::ui_context& ui, const char* label, enum_field& st,
                           gfx::rect* out) {
    ui.label(label);
    const bool pressed = ui.button(st.value.c_str(), label);  // salt: many
    if (out) *out = ui.last_rect();                           // enums per window
    if (pressed) {
        const auto opts = enum_options(st.set);
        if (opts.empty()) return field_edit::none;
        std::size_t i = 0;
        for (std::size_t k = 0; k < opts.size(); ++k)
            if (opts[k] == st.value) i = k;
        st.value = std::string(opts[(i + 1) % opts.size()]);
        return field_edit::committed;
    }
    return field_edit::none;
}

field_edit draw_picker_field(gfx::ui_context& ui, const char* label, picker_field& st,
                             std::span<const std::string> candidates,
                             gfx::rect* out, std::vector<gfx::rect>* cand_out) {
    ui.label(label);
    ui.label(st.committed.empty() ? "(unset)" : st.committed.c_str());
    const bool toggled = ui.button("[pick]", label);  // salted ids: 8.2 windows
    if (out) *out = ui.last_rect();                   // carry many pickers
    if (toggled) st.open = !st.open;
    field_edit edit = field_edit::none;
    if (st.open) {
        for (const auto& c : candidates) {
            const bool hit = ui.button(c.c_str(), label);
            if (cand_out) cand_out->push_back(ui.last_rect());
            if (hit) {
                st.committed = c;  // exact candidate spelling — refs cannot be typos
                st.open = false;
                edit = field_edit::committed;
                break;
            }
        }
    }
    return edit;
}

field_edit draw_bool_field(gfx::ui_context& ui, const char* label, bool& value,
                           gfx::rect* out) {
    const bool toggled = ui.checkbox(label, &value);
    if (out) *out = ui.last_rect();
    return toggled ? field_edit::committed : field_edit::none;
}

}  // namespace ve::game
