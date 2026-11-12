#pragma once
// Phase 8 task 8.1 — typed def-form field widgets (#43 mode C, #48 editor
// kit; panel pattern per 3.16 scrubber: gfx::ui_context I/O in, edits out,
// headless-testable).
//
// Every def value gets a field that makes garbage *hard to enter*:
//  - unit_str fields run the shared #42 grammar + DIM gate on ENTRY
//    (parse_unit_entry — same code the cooklet parses with; forms add the
//    per-field dimension check the schema declares). Bad text is rejected
//    with the cooklet's own message; the committed value never changes.
//  - number/integer/text/tag fields validate on Enter with strict grammars.
//  - enumeration fields cycle ratified options — free text impossible.
//  - ref fields (#26a/#28a endpoints, seam sockets, facet targets, def
//    paths) are PICKERS: commit only candidate strings the caller supplies
//    (8.2 feeds them from the live def's own ids + manifest), so refs stay
//    live cross-references, never typos.
// Committed values are authored TEXT (what TOML write-back re-emits in 8.2);
// rejected drafts stay editable with the error shown under the field.
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "ve/game/def_schema.h"  // unit_dim

namespace ve::gfx { class ui_context; }

namespace ve::game {

enum class field_edit : std::uint8_t { none, committed, rejected };

// Text-ish field state (unit/number/integer/text/tag). draft = live edit
// buffer; committed = last validated authored text.
struct edit_field {
    char draft[64] = "";
    std::string committed;
    bool error = false;
    std::string message;  // why the current draft was rejected (shown under field)
};

// Seed a field from def data (panel open / hot-reload): draft shows the
// authored text so it can be edited in place.
void prime_field(edit_field& st, std::string_view authored);

// Each draw renders label + input; returns an edit event for this frame.
// ui ids derive from `label` — one field per label per window.
field_edit draw_unit_field(gfx::ui_context& ui, const char* label, edit_field& st,
                           unit_dim want);
field_edit draw_number_field(gfx::ui_context& ui, const char* label, edit_field& st);
field_edit draw_integer_field(gfx::ui_context& ui, const char* label, edit_field& st);
field_edit draw_text_field(gfx::ui_context& ui, const char* label, edit_field& st);
field_edit draw_tag_field(gfx::ui_context& ui, const char* label, edit_field& st);

// Enumeration field: shows the current option; clicking cycles the ratified
// set (wraps). The value is only ever a ratified spelling — garbage is
// unconstructable. prime: unknown authored value snaps to option 0 (cook
// already rejected such defs; belt-and-braces).
struct enum_field {
    std::string value;
    std::string_view set;
};
void prime_enum_field(enum_field& st, std::string_view set, std::string_view authored);
field_edit draw_enum_field(gfx::ui_context& ui, const char* label, enum_field& st);

// Candidate picker: current value as a label, "[pick]" toggles the
// candidate list, clicking a candidate commits it (exactly, prefix
// included — endpoint spellings "socket.<id>"/"component.<id>" arrive
// pre-spelled from the caller). Nothing can be typed here at all.
struct picker_field {
    std::string committed;
    bool open = false;
};
field_edit draw_picker_field(gfx::ui_context& ui, const char* label, picker_field& st,
                             std::span<const std::string> candidates);

// Boolean field: ui checkbox wrapper (commit = any toggle).
field_edit draw_bool_field(gfx::ui_context& ui, const char* label, bool& value);

}  // namespace ve::game
