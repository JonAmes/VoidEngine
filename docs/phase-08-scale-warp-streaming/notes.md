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

## 8.5 — loadout editor dev panel (kit's last panel; #48 editor kit complete)

- **Gates are the sim's, not the panel's.** Every mount/unmount candidate re-runs
  `sim::Loadout::assemble(hull, sockets, types, candidate)`; a failing verdict
  names the reason (fixed table over `loadout_error`) with the attempted part +
  socket, the candidate never enters the working mount list. Mutation-checked:
  bypassing the assemble check fails both cases; removing the REFUSED label
  fails the presence pin; making emit keep removed regions fails 3 pins.
- **On-entry refusal beats pre-filtering** (#48 "garbage hard to enter, easy to
  fix" — the picker is NOT pre-filtered by socket category; the gate refuses
  with a reason. Same doctrine as the 8.1 unit-suffix gate #42.)
- **File-free boundary survives** (#16): host injects `loadout_snapshot`
  {cooked DEF0+SOCK decoded via `load_hull_loadout`, live def TOML text,
  host-built part catalog}; panel emits source text only. `ve_game` links no
  `ve_cook` (re-verified).
- **Selection split holds** (from 8.4): hull-def selection is panel state (per
  hull option, each with its own working mount list — #24d swap replaces the
  whole loadout); entity selection stays in the 8.3 `selection_service`.
- **TOML emit semantics** (line-oriented, extends the 8.2 doc model): exact
  `[[component]]` starts a region; `[component.attrs]` / `[[component.facet]]`
  continue it; unmounted regions vanish; new blocks append after the last
  region (EOF when the def had none); comments, wires, blank spacing, and #42
  suffixes (`15 deg`, `150 km`) untouched. Reload equality: panel order ==
  reloaded `mounts()` order == def component order (11/11 after edit+recook).
- **UI test traps re-confirmed:** a click that changes state can insert/remove
  label rows (REFUSED line, mount info), shifting later rows — the harness must
  render a settle frame after each click before trusting `rect_for` again
  (otherwise a click lands one row above the intended widget; caught live).
  `sanitize_id` lowercases, so generated instance ids are lowercase-stable.
- Readouts: `total_mass` = hull + `inertia_from_parts(mounts).total_mass`
  (fixture pins: 100670 kg A / 51670 kg B); supply/draw sum game attr keys
  (170000 W / 57300 W) — core never sees key names.
- **Editor kit (#48) 8.1–8.5 complete.** Next: 8.6 terrain spike (throwaway,
  ends with a user decision presentation), then world work 8.7+.

## 8.6 — terrain art-direction spike (#17d, throwaway by checklist design)

- **What it is:** `ve_demo --mode terrain` renders the *same* 7.2 analytic
  sampler (authored stamps: pad, 2 mesas, crater) through one fixed camera,
  fixed sun, one pipeline — two CPU meshes of one face-atlas:
  - **stylized:** 24-wide grid, flat per-triangle facets, cel-quantized N·L
    bands, band-edge lines, height-band palette cards. 20,736 v / 6,912 t.
  - **realistic-leaning:** 132-wide grid, smooth `terrain_normal` normals,
    continuous height/slope color blend, Blinn sheen, distance haze.
    106,134 v / 209,088 t.
  Zero baked meshes (#29 intact) — every vertex is a `terrain_height` sample.
- **No new gfx/core surfaces:** spike reuses the 7.16 `Cam` UBO contract +
  `mesh_vertex` layout; the uv channel is *re-purposed* as
  `(height param, slope param)` — spike-local semantics live in the shader
  and demo only, not in core/gfx. Push constant `look.x` picks the look so
  both renders share pipeline/rig/descriptor — the comparison is shading +
  tessellation, nothing else can differ.
- **Self-attestation:** exit 0 only if both looks painted (>= 0.30 sky-minus
  fraction of the capture) AND the looks differ (>= 0.05 differing pixels);
  exit 4 on capture/paint/diff failure, 2 on setup refusal.
  `tools/dev/terrain_spike.sh` (not ctest — spike rule) re-decodes the PNGs
  on disk with stdlib zlib and re-runs the sky-surround / painted-center /
  diff checks independently, so a capture bug can't hide behind the demo's
  own report. Evidence run: painted 0.580 / 0.611, look_diff 0.612,
  validation ON zero problems, median present 85.8 ms.
- **Process traps (this spike):**
  - `gpu_mesh::upload` refuses `index_count == 0` — a "flat-shaded" mesh
    still needs per-triangle indices (3 verts/tri in the index buffer), not
    a bare vertex list.
  - The WSI presenter hard-errors `present() without acquire()` — every
    hand-rolled render loop must call `pres.acquire()` before
    `begin_command()` (flight loop does; a copy-paste miss costs a round).
  - Validation layer caught nothing in the new pass on the first clean
    run once those two were fixed — the 7.16 pass-shape (dynamic rendering,
    one set, push range) is a good template for ad-hoc passes.
- **Decision gate (open):** which look (or a blend) becomes the game's
  terrain art direction — USER decision, recorded in PLAN.md before 8.18
  commits anything terrain.

## 8.7 — world-def TOML schema + WORLD cook unit (commit 1b0099a)
- **One codec, both sides (DEF0 precedent):** `game/{include/ve/game,src}/world_load.*`
  owns the WORLD0 payload — `encode_world` / `decode_world` / `WorldDef`. The
  cooklet parses TOML into the same structs and encodes; the runtime will
  decode. `worlddef_cook` pins `encode(decode(payload)) == payload`
  byte-for-byte for every cooked unit, so the two directions can never drift
  silently. Payload speaks game concepts (market/lane/faction/build plan), so
  the codec lives in `ve::game`, not `core/`.
- **Unit coercion at cook (#42):** every length/time/angle scalar in the schema
  is a `"NUMBER SUFFIX"` string (`"6200 km"`, `"27.3 day"`, `"24 deg"`); structs
  and payload only ever hold SI. **Composite units ride bare SI** —
  `surface_g` (m/s^2) and `atm_rho0` (kg/m^3) have no ratified #42 suffix, so
  they follow the hull-attr rule (bare numbers) instead of inventing one.
- **Routing:** a `.toml` source with root `world_class = "system"|"body"|"station"`
  cooks to `WRLD` (`kUnitWrld`, cook format **v6**); every other `.toml` stays
  on the DEF0 hull-def path. No new source extension.
- **Flat-row AOT schema:** `[[moon]]`-style nesting under other AOT rows is NOT
  used; every section is a flat root-level row array (`[[lane]]`, `[[edge]]`,
  `[[job_step]]`, ...) — sturdier subset parsing, and it maps 1:1 onto future
  forms panels. `members` is a string *array*, not rows.
- **Gate doctrine:** schema gates run in fixed code order, first failure only;
  every failure writes through `Schema::errp` to the caller's err (a bare
  `sc.err = ...; return false` once escaped silently — the write-through helper
  `bad()` now backs every path). Error context grows per class/row
  (`world body 'Vesper Prime' stamp 1: ...`).
- **Manifest edges:** `WorldDef::collect_refs()` (parent, hull def, members,
  build-plan modules) become sorted deps; cook.cpp now **dedupes** sorted deps
  (a station's hull def and its build modules are the same def — one edge).
  Dangling refs fail at cook time via the existing def-ref resolution (#27a).
- **Decode tamper ladder (pinned):** empty payload, truncated mid-field
  ("truncated"), trailing junk ("trailing"), out-of-range class byte — the
  class byte is guarded twice (explicit range check + switch default arm);
  the mutation below only went red with BOTH removed.
- **Evidence:** `worlddef_cook` + `worlddef_manifest_deps` + `worlddef_gate_order`
  = 3 cases / 156 assertions; full suite 159 / 8,311,426; ci_local GREEN
  375.6 s, CI_EXIT=0. Mutations caught: err-plumbing removed (gate_order,
  8 failures), dep dedup removed (manifest_deps, 1), both class-byte gates
  removed (worlddef_cook, 1).
- **Process traps (this task):**
  - Compound short-circuit chains `!strf(...) || !x.empty()` invert silently —
    the lane from/to check required the fields to be EMPTY. Read every
    `||`-chain term twice; a green parse of well-formed fixtures is the tell.
  - Unknown AOT keys are silently NOT schema data: a dangling-ref fixture must
    corrupt a key the schema actually reads (`members` array), not an invented
    `[[moon]]` row — the first dangling fixture "passed" because nothing parsed it.
  - Cook output layout is `out/units/<id>.ck` + `out/manifest.bin` (readers in
    `core::cooked`), not `out/<id>.vecook` — copy paths from test_hulldef, not
    from memory.

## 8.8 — world-def validation gates (commit ba283fe)
- Second cook pass `cook_wrld_validate` (tools/cook/src/wrldvalidate.cpp) runs
  after the 5.9b hull pass, over touched WRLD units in manifest (content-ID)
  order, fixed gate order per class — deterministic first-error text, same
  doctrine as hull validation. World/hull deps load transitively from the
  manifest; a missing dep is a gate message, never a crash.
- The pad-normal ∥ well-normal gate (#29c) is PROOF-BASED, not a tolerance
  check: a pad POI rides a flatten/mesa stamp, whose full-weight core is
  bit-constant height (terrain.h derivation), so the pad plane is parallel to
  the well normal by construction. The gate instead pins the preconditions of
  that proof: stamp type, radius covers footprint, and blend core still covers
  footprint (a fat blend ring would let the pad sit on sloped falloff).
- `resolve_ref` is purely lexical — existence of refs is validation's job.
  Market anchors + waypoint bodies joined `collect_refs()` so they become
  manifest edges (dangling ones now fail at the cook's ref stage); `poi:<name>`
  anchors deliberately do NOT: they point inside member bodies and are
  resolved by the wrld graph gates instead.
- Fixture trap: relative refs resolve against the DEF's own directory — a ref
  from `world/system.toml` to a root-level def is `../defs/...`, not
  `defs/...`. Two variants failed for this alone.
- Unreachable-by-TOML gates exist on purpose: duplicate faction names are
  already a schema gate (8.7), so the wrldvalidate dup check is defensive.
  Don't write a test chasing a message a schema gate always beats.
- doctest `CHECK_MESSAGE(err.find(n) != npos, ...)` prints `npos` for BOTH
  sides when the needle is a raw `const char*` streamed into the message —
  wrap it in `std::string{}` or the failure log is two identical hex blobs.

## 8.9 — floating-origin rebasing + camera-relative rendering (commit 227772d)

- `sim::OriginTracker` (core/include/ve/core/sim/origin.h, header-only) is the
  sim-side origin state. The rebase rule is **evaluated exactly once per tick**
  on post-step state: `advance(tick, focus_pos, threshold)` after each
  `replay_step`. Strict `>` crossing at an exact tick; equality does NOT fire
  (pinned: 8 m/tick, threshold 1000 m fires at 126/252/378, not 125).
- Replay contract: rebasing mutates ONLY the tracker — positions stay absolute
  fp64 (1.7 doctrine), dynamics never read the origin — so per-tick state
  hashes (4.7) are bit-identical with and without rebasing, and the rebase
  tick SEQUENCE itself replays identically for identical command logs
  (`floating_origin` pins both).
- `origin_frame::to_local_f32` (coords.h) is the ONLY sanctioned world→GPU
  path: fp64 subtract FIRST, f32 cast SECOND. Pinned numerically at 1 AU:
  fp32 ulp at 1.496e11 m is 16384 m; two bodies 10 m apart collapse onto one
  bucket under the absolute-cast path (diff exactly 0, rounding error exactly
  7998.5 m hand-computed), while the relative path keeps the 10 m separation
  BIT-exact (binary-rational locals) through trs_compose + look_at_rh f32
  (`camera_relative_precision`).
- Render callers (7.15/7.16 chase-eye proofs) already composed camera-relative
  (fp64 anchor subtract); the tracker centralizes the anchor + rule so 8.10
  log-zoom, 8.19 authoring, FTL (8.11+), and terrain streaming (8.17/8.18)
  share one origin instead of per-caller anchors.
- Traps: `needs_shift` is const — a render frame probing it cannot move state
  (tested). Threshold choice: the fall-world drift is only ~1 km/min, so a
  5 km corridor rebases ONCE in the hash-equality run — use ~250 m there to
  exercise multi-rebase sequences. Mutation-checked: `>` → `>=` reds the
  exact-tick pins (11 assertions); cast-then-subtract in `to_local_f32` reds
  the AU-scale collapse pins (2 assertions).
## 8.10 — log-zoom camera + reversed-Z float24 depth (commit beaf6a5)

- A logarithmic depth mapping **cannot live in a `mat4`** (clip `z/w` is linear
  in view space). The engine therefore pairs
  (1) `perspective_logzoom` = off-axis x/y/w exactly as every other builder +
  the `reversed_inf_far` z row — hardware z is then the hyperbolic `n/d`,
  already correctly ordered, a sane pre-refinement fallback; and
  (2) the ONE sanctioned GLSL refinement:
  `gl_FragDepth = clamp((log2(f) + log2(gl_FragCoord.w)) * inv_log2_span, 0, 1)`.
- **GLSL window-coord trap:** `gl_FragCoord.w` is `1/w_clip`, NOT `w_clip`.
  For these pinhole builders `w_clip = -z_view = d`, so `log2(d) =
  -log2(FragCoord.w)` and the sign in front of it is PLUS. The first pass of
  the test shipped the minus sign: the depth gradient inverted, near/far edges
  swapped in the golden and the readback flipped sign at 2^37 m — the pinned
  color-vs-depth contradiction caught it ("C is green left of D" failed at
  (8,24)). Pinned so the trap can never resurface silently.
- Orientation stays **reversed: near -> 1, far -> 0** — pairs the existing
  GREATER compare + depth-clear-to-0 of 3.3. Host mirror
  `logzoom_ndc_z(d,n,f) = (log2 f - log2 d) / log2(f/n)`; resolution is
  constant RELATIVE: one octave = 1/log2(f/n) of the [0,1] budget EVERYWHERE.
  At AU (d ~ 1.37e11 m, span 38 octaves) the hyperbolic hw z puts one octave
  at ~3.6e-12 (~4 f32 ulp); the log map puts it at 0.026 — ~50M times more
  headroom, which is what "near AND far resolve in one frame" runs on.
- Span pins n=1 m, f=2^38 m: 38 octaves; 2^38 exact in float (2^48 would not
  be). Far pair at 2^37 m vs 2^37-2e6 m (2 m separation in front) — the
  z-readback gap is ~3 f32 ulp, resolvable only because the mapping is log.
- **float24+:** `kDepthFormat` is `D32_SFLOAT` since 3.3 (comfortably above
  PLAN's float24 budget); UNORM depth would quantize octaves uniformly and is
  disallowed for this pass. The test asserts the attachment format directly.
- **VR off-axis rule survives untouched:** `perspective_logzoom` reuses the
  off-axis builder, so log depth cannot perturb x/y. Tier-1 pins all four
  asymmetric frustum edges + center ray + x/y rows independent of the z row;
  Tier-2 renders an asymmetric frustum (l/r/b/t = -0.75/0.45/-0.4/0.6) and
  predicts every quad's screen box from the 1.6 x/y map.
- `gfx/camera.cpp` still selects `perspective_offaxis` per eye: the frustum
  SWITCH to a log-zoom range rides 8.11, where the real-scale slice world
  first supplies a real AU frustum. 8.10 pins the math + GPU contract named
  by the checklist.
- **Geometry pin trap (cost a debugging pass):** the debug VS square strip's
  corner multipliers must be +-1 when `qa.xy` carries the half-extent; a +-0.5
  set silently halves every quad and every predicted pixel box misses.
  Cross-checked golden pixel boxes against hand-computed tangent->px
  predictions (PIL) before accepting the golden; determinism via
  byte-identical regeneration (cmp).
- Mutations: plus->minus in the shader reds `depth_zfight_golden`; host-
  mirror sign flip reds `logzoom_projection`; deleting the refinement
  entirely (hyperbolic hw z only) reds the z-value pins — each pins one half
  of the matrix+shader pair.

## 8.11 — real-scale slice world (commit 5faa8c1)

**The fixture.** `world/realscale/sable_reach/` is the project's first world
authored at REAL scale (#17c): Thalassa (Earth-class root, 1.0 AU from its
star by implication, 6371 km radius, 8.87 m/s², day spin) with Ferrous
(Mars-class) at a true **1.52 AU** separation on a 400,000-day orbit, Pale
(Moon-class, airless, 384,400 km around Thalassa, 28-day orbit) and Haven
(rail-pinned station, 620 km above Thalassa's surface, 5400 s period,
`[[station]]` + build plans + pads + outpost POI `poi:rustflare` hosting the
second market node #20a). Sable Reach carries the full world-graph payload
(2 POIs with terrain stamps, 2 biomes, 2 market nodes, lane, 2 nav edges,
waypoint, faction, build plan with 2 steps, 2 job steps) so 8.12–8.16 and
8.21 have one fixture that exercises everything at the hard end of the scale
range. The three existing synthetic worlds stay — they are the small-numbers
fixtures where arithmetic stays eyeballable.

**Wells stay bounded (#22a).** Real radii, real separations, but wells are
still authored multiples of body radius: Thalassa 637,100 km = 100×,
Ferrous 339,000 km ≈ 100×, Pale 172,980 km ≈ 50× — all inside the ratified
10..100 band, asserted in-test. This is why AU-scale separations are
representable without touching the well model: nesting is a per-body
authority radius, not a gravitational domain.

**"rails eval fp64-checked at AU scale" — hand-pinned, not self-referential.**
Every pinned rail value was computed *before* the test ran, by a Python port
of `core/src/rails.cpp` that mirrors the engine's exact operation order:
`theta = ph + TAU*(sec/P)`, `s=sin, c=cos`,
`x = c*R + (-s*R)`, `z = -(s*R) + (-c*R)` (two separate products then one
addition each — NOT `(c±s)*R`), velocity from two full positions at t and
t±1 tick times `(p1-p0)·(0.5·hz)` (60 Hz is exactly representable so the
factor is bit-exact), and the left-to-right Hamilton association of
`quat.h`'s `operator*` verbatim. The values come out bit-exact for the
clean cases (Ferrous at t=0 with ph=30° lands `cos = 0.8660254037844387`
bit-identically through `sin(x+π/2)` — the x=0 and y=0 components are exact
zero pins, and the fp32 ulp at 1.52 AU is exactly 2^-9 m so `+0.0f` rounding
is pinned too). Position/velocity at quarter-ticks are bit-exact because the
angle sums land on exact π/2 multiples (`TAU*0.25` etc. round correctly).

**Tidal lock is bit-exact.** Moon-class spin 12.857142857142858 deg/day =
360/28 over the 28-day orbit: through the ratified `(pi/180)/86400` factor
that is *the same double* as `tau/P`, so `spin*sec` and `TAU*(sec/P)`
evaluate identically at every tick — at Pale's quarter-orbit tick
(36,288,000 ticks = 604,800.0 s = exactly P/4) both angles are bit-equal
(diff 0.0) and Pale's yaw quaternion exactly equals its orbit frame: the
moon faces the parent with zero numerical drift, forever, and the test
asserts that equality, not an approximation.

**Station orientation = parent chain composition, first tick.** Haven's own
spin is zero, so its orientation is the composed identity through
{Thalassa, Haven} — the test pins it equals Thalassa's orientation
bit-for-bit at tick 1, pinning the chain order in `rail_world::orientation`
(root-first accumulation) as a checked contract, not an accident.

**fp32 lesson re-proven, now at 1.52 AU.** The fp32 ulp at Ferrous's
distance is 16384·1024 m (16.384 km) — 2.15e6× worse than the 8.9 AU pin.
Naive float subtraction of the separation leaves zero metres of detail;
`to_local_f32` (fp64 subtract first) keeps the exact 2 m gap. This is the
#17c motivation stated as a number.

**Cook-loop evidence.** The system def's manifest deps are exactly its four
member refs (parent dedup works at world scale: Ferrous and Pale both point
at Thalassa, Haven too, one edge each); `poi:rustflare` stays out of the
file graph (8.8 rule); `[[build_plan]] module=` and waypoint `body=` resolve
to file paths through `resolve_ref`; `[[world_ref]]` chains world→world and
the validator walks it (a dangling world ref reddens the graph gate). The
WORLD0 payload survives an encode/decode round trip with every unit-string
value landing at its exact SI double (1.52 AU cook = `1.52 * kMeters_per_AU`
bit-exact via the parser's divide-by-exact-reciprocal path, verified
against the same C++ expression the test uses for its pins).

**Mutations (chain in /tmp/ve811_mut.py, evidence kept):** dropping the
authored orbit phase reddens 19 assertions (the 30°-phase pins are load
bearing); doubling the velocity factor (0.5→1.0) reddens 8; multiplying the
spin angle by 1.0000001 — a 1e-7 relative drift too small to matter
physically — reddens 4 (quarter-tick exactness + tidal-lock equality). The
rail pins are tight to the last ulp by construction.

## 8.12 — FTL drive (commit 61b9a47)

**Shape (PLAN #22b/#22d to the letter).** `drive.ftl` is a component; its attrs are
`ftl.speed_mult` / `ftl.spool_s` / `ftl.flow` — the core holds the mechanics, not the names:
`core/sim/ftl.h` is a pure state machine (`ftl_drive` aggregate value + `ftl_state` +
`ftl_burning` drain gate), and the integrator gains one parameter:
`integrate_step_warp(_dt)` advances position at `vel * speed_mult` inside the SAME
semi-implicit Euler body — warp folds the *step*, not the state; `vel` stays physical
sublight momentum. `speed_mult == 1.0` is bit-identical to pre-8.12 (x*1.0 is exact IEEE),
pinned by a 20,000-tick randomized flight memcmp-equal through both paths. No second space, no gates.

**Anchored analytic ramp, not an accumulator.** `mult(t) = anchor_mult ± ramp_rate *
tick_to_seconds(t - anchor_tick)`. `tick_to_seconds` divides by 60.0 — a correctly-rounded
divide is exact-by-construction for every integer tick — while multiplying by
`kSim_tick_dt = fl(1/60)` is a *different fp64 value* at some ticks. The two forms agree
suspiciously often (k=30..240 at the natural ramp pins all agree; only ~8% of k in
1..1200 differ, first at k=23, 31, 46...) — so the k=23-elapsed up-ramp pin `{123,
1.0 + 3.0*(23.0/60.0)}` is what makes the divide form LOAD-BEARING: MUT1 (swap in the
single-product dt multiply) reddens exactly that pin. Discovered the hard way: a poisoned
stale `.o` (see traps below) produced the dt-multiply result 2.1500000000000004-vs-
2.1499999999999999 and the pin caught it before any test could bless the wrong form.

**fp64 pin doctrine (same as 8.11 rails).** Decimal literals round the wrong way at the
seams: the down-ramp at 239 elapsed ticks is `13 - 3*fl(239/60) = 1050000000000001×2⁻⁵⁰
≈ 1.0500000000000007`, NOT decimal 1.05; the up-ramp at the same elapsed tick lands
exactly on 12.95 only because fl(239/60) rounds UP. Where literal and expression disagree,
pin the expression. Full-length ramps clamp BIT-exactly on their flip tick (13.0 and 1.0);
an interrupted ramp lands `mult == 1` at the symmetric tick because the identical fp64
product appears on both sides of `anchor_mult - ramp_rate*elapsed`.

**Live-aggregate survival ("dark-not-dead").** The drive aggregate is recomputed every
tick from stats; a function-zeroed or removed drive mid-ramp goes idle INSTANTLY
(mult := 1, no fabricated down-ramp) — the flip branches never compare against or copy a
dead target, so a `warped -> spooling_down -> warped` ping-pong on a changing aggregate
is structurally impossible. `ftl_engage` refuses on an invalid aggregate (refusing is
game-side policy; the core just can't ramp to a nonexistent ceiling).

**Drain (#19e, 6.11 doctrine, zero stored burn state).** Per tick the glue asks
`ftl_burning(state)` AFTER stepping the machine (the tick just taken is what burns), and
writes the gated load attr: `flat_add(0)` then `mod(replace, ftl.flow*mult)` while
burning, `flat_add(0)` alone otherwise (removes this tick's flow modifier at the same
source so the base returns — source-keyed mod, never a stale value). The reservoir
integrates exactly what it is shown. Bit-exact by construction: 120 kg/warp-s ×
kSim_tick_dt = 2.0 kg/tick, and a 300-engage / 900-door cycle drains 840 ticks × 2.0 =
1680.0 kg — every intermediate (spool ramp paid at tick 301, door tick 900 paid, 1140
idle dry) pinned with `==`. "Burning" covers both spool directions: the ramp itself is
paid for.

**Replay/preview ride the command stream.** `tick_command` gains `speed_mult` (default
1.0 — every pre-8.12 log replays bit-identical, and the 8.11 `realscale_world` goldens
passed untouched). Warp legs replay, scrub, and preview from recorded commands alone,
never from live drive state: mutating one recorded mult by (1+1e-6) diverges the leg
(~1.6e-3 m, clears the ~1.1e-6 m position ulp at 5e9 m — a 1-ULP nudge is provably
invisible at that scale, which is itself pinned doctrine); the divisor-1 ghost preview is
BIT-identical to the fine roll through a warp leg, and divisor 4 lands at the same fp64
endpoint (trajectory drift mid-leg is 8.14's shaping gate, endpoint identity is the
integrator's).

**Mutation-harness trap (cost a phantom failure).** Restoring a mutated source with
`cp -p`/`shutil.move` preserves the BACKUP's older mtime, so ninja treats the .o built
from the mutant as fresh — the library archive stays poisoned and later "clean" rebuilds
keep linking the mutant. Symptom: a bit-pin "fails" against the supposedly-unmutated
engine with off-by-1-ulp values that exactly match the mutation's form. Rule: always
bump the mtime after any mutation restore (harness now `os.utime`s), and treat an
unexpected bit-difference as a poisoned-build suspect first (verify via
`objdump -dr` on the suspect .o).

**Evidence.** Targeted `ftl_spool` + `ftl_flow`: 2 cases / 3,464 assertions green. Four
mutations killed (fail counts 1/630/7/5): ramp->single-product-dt-multiply — killed by
exactly ONE pin, the k=23-elapsed one (all other ramp pins are bit-equal under both
forms; that single pin is what makes the divide load-bearing); collapse-guard removal
(dark drive keeps burning); integrator mult dropped (flow end_z port + warp distance);
replay mult ignored (canary + determinism). Clean restore green, zero markers. Full
suite 169 cases / 8,315,573 assertions; CI: see checklist box.


## 8.13 — authored safe bubbles (decision #22c amended by #50)

**#50 semantics (user decision 2026-11-13).** The bubble edge is a HARD WALL
at warp. While `speed_mult > 1`, any tick whose position step would touch or
cross a bubble surface resolves AT the surface: position truncates to the TOI,
or stays pinned BIT-IDENTICAL at the surface when the step starts there (u==0
re-pin returns `p0` verbatim — never re-derived via the centre, which would
reintroduce fp noise). The wall holds EVERY warp tick until `speed_mult`
returns to 1: the ship stops dead at the edge while the forced spool-down
(the 8.12 disengage door) runs; momentum carries inward only sublight.
Outward crossings resolve symmetrically with the exit root. A step that
starts inside AND ends inside (or outside→outside without touching) is a
non-event. An exact tangent graze (disc == 0 at the closest approach) is NOT
a contact — measure-zero rule, shared with Phase 7 swept contacts.

**Core split.** `core/sim/safe_bubble.h` holds only deterministic predicates
and the contact resolver: `in_safe_bubble` (strict interior — d == r is NOT
interior, so the engage door is open AT the surface) and `bubble_warp_contact`
(event: tick, body, u, TOI pos, outward normal, step length). The engage door
is glue composition (`engage only when !in_safe_bubble`) — pinned in the
interior test against `ftl.h`'s state machine; core holds no live FTL policy.
Impact damage VALUE is game-side only (`WorldDef::safe_impact`, negative
`flat_add` on hull, once per crossing at the FIRST contact tick — the u==0
re-pin is the same crossing, glue dedupes); it never reaches `body_def`.

**Pinned quadratic.** Contact math is the quarter-coefficient form, one
correctly-rounded op per step: `a = dot(disp,disp)`, `bq = dot(rel,disp)`,
`cq = dot(rel,rel) - r*r`, roots `(-bq ± sqrt(bq*bq - a*cq)) / a`. Tests port
the exact order; do not "simplify" (factoring changes bits, 8.12 doctrine).

**Surface band (#50's fp hazard).** Truncation lands the next tick's start
within an ulp of the sphere: without banding, an inside-by-1ulp residue takes
the EXIT root and jumps the pinned ship by 2r. Classification only:
`band = r*r*0x1p-40`; `f(0)`/`f(1)` snap to 0 inside the band. The band sits
~2^12 above worst-case f evaluation noise and ~2^12 below any physically
meaningful clearance — deterministic both ways. Root math stays raw.

**Dyadic pin fixture.** r = 1024 m, entry from z = 2048 at 1200 m/step:
u1 = fl(64/75) = 0.8533333333333334 and the TOI z' == 1024.0 BIT-EXACTLY;
outward exit u2 = fl(32/75) = 0.4266666666666667, also lands exactly on the
surface. Then per-tick engine-vs-port vector equality kills any drift.

**Def pipeline.** `body_def.safe_bubble_radius` (0 = no wall) must be
`>= max(well_radius, surface_radius)` and finite (`add_body` gate). Cook:
`[safe]` table — `bubble` is a #42 unit string; `speed` and `impact` are BARE
SI numbers (the ratified #42 suffix table has no velocity dim — `speed`
follows the `surface_g = 9.81` precedent). Validator ladder (fixed order,
first failure): fields >= 0 → bubble without speed → bubble inside well →
bubble inside surface. WRLD body payload gained the safe trio → `kCookFormat`
6 → 7 (no on-disk WRLD goldens to re-bake; the v7 trio rides the realscale
Thalassa fixture round-trip).

**Evidence.** Targeted `safe_bubble_interior` + `safe_bubble_crossing`:
2 cases / 70 assertions green. Targeted cook+core combo
(`safe_bubble_*,worlddef_validate_safe,realscale_world`): 10 cases /
771 assertions green. Six mutations killed (fail counts 6/1/22/1/1/4):
band removal, entry-root sign flip, TOI-pin→step-start in the replay hook,
`&&`→`||` in the add_body geometry gate, well-vs-surface swap in the
validator gate, and `bare`→`unit` on `[safe].speed`. MUT1 initially
SURVIVED — the dyadic r=1024 fixture lands the TOI exactly on the surface,
so band removal was unobservable there; the discriminating fixture is a
non-dyadic entry (z0 = 1629.5) that truncates 1 ulp INSIDE. MUT4 initially
SURVIVED — the rejecting fixture failed an UNRELATED gate (missing
`well_taper_start`), masking the mutation; a mutation probe must isolate
the gate it targets. Full suite 172 cases / 8,315,691 assertions green; ci_local GREEN, Total Test time 362.04 s (no display, ASan+UBSan).

## 8.14 — first scripted warp leg at real system scale (test-only, no new engine code)

`tests/core/test_warp_leg_scenario.cpp` runs Thalassa→Ferrous (1.52 AU fixture
separation, 8.11 world) end-to-end on shipped machinery only: rail bodies +
#50 bubbles + ftl machine + reservoir/stats glue + the 8.13 replay wall hook.
Done-when (a) evidence. No core/gfx/game file changed — 8.13's machinery
already covers every leg stage.

**Pin authority: `tests/scenarios/port_warp_leg.py`** — an op-for-op fp64
Python port (rails, well-blend total_accel with 0.25 dominance band, the
warp half-step `vel += a·dt; pos += (vel·mult)·dt`, ftl ramps + anchors,
reservoir closed form, Fnv1a64 over the same state_hash, replay wall hook
with the quarter-coefficient roots + 2⁻⁴⁰ band + strict inside check,
ascending-body scan, first hit wins). Pins come from the port; the
interval-hash literal (0xB4305E2D05418068) matching bit-identically is the
proof the port mirrored the C++ ops exactly. Keep the port next to the test:
when engine math changes, re-run the port first and read the drift there.

**Pinned leg:** sublight climb 17,312 ticks → door opens tick 17313
(first tick strictly outside the departure bubble) → 600-tick spool ramp →
cruise at mult 5000 (16.67 Mm/tick chords) → arrival-bubble crossing tick
31391 (forced spool-down 600 ticks, hull.hp flat_add −(v·mult−v)²×1e-5 via
the stats pipeline, #50c) → wall hold 1000 ticks → arrival predicate tick
32324. Reservoir lands 35,000 − 17,400,000 propellant-seconds exactly
(cruise load 1000 × 17,400 warp-s + closed-form ramp integral
(s⁻¹·(m−1)²/2), hand-derived in the test). Replay-hash interval over all
32,325 ticks pinned twice-equal.

**Counterfactual (blind engage, tick 10000 deep inside the departure
bubble):** the ftl machine accepts — enforcement lives in the wall layer,
not the drive (#50a) — and the exit-root truncation pins the ship ON the
departure wall from first contact (port pin: tick 10042) through full
mult 5000; no warp passage ever; the leg can only end by sublight
disengage. `b.wall == 10042` pinned.

**Ghost preview across the leg (8.12's hook proven):** divisor-1 windows
before/after crossing reproduce the recorded trajectory BIT-EXACT (the
coarse dt = kSim_tick_dt·D is the canonical tick multiple, so rounding
matches the recorder); divisor-2 coarse steps (D·kSim_tick_dt is NOT a
canonical multiple — deliberate) stay on-track to ulp accumulation
(<0.1 m over 700 coarse steps at 2e11 m — 8 orders below one 16.7 Mm
warp step; the 0.25 band makes gravity bitwise-identical at every leg
sample, so divergence is pure double-rounding).

**Gotchas this box paid for:**
- doctest "Expression Too Complex": any CHECK whose operand mixes
  `a > b && a < c` or a temporary `std::optional<T>(0)` comparison fails
  the static assert — hoist to a named bool / split comparisons.
- Loop-break patterns inside doctest loops must be sentinel-guarded
  (`bad = SIZE_MAX; if (...) { bad = t; break; } CHECK(bad == SIZE_MAX)`)
  — a bare CHECK inside a break-path skips silently.
- Coarse ghost step k lands on live tick `start + D·k + 1` (last tick it
  consumed), not `start + D·k` — off-by-one when diffing against recorded
  states.
- Never pin bit-equality ACROSS implementations for distance: the port's
  `math.dist` is correctly-rounded, C++ `length()` is `sqrt(fl(x²))`;
  cross-impl distance checks ride a tolerance, hash pins carry the exact
  fidelity proof.
- doctest `-tc=` matches whole case names; partial names match nothing
  (silent 0-run looked like a green suite).

**8.14.5 — two ladder survivors, closed at their owners (ac4f8c8).**
The mutation ladder (wall hook off / band 2⁻³⁰ / ramp +0.1% / reservoir dt
+0.01% / preview clamp / dominance band 0.5) killed 4/6 through the
scenario; the 2⁻⁴⁰ surface band and the 0.25 dominance default SURVIVED —
the leg's geometry never samples either discriminating window (its fp-slip
residue is one ulp, inside every band up to 2⁻³⁰; its weak well is at
ratio ~0.1 or ~1.0, outside the blend-vs-suppress corridor). Pinning a
property at a sample that can't distinguish it is theatre — the pins went
where they bite instead:
- `safe_bubble_crossing` subcase "band window": 350 pm outside the wall
  (|f0| = 7.17e-7 ≤ r²·2⁻⁴⁰ = 9.54e-7 → f0 := 0 → u=0 wall contact, pos
  unchanged) vs 700 pm outside (|f0| = 1.43e-6 > band → raw start-outside,
  outward entry root ≤ 0 → no contact). Any band narrower than 2⁻⁴⁰ flips
  the first; any band ≥ 2⁻³⁹ flips the second. Both KILL the 2⁻³⁰ mutation.
- new `sim_well_blend_dominance_band` case: on the planet→moon line at
  9500 m, moon/planet well ratio = 0.5830 ∈ (0.5, 0.75): the 0.25 default
  suppresses the moon to weight EXACTLY 0 (smoothstep clamp at the −0.667
  ramp foot), w_sum = 1 → total_accel BIT-IDENTICAL to the planet's raw
  accel; any default in (0.25, 0.583] gives the moon weight > 0 and the
  equality check dies. Hand-pinned values: planet g = 2.3450788880540956
  (u = 7500/11000 mid-ramp fp64 op-order), moon g = 1.3671875 (u = 3/8,
  exact binary).

Evidence (all headless, ASan+UBSan): targeted `warp_leg_scenario` 1 case /
32,388 assertions; new pins green before/against mutation split verified by
attr re-run (both rc=1 KILLED); full suite `./tools/ci_local.sh` GREEN
359.13 s — 174 test cases / 8,348,093 assertions, 0 failed, replay goldens
untouched (zero golden drift after 8.14).

**8.15 — time compression + physics LOD (decision #53).** World-time
compression is *integer tick multiples*: a far body integrates once every
m ticks, each coarse step advancing sim time by exactly the elapsed tick
gap since that body's last step — a rational multiple of the canonical
tick riding 4.10's authorized variable-dt path (`integrate_step_warp_dt`),
never a free-float dt, never skipped sim time. Bands are observer-relative
(#53a): plain distance thresholds re-evaluated EVERY tick through the
hysteresis-free threshold-band contract (first match wins, no match =
divisor 1); the payload is an integer step divisor 1..1000, not a tag.
A coarse body samples gravity/assists/warp-mult ONCE at the interval-start
tick (#53b) — the same recorded-command spine + shared
`resolve_warp_bubble_wall` helper that `replay_step` uses, so live,
replay, and scrub agree on the wall bit-for-bit.

The lattice is PURE INTEGER state and, decisively, DERIVED: live run,
determinism re-run, and scrubber each rebuild a body's phase in closed
form from the RECORDED divisor schedule (`lod_lattice_at`) — transitions
apply at their tick BEFORE that tick's due test, and a divisor drop
resumes full rate on the switch tick itself, catching the whole elapsed
gap in one authorized long step. Nothing mutates lattice state out of
schedule order, which is why scrub hashes equal live-compressed hashes
bit-for-bit at every tick (#53c) — the schedule equality operator makes
"same run" an integer fact, not a float comparison. Pin 1 guards against
a second physics implementation: divisor 1 through `lod_replay_step` is
BIT-FOR-BIT `replay_step`.

Evidence (all headless, ASan+UBSan): `lod_divisor1_equals_replay_step_bitwise`
20k ticks hashes memcmp-equal vs the reference spine;
`lod_lattice_schedule_closed_form` walks a 100→1000→1 schedule and pins
the closed-form phase at every tick incl. the 5310 catch-up stride;
`lod_band_contract` pins validator + first-match + no-match→1;
`time_compression_lod` flies the 8.14 Thalassa→Ferrous leg with the
observer 1e9 m / divisor 1000 — schedule exactly {17090→1000, 31090→1},
arrival 31090 vs reference 29900 (1190-tick gap, well inside the ±3000
pin), the far leg's 13,999 ticks covered by 14 coarse steps
(17,105 total physics steps vs 29,901 full rate — 99.9% of far-leg
physics skipped), crossing pinned at the 4.0e7 m wall (retained position
NEVER inside while warp is engaged; sublight entry after the forced
disengage is legal), bounded error pins at ±2000/±3000 ticks, and the scrubber replays the
compressed capture bit-for-bit at every sampled tick (step_frame keeps
lattice phase; rebuilt-from-zero scrub costs < half the full-rate steps);
`compression_event_order` runs the compressed leg twice — identical hash
vectors, schedules, and full event streams (order + seq), nav stats change
only on step ticks while `warp.live_mult` fires on physics-FROZEN ticks
inside the far interval, and the reservoir stays tick-dense: tank
divergence is exactly the FTL flow over the bounded crossing offset
(39.7 kg over 1190 ticks). Full gate `./tools/ci_local.sh` GREEN
386.45 s — 179 test cases / 8,518,131 assertions, 0 failed, replay
goldens untouched (zero golden drift after 8.15).
