# Phase 8 — Notes

Design notes, findings, and gotchas worth keeping. One section per task number as work lands.

## Phase-shaping decisions on the table
- **8.6 terrain art-direction spike ends in a user decision** (#17d): stylized low-poly PBR + palette cards vs realistic-leaning, chosen with pixels in hand. 8.18 must not commit the surface look before that decision is recorded.
- Everything else in this phase is pre-ratified (#13, #16, #17, #20, #22, #24, #25, #27, #29, #33, #43, #45, #48).

## Pre-existing machinery this phase builds on (grep before building)
- `gfx::ui_context` (3.13): pure-CPU immediate UI; headless panel pattern = `game/scrubber_panel` (ui_io in, draw list out).
- `core/sim/terrain.h` (7.2/7.3): the shared analytic height+normal sampler + stamp stack already exist — streaming and the spike must call it, never a copy.
- `core/sim/loadout*.h` (Phase 5): the loadout editor's entire data model is headless already.
- `core/sim/swept.h` (7.4): the warp-tick contact primitive (bit-exact TOI across speed decades 1..1e12).
- `core/sim/scrubber.h` + `stats_hash` (6.15/6.5), `well_blend`, `rails`, `reservoir`, `assist`, `preview` (ghost-sim): all live.
- Cooklet: TOML parser + hulldef validation + watch mode + manifest; attr-registry grammar 5.4, extended 7.5.

## 8.1 — def-schema registry + typed form fields
- #42's unit grammar moved to `ve::game` (`units.h`/`units.cpp`): the ratified suffix table became data (`kUnits`, each with its `unit_dim`) and the "NUMBER SUFFIX" string parser became `parse_unit_entry` — ONE implementation shared by cook and forms. `ve::cook::si_from_string` is now a wrapper forwarding here; error strings stay **byte-identical** because `tests/cook/test_toml.cpp` pins their substrings. Cook itself stays dim-agnostic; the FORMS gate adds the per-field dim check the schema declares — `"5 s"` in a length field is refused at entry even though the cooklet would coerce it (#42 "on entry" is stricter by design).
- `def_schema.h`: one registry consumed by forms and cook. Closed enumerations (socket category, face axis, wire type, facet actions) now have exactly ONE spelling table; `hulldef.cpp` indexes `kCategory_names`/`kFace_axis_names`/`kWire_type_names` instead of duplicating literals (encoding contract with core pinned in comments: `socket_category` codes, face code = index+1).
- `def_fields.h/cpp`: typed widgets over `gfx::ui_context` (scrubber-panel pattern, headless): unit/number/integer/text/tag fields validate on Enter with strict grammars (`from_chars` + `isfinite`, dotted-name tag grammar); enum fields cycle ratified options so garbage is unconstructable; ref fields are candidate pickers — they commit only whole candidate spellings the caller supplies, nothing can be typed into them (typoed cross-refs impossible; 8.2 feeds candidates from the live def's ids + manifest). Committed value = authored TEXT, i.e. what 8.2's TOML write-back re-emits.
- gfx addition: `ui_context::refocus_text(label)` — `text_field`'s Enter-drop-commits-still-drops-focus behavior is untouched (3.13 semantics); the REJECT path in def_fields re-focuses the field so the rejected draft stays editable without a re-click (#48: garbage hard to enter, easy to fix).
- Gotcha for 8.2+: a rejected entry keeps focus (refocus_text), a COMMITTED one drops it — panel tests must re-click after every commit; and error labels shift every widget below them, so pinned click rects must account for the error-label state of fields ABOVE in layout order.
- Evidence: `def_form_fields` + `def_form_refpicker` (drives raw `ui_io` keystroke sequences; 96 assertions incl. a byte-identical fresh-context replay determinism check); full suite **148 cases / 8,310,786 assertions** green via `tools/ci_local.sh` (338 s, no display, ASan+UBSan).
