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


## 8.2 — def-forms panel (TOML source model + real-cooklet gate)
- **Two files, one rule**: `def_forms_doc` = line-oriented TOML model (every leaf knows its authored line(s); `set_leaf` re-emits ONLY those); `def_forms_panel` = immediate-UI render over it. The panel NEVER writes a file or touches cooked bytes — the host does (#16).
- **Host gate is a std::function<const std::string&(const std::string&)>** returning "" or the reason: ve_game stays cook-free; the test-side host stages the fixture into a temp tree and runs `cook_incremental` (parse + hull validate) — the exact watch-loop contract. A rejected candidate never enters the source: try_commit set_leaf → gate(candidate) → on reject set_leaf(old) rollback.
- **Minimal-delta gotchas found by pinning the diff**: block re-emission must take body indent from the FIRST ENTRY line (not the header — fixture tables are 0-indent), must exclude trailing blank lines from `block_end` (file spacing, not table), and must carry authored trailing comments (`# #46e...` on thrust.gimbal_rad) through when the same key is re-emitted. A whole-file line-diff check (`changed == 1` for a value-only edit) is the cheap way to pin all three.
- **Rollback span bug class**: after a block rebuild that changes line count, the touched block's OWN `block_end` must shift too (`>= be`, not `> be`) — otherwise a gate-rejection rollback erases the wrong span and orphans the rejected lines in the source (this is how `def_forms_writeback_blocked` caught it: doc().value clean but take_write() carried `foo.bar`).
- **libstdc++ gotcha**: `std::search` across mixed `char`/`unsigned char` iterator value types silently returns not-found even for an identical byte pattern (verified isolated; memmem/memcmp loops find it). Use memcmp scans for byte-pattern searches.
- **Unregistered attr key proves the layering**: `foo.bar=1` passes the 5.4 entry grammar (dotted lowercase is well-formed) — only the cooklet's registry lookup refuses it, with the key named in the error string. That's the "garbage past the form" net.
- Hotswap evidence: edit component.0 attrs thrust.force 320000→350000 → cooked payload's first `thrust.force` f64 == 350000.0; engine_R's separate attrs row still 320000.0 (swap is per-component, authored order in DEF0).
- gfx additions for hit-testing: `gfx::rect` (namespace-scope, screen px), `ui_context::last_rect()`, `button(label, id_salt)` (same-spelled widgets in one window need distinct ids without dirtying labels), rect out-params on the 8.1 field draws + `cand_out` rects on picker candidates.
- Test gotchas (pinned): every `begin_frame` `ui_io` must OUTLIVE `end_frame` (named locals — temporaries trip ASan stack-use-after-scope in the frame-retention path); attr-map composite drafts hit the 64-char field buffer — raised to 256; composite key trim must strip BOTH sides of `=` (comma-split leaves `" power.draw"` leading spaces → grammar refused).
- Evidence: `def_forms_panel` (full-form edit matrix: unit/number/enum/picker/attr-map/add_child; minimal-delta line diff; fresh-panel replay determinism), `def_forms_writeback_blocked` (cooklet-registry refusal; blocked value absent from take_write; earlier good edit intact), `def_forms_recook_hotswap` (re-cook → payload swap + 1-line delta); 8.2 pinned 3 cases / 216 assertions; full suite **151 cases / 8,311,002 assertions** green via `tools/ci_local.sh` (341 s, no display, ASan+UBSan).

## 8.2.5 + 8.3 — ECS registry + inspector panel (selection spine for the kit)

**Why a box appeared mid-phase:** PLAN #8 ratified "entity + component
arrays" but no Phase 0–7 checklist box ever built a registry — the
sim systems (rails, contacts) own their own stores. 8.3 was the first
consumer, so 8.2.5 (minimal registry, own commit) was inserted per
checklist rule 3. Do NOT read ecs_world as the Phase 12 hot-system
answer: it is the editor-visible entity spine only.

**Registry gotchas (core/ecs.h):**
- No RTTI (AGENTS): dispatch is by REGISTRATION INDEX. `emplace<T>`/
  `get<T>` pair T with the index the CALLER registered — a wrong pair is
  a silent type-pun; the pairing discipline lives at call sites.
- Storage is `shared_ptr<void>`: deleter captured at creation, so
  deleting through void is defined WITHOUT RTTI. register_component_type
  after entities exist RESIZES every slot's comps vector (column growth).
- Generation bumps on REUSE (inside create()), not on destroy() — the
  bump point is then a create, keeping id order deterministic. The
  alive() generation match is mutation-checked (remove it -> ecs_registry
  fails).

**Selection service (game-side, #48 spine):** current entity + expansion
set live in `selection_service`, NOT in the panel — host owns it, any
editor panel reads/drives it (8.4 browser, 8.5 loadout select the same
entities). Expansion kept sorted by (slot, generation) so binary_search
is legal (rule 5: ordered container, explicit total order).

**Inspector panel gotchas:**
- ui has no wheel (#15 keeps input neutral) -> scroll is [^]/[v] buttons
  with clamped row-skip state in the panel; scrolled-away rows have no
  hit rect that frame, so clicks on them are no-ops by construction.
- Two begin_window calls per frame (tree + detail) is fine — the window
  stack is one deep but pairs are sequential within the frame.
- Hit rects captured per frame via ui.last_rect() RIGHT AFTER each
  interactive draw (scroll/twisty/row), keyed "tree:<slot>" /
  "tw:<slot>" — same pattern as 8.2's field_rect.
- TEST PITFALL (caught a real bug): counting glyph COMMANDS to pin text
  presence is wrong — consecutive label runs MERGE into one cmd
  (add_rect flushes runs; labels don't), so 6 extra labels changed the
  cmd count by 0. Pin with `frame.verts.size()` instead (glyph verts
  never merge). And: the twisty `ui.button` return was initially dropped
  on the floor — expand never fired; any button whose click is ignored
  silently no-ops. Tests for both actions go in the SAME commit as the
  panel (done-discipline).

## 8.4 — manifest browser panel (the kit's def-selection spine)
- Panel is **data-driven, file-free**: the host assembles `manifest_row`
  snapshots (`read_manifest` + `file_size(units/<id>.ck)` + live-source
  `sha256` vs entry `source_sha` = the watch-mode stale test) and injects
  them. `def_source` provider likewise keeps file IO host-side, so
  `ve_game` still links no `ve_cook` (#16 rule survives the browser).
- **Def selection is panel state, NOT the 8.3 `selection_service`** — that
  one is entity-keyed (`core::entity_id`); manifest rows are content IDs
  (src-relative paths). 8.5 needs both: entities via the service, defs via
  this panel's `def_handoff`. Don't merge the two selection models.
- Handoff is the 8.2 contract: `{id, exact TOML text}` -> `def_forms_doc::
  load(kHull_def_schema, source)`; provider returning nullopt (meshes/
  fonts) clears it — forms never sees a non-def.
- Manifest fixture pinned numerically at the HOST, not the panel: 3 rows,
  `std::map` content-ID order (def, glb, sock), dep edges `def->glb`
  (`refs =`), `sock->glb` (companion), glb leaf empty. Any future cooklet
  change to manifest shape fails this test before any panel code.
- UI gotchas reused from 8.3: layout budget uses the *public*
  `ui_context::kRowH/kSpacing/kTitleH/kPad` constants (panel and layout
  engine share one source of truth — scroll math stays exact);
  scrolled-away rows get NO hit rect (click where a row "was" does
  nothing); text-presence pins count frame verts, not cmds.
- **Test-quality lesson (mutation checks caught a lying pin):** the STALE
  badge pin initially compared glyph-vert counts across a *selection
  change* — it passed even with both badges deleted. Baseline must be
  taken with the same selection held, so only the badge itself can move
  the count. Rule: any presence pin via vert-count delta needs a baseline
  where the flagged artifact is the ONLY delta.
- `NOT COOKED` badge proven by deleting the unit file then re-stats
  (`file_size` 0-path); re-cook restores it — same staged-tree cooklet
  doctrine as 8.2's recook hotswap.
